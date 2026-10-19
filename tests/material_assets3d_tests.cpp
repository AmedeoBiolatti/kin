#include <kin/ecs/scene3d.hpp>
#include <kin/assets/content.hpp>
#include <kin/platform/app.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/scene/scene_session3d.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using namespace kin;
void check(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
struct Scratch {
    std::filesystem::path path=std::filesystem::temp_directory_path()/
        ("kin-materials-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Scratch() { std::filesystem::create_directories(path); }
    ~Scratch() { std::error_code error;std::filesystem::remove_all(path,error); }
};
void write(const std::filesystem::path& path,std::string_view text) {
    std::ofstream(path,std::ios::binary)<<text;
    static int revision=0;
    std::filesystem::last_write_time(path,std::filesystem::file_time_type::clock::now()+std::chrono::seconds(++revision));
}
std::string read(const std::filesystem::path& path) {
    std::ifstream stream(path,std::ios::binary);
    return {std::istreambuf_iterator<char>(stream),std::istreambuf_iterator<char>()};
}
constexpr auto materials=R"({"schema":"kin.materials3d/1","material":{
  "color":[255,0,0,255],"unlit":true,"texture":null,"normal":null,"metallic_roughness":null,"occlusion":null,
  "roughness":0.75},"variants":{
  "green":{"material":{"color":[0,255,0,255],"metallic":0.6}},
  "ghost":{"material":{"color":[0,120,255,128],"blend":"alpha","casts_shadow":false}},
  "mask":{"material":{"blend":"mask","alpha_cutoff":0.4}},
  "glow":{"material":{"emissive":2,"blend":"additive"}}}})";
constexpr auto prefab=R"({"schema":"kin.prefab/1","id":"quad","root":"root","entities":[
  {"id":"root","components":{"Transform3D":{},"ModelRenderer3D":{"asset":"shared-a.gltf"},
   "ModelMaterials3D":{"asset":"colors.kinmaterials"}}}]})";
constexpr auto scene=R"({"schema":"kin.scene/1","entities":[
  {"id":"camera","components":{"Transform3D":{"position":[0,0,5]},"SceneCamera3D":{"fov":60}}},
  {"id":"environment","components":{"SceneEnvironment3D":{"samples":1}}}],"prefabs":[
  {"id":"left","asset":"quad.kinprefab","overrides":{"root":{"Transform3D":{"position":[-1.25,0,0],"scale":[0.8,0.8,0.8]}}}},
  {"id":"right","asset":"quad.kinprefab","overrides":{"root":{"Transform3D":{"position":[1.25,0,0],"scale":[0.8,0.8,0.8]},
   "ModelMaterials3D":{"variant":"green"}}}}]})";

void cpu() {
    const auto parsed=parse_materials3d_asset(materials);
    check(parsed.ok() && parsed.asset->variants.size()==4,"valid material variants rejected");
    check(parsed.asset->base.material.texture && parsed.asset->base.material.texture->asset.empty(),"explicit texture clearing lost");
    for (const auto* text : {
        R"({"schema":"kin.materials3d/1","material":{"shader":12}})",
        R"({"schema":"kin.materials3d/1","material":{"shader_params":[1,2,3]}})",
        R"({"schema":"kin.materials3d/1","material":{"shader_params":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,"bad"]}})",
        R"({"schema":"unknown"})", R"({"schema":"kin.materials3d/1","metallic":0.1})",
        R"({"schema":"kin.materials3d/1","material":{"roughnes":0.1}})",
        R"({"schema":"kin.materials3d/1","material":{"roughness":-0.1}})",
        R"({"schema":"kin.materials3d/1","material":{"metallic":1.1}})",
        R"({"schema":"kin.materials3d/1","material":{"color":[256,0,0,255]}})",
        R"({"schema":"kin.materials3d/1","material":{"unlit":1}})",
        R"({"schema":"kin.materials3d/1","material":{"blend":"multiply"}})",
        R"({"schema":"kin.materials3d/1","material":{"wrap_u":"invalid"}})",
        R"({"schema":"kin.materials3d/1","material":{"texture":{"asset":"a.png","filter":"invalid"}}})",
        R"({"schema":"kin.materials3d/1","parts":[{"part":-1,"material":{}}]})",
        R"({"schema":"kin.materials3d/1","parts":[{"part":0.5,"material":{}}]})",
        R"({"schema":"kin.materials3d/1","parts":[{"part":0,"material":{}},{"part":0,"material":{}}]})"})
        check(!parse_materials3d_asset(text).ok(),"invalid material source accepted");
    check(!parse_materials3d_asset(std::string(1024*1024+1,' ')).ok(),"material source budget ignored");
    const auto shader=parse_materials3d_asset(R"({"schema":"kin.materials3d/1","material":{"shader":"glass.spv","shader_params":[1.45,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]},"variants":{"plain":{"material":{"shader":null}}}})");
    check(shader.ok() && shader.asset->base.material.shader==std::filesystem::path{"glass.spv"} &&
        (*shader.asset->base.material.shader_params)[0]==1.45f && shader.asset->variants.at("plain").material.shader->empty(),"shader parameters or explicit clearing lost");
    Scratch scratch;write(scratch.path/"colors.kinmaterials",materials);write(scratch.path/"quad.kinprefab",prefab);
    kin::EcsWorld world;register_builtin_editor_components(world);
    auto loaded=parse_scene_asset(scene,world.components());check(loaded.ok(),"shared scene codec rejected material component");
    auto instance=instantiate_scene_asset(world,*loaded.scene,scratch.path,world.components());check(instance.ok(),"material prefab instances failed");
    auto left=instance.by_authored_id.at("left"),right=instance.by_authored_id.at("right");
    check(left.get<ModelMaterials3D>()->variant.empty() && right.get<ModelMaterials3D>()->variant=="green","prefab variant override leaked");
    check(world.components().patch_field(right,"ModelMaterials3D","variant",std::string("ghost")),"material variant field patch failed");
    auto saved=build_scene_asset(world);
    check(saved.ok() && saved.scene->entities.size()==2 && saved.scene->prefabs.size()==2,"material save flattened prefab links");
    kin::EcsWorld copy;register_builtin_editor_components(copy);
    auto restored=instantiate_scene_asset(copy,*saved.scene,scratch.path,copy.components());
    check(restored.ok() && restored.by_authored_id.at("right").get<ModelMaterials3D>()->variant=="ghost" &&
          !restored.by_authored_id.at("right").get<ModelMaterials3D>()->binding,"save serialized runtime binding or lost variant");
}

void material_gpu(Renderer2D& renderer) {
    Scratch scratch;
    for (auto file:{"shared-a.gltf","shared pixel.png","parts.glb"})
        std::filesystem::copy_file(std::filesystem::path(KIN_TEST_MODELS)/file,scratch.path/file);
    write(scratch.path/"colors.kinmaterials",materials);write(scratch.path/"quad.kinprefab",prefab);write(scratch.path/"test.kinscene",scene);
    Renderer3D r3d{renderer};Assets3D assets{r3d,scratch.path};
    SceneSession3DOptions options;options.viewport={160,120};options.view.samples=1;
    SceneSession3D session{r3d,assets,options};
    const auto ok=[&](bool value,const char* message) {
        if (!value) for(const auto& error:session.diagnostics()) std::cerr<<error<<'\n';check(value,message);
    };
    ok(session.load("test.kinscene"),"material scene load failed");
    auto left=session.entity("left"),right=session.entity("right");
    auto model=left.get<ModelRenderer3D>()->model;
    { kin::EcsWorld manual;WorldRenderState3D state{manual};
      manual.entity("plain").set(Transform3D{}).set(ModelRenderer3D{.model=model});
      Scene3D collected;state.propagate_transforms();state.collect(collected);
      check(collected.draws().size()==model->parts.size(),"material-free C++ world collection regressed"); }
    auto red=left.get<ModelMaterials3D>()->binding,green=right.get<ModelMaterials3D>()->binding;
    check(model==right.get<ModelRenderer3D>()->model && assets.stats().model_loads==1,"variants duplicated model geometry");
    check(red && green && red!=green && red->materials[0].color==Color::rgb(255,0,0) && green->materials[0].color==Color::rgb(0,255,0),"variant colors incorrect");
    check(model->parts[0].material.texture && !red->materials[0].texture && red->materials[0].roughness==.75f &&
          green->materials[0].metallic==.6f,"patch changed imported material or lost inheritance");
    check(assets.load_materials("colors.kinmaterials",model).shared()==red,"identical bindings not shared");
    check(!left.raw().target(flecs::IsA).get<ModelMaterials3D>()->binding,"binding mutated prefab prototype");
    renderer.clear();ok(session.render(),"variant render failed");renderer.draw_texture(session.view()->texture(),{0,0,160,120});
    std::vector<u8> pixels;Vec2i size;
    check(renderer.read_rgba({0,0,160,120},pixels,size) && size==Vec2i{160,120},"variant readback failed");
    const auto pixel=[&](int x,int y,int c) { return pixels[(y*160+x)*4+c]; };
    check(pixel(54,60,0)>240 && pixel(54,60,1)<10 && pixel(106,60,1)>240 && pixel(106,60,0)<10,
          "per-instance material colors not rendered");
    renderer.present();
    std::string error;
    const auto ghost=assets.load_materials("colors.kinmaterials",model,"ghost",&error);
    const auto glow=assets.load_materials("colors.kinmaterials",model,"glow",&error);
    const auto mask=assets.load_materials("colors.kinmaterials",model,"mask",&error);
    check(ghost && glow && mask && ghost->materials[0].blend==Blend3D::Alpha && !ghost->materials[0].casts_shadow &&
        mask->materials[0].alpha_cutoff==.4f && mask->materials[0].blend==Blend3D::Mask && glow->materials[0].emissive==2 &&
        glow->materials[0].blend==Blend3D::Additive,"transparency/emissive settings lost");

#ifdef KIN_TEST_MATERIAL_SHADER
    {
        std::filesystem::copy_file(KIN_TEST_MATERIAL_SHADER,scratch.path/"custom.spv");
        write(scratch.path/"shader.kinmaterials",R"({"schema":"kin.materials3d/1","material":{"shader":"custom.spv","shader_params":[0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0]},"variants":{"plain":{"material":{"shader":null}}}})");
        auto shaded=assets.load_materials("shader.kinmaterials",model,{},&error);
        check(shaded && shaded->materials[0].shader && shaded->materials[0].shader_params[5]==1,"shader material did not bind");
        auto shared=assets.load_material_shader("custom.spv",&error);
        check(shared==shaded->materials[0].shader && assets.load_material_shader("./custom.spv")==shared,"shader source aliases not shared");
        const auto plain=assets.load_materials("shader.kinmaterials",model,"plain",&error);
        check(plain && !plain->materials[0].shader,"plain variant did not clear custom shader");
        auto view=r3d.create_view({64,64});Scene3D probe;
        probe.camera=Camera3D::look_at({0,0,3},{0,0,0});
        probe.draw(*model,Mat4f::identity(),colors::white,0,shaded->materials);
        check(r3d.render(view,probe),"custom asset shader render failed");
        {auto target=renderer.create_render_target({64,64});auto bind=renderer.scoped_render_target(target);
         renderer.clear();renderer.draw_texture(view.texture(),{0,0,64,64});std::vector<u8> rgba;Vec2i size;
         check(renderer.read_rgba({0,0,64,64},rgba,size),"custom asset shader readback failed");
         const auto i=(32*64+32)*4;check(rgba[i]<10 && rgba[i+1]>150 && rgba[i+2]<10,"shader_params did not affect actual pixels");}
        const auto old=shaded->materials[0].shader;assets.invalidate("custom.spv");
        auto rebound=assets.load_materials("shader.kinmaterials",model,{},&error);
        check(rebound && rebound.shared()!=shaded.shared() && rebound->materials[0].shader!=old && old,"shader dependency invalidation lost retained handles");
        write(scratch.path/"broken.spv","bad");
        write(scratch.path/"header.spv",std::string(20,'x'));
        const auto entries=assets.stats().shader_entries;
        check(!assets.load_material_shader("broken.spv",&error) && !error.empty() && !assets.load_material_shader("missing.spv",&error) &&
              !assets.load_material_shader("header.spv",&error) &&
              assets.stats().shader_entries==entries,"failed shader cached or missing error");
        Assets3D uncached{r3d,scratch.path,{.shaders=0}};
        const auto one=uncached.load_materials("shader.kinmaterials",model,{},&error);
        check(one && uncached.stats().shader_entries==0 && uncached.stats().shader_loads==1,"disabled shader cache duplicated one binding's shader");
    }
#endif
    // Authored maps reuse imported glTF textures only when role AND sampling match.
    write(scratch.path/"maps.kinmaterials",R"({"schema":"kin.materials3d/1","material":{
        "texture":{"asset":"shared pixel.png","filter":"nearest"},
        "normal":{"asset":"shared pixel.png","filter":"nearest"},
        "occlusion":{"asset":"shared pixel.png","filter":"nearest"}}})");
    auto maps=assets.load_materials("maps.kinmaterials",model,{},&error);
    check(maps && maps->materials[0].texture==model->parts[0].material.texture && maps->materials[0].normal==model->parts[0].material.normal &&
        maps->materials[0].normal==maps->materials[0].occlusion && maps->materials[0].texture!=maps->materials[0].normal,
        "texture sharing lost role/filter semantics");
    auto filtered=assets.load_texture("shared pixel.png",false,true,&error);
    check(filtered && filtered!=maps->materials[0].texture,"different filters incorrectly shared");

    auto parts=assets.load_model("parts.glb").shared();
    write(scratch.path/"parts.kinmaterials",R"({"schema":"kin.materials3d/1","material":{"roughness":0.9},
      "parts":[{"part":0,"material":{"metallic":0.4}}],"variants":{"wet":{"material":{"roughness":0.2},
      "parts":[{"part":1,"material":{"emissive":3}}]}}})");
    auto layered=assets.load_materials("parts.kinmaterials",parts,"wet",&error);
    check(layered && layered->materials.size()==parts->parts.size() && layered->materials[0].metallic==.4f &&
        layered->materials[0].roughness==.2f && layered->materials[1].emissive==3 &&
        layered->materials[2].emissive==parts->parts[2].material.emissive,"per-part layering incorrect");
    { Scene3D direct;const Material3D wrong[]={{}};bool rejected=false;
      rejected = !direct.draw(*parts,{},colors::white,0,wrong);
      check(!direct.diagnostics().empty(),"missing draw rejection diagnostics");
      check(rejected && direct.draws().empty(),"mismatched material array partially submitted a model"); }
    // A unique name resolves to the same index; duplicate names require indices.
    auto named_model=std::make_shared<Model>(*parts);named_model->parts[0].name="unique";
    write(scratch.path/"names.kinmaterials",R"({"schema":"kin.materials3d/1","parts":[{"part":"unique","material":{"metallic":0.8}}]})");
    auto named=assets.load_materials("names.kinmaterials",named_model,{},&error);
    check(named && named->materials[0].metallic==.8f,"unique name selector failed");
    named_model->parts[1].name="unique";assets.invalidate("names.kinmaterials");
    check(!assets.load_materials("names.kinmaterials",named_model,{},&error) && error.find("ambiguous")!=std::string::npos,"ambiguous part name accepted");
    for (const auto* rules:{
        R"({"schema":"kin.materials3d/1","parts":[{"part":99,"material":{}}]})",
        R"({"schema":"kin.materials3d/1","parts":[{"part":"missing","material":{}}]})"}) {
        write(scratch.path/"bad.kinmaterials",rules);assets.invalidate("bad.kinmaterials");
        check(!assets.load_materials("bad.kinmaterials",parts,{},&error) && !error.empty(),"bad selector accepted");
    }
    check(!assets.load_materials("colors.kinmaterials",model,"unknown",&error),"unknown variant accepted");

    // Activated session registry edits validate/bind transactionally. Standalone
    // registry worlds retain their explicit binding contract above.
    const auto original_variant=left.get<ModelMaterials3D>()->variant;
    check(!session.world()->components().patch_field(left,"ModelMaterials3D","variant",std::string("unknown")) &&
        !session.world()->components().last_error().empty() && left.get<ModelMaterials3D>()->variant==original_variant &&
        left.get<ModelMaterials3D>()->binding==red,"invalid live material edit changed reference/binding");
    check(session.world()->components().patch_field(left,"ModelMaterials3D","asset",ComponentAssetRef{}),"clear material edit failed");
    check(!left.get<ModelMaterials3D>()->binding && right.get<ModelMaterials3D>()->binding==green &&
        resolve_materials3d_assets(*session.world(),assets).empty(),
        "clearing overrides mutated sibling");
    ok(session.reload(),"material reload failed");
    auto* old_world=session.world();const auto old_model=session.entity("left").get<ModelRenderer3D>()->model;
    write(scratch.path/"colors.kinmaterials","broken");
    check(session.reload_if_changed()==SceneSession3DReload::Failed && session.world()==old_world,"broken material replaced active scene");
    std::filesystem::remove(scratch.path/"colors.kinmaterials");
    check(session.reload_if_changed()==SceneSession3DReload::Failed && session.world()==old_world,"deleted material replaced active scene");
    write(scratch.path/"colors.kinmaterials",std::string(materials)+"\n");
    check(session.reload_if_changed()==SceneSession3DReload::Activated && session.entity("left").get<ModelRenderer3D>()->model==old_model,
        "material edit reuploaded model or failed recovery");

    std::filesystem::copy_file(scratch.path/"shared pixel.png",scratch.path/"override.png");
    const auto png=read(scratch.path/"override.png");
    write(scratch.path/"colors.kinmaterials",R"({"schema":"kin.materials3d/1","material":{"texture":"override.png"},"variants":{"green":{}}})");
    check(session.reload_if_changed()==SceneSession3DReload::Activated,"map dependency load failed");
    auto previous=session.entity("left").get<ModelMaterials3D>()->binding;
    write(scratch.path/"override.png","corrupt");old_world=session.world();
    check(session.reload_if_changed()==SceneSession3DReload::Failed && session.world()==old_world,"corrupt material texture replaced scene");
    write(scratch.path/"override.png",png);
    check(session.reload_if_changed()==SceneSession3DReload::Activated && session.entity("left").get<ModelRenderer3D>()->model==old_model &&
        session.entity("left").get<ModelMaterials3D>()->binding->materials[0].texture!=previous->materials[0].texture,
        "texture edit did not refresh material independently of geometry");

    session.entity("left").get_mut<ModelRenderer3D>()->model=parts;
    ok(session.collect(),"collection after model replacement failed");
    for (const auto& part:parts->parts) {
        check(std::ranges::any_of(session.scene()->draws(),[&](const auto& draw) {
            return draw.mesh==part.mesh && draw.material==part.material;
        }),"model replacement used stale material indices");
    }
    check(resolve_materials3d_assets(*session.world(),assets).empty() &&
        session.entity("left").get<ModelMaterials3D>()->binding->materials.size()==parts->parts.size(),
        "material binding did not follow replacement model");

    // Shared binding metadata must never pin meshes. A failed model replacement
    // cannot accidentally apply a stale per-part array to the new model.
    std::filesystem::copy_file(scratch.path/"parts.glb",scratch.path/"release.glb");
    { auto isolated=assets.load_model("release.glb").shared();
      auto binding=assets.load_materials("parts.kinmaterials",isolated,"wet").shared();
      std::weak_ptr<IMeshBackend> mesh_weak=isolated->parts[0].mesh.backend();isolated.reset();
      check(binding && binding->model.expired() && mesh_weak.expired(),"material binding/cache pinned model geometry"); }
    previous.reset();red.reset();green.reset();session.unload();assets.trim();
    { Assets3D small{r3d,scratch.path,{.materials=1}};
      auto one=small.load_materials("colors.kinmaterials",model).shared();
      auto two=small.load_materials("colors.kinmaterials",model,"green").shared();
      std::weak_ptr<const MaterialBinding3D> weak=two;
      check(one && two && small.stats().material_entries==1,"material cache exceeded metadata budget");
      one.reset();two.reset();small.trim();
      check(weak.expired() && small.stats().material_entries==0,"material cache retained binding ownership"); }

    Scratch packed;const auto archive=packed.path/"art.kinpak";
    write(scratch.path/"colors.kinmaterials",materials);
    check(write_content_pack(scratch.path,archive).ok,"material pack write failed");
    const auto mount=packed.path/"virtual";mount_content_pack(mount,ContentPack::open(archive));
    { Assets3D packed_assets{r3d,mount};SceneSession3D packed_session{r3d,packed_assets,options};
      check(packed_session.load("test.kinscene"),"packed material references failed");
#ifdef KIN_TEST_MATERIAL_SHADER
      const auto packed_shader=packed_assets.load_materials("shader.kinmaterials",model,{},&error);
      check(packed_shader && packed_shader->materials[0].shader,"packed shader reference failed");
#endif
    }
    unmount_content_pack(mount);

    if (r3d.capabilities().skinning) {
        Assets3D real_assets{r3d,KIN_TEST_SUMMIT};
        SceneSession3D camp{r3d,real_assets,options};
        check(camp.load("scenes/camp.kinscene") && camp.step(1.f/60),"camp material variants failed");
        if (std::getenv("KIN_TEST_MATERIAL_CAPTURE")) check(camp.resize({640,360}),"camp capture resize failed");
        renderer.clear();check(camp.render(),"camp material render failed");
        const bool capture=std::getenv("KIN_TEST_MATERIAL_CAPTURE")!=nullptr;
        renderer.draw_texture(camp.view()->texture(),capture ? Rectf{0,0,640,360} : Rectf{0,0,160,120});
        if (const auto* path=std::getenv("KIN_TEST_MATERIAL_CAPTURE")) check(renderer.save_png(path),"camp material capture failed");
        renderer.present();
        auto survivor=camp.entity("survivor");
        survivor.set(ModelMaterials3D{.asset={(scratch.path/"colors.kinmaterials").string()},.variant="green"});
        check(resolve_materials3d_assets(*camp.world(),real_assets).empty() && camp.collect(),"skinned material binding failed");
        bool green_skin=false;
        for(const auto& draw:camp.scene()->draws()) if(draw.mesh.skinned() && draw.material.color==Color::rgb(0,255,0)) green_skin=true;
        check(green_skin,"skinned draw ignored material override");
    }
}
} // namespace
int main(int argc,char** argv) {
    try { cpu(); } catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
    if(argc>1 && std::string_view(argv[1])=="cpu") return 0;
    bool ready=false;
    try {
        App app{};const bool capture=std::getenv("KIN_TEST_MATERIAL_CAPTURE")!=nullptr;
        auto& window=app.create_window({.title="material-assets3d-tests",.width=capture?640:160,.height=capture?360:120,.hidden=true});
        auto backend=make_render_backend(window,false,true);
        if(!backend || backend->name()!="SDL_GPU") throw std::runtime_error("GPU backend required for material checks");
        ready=true;Renderer2D renderer{std::move(backend)};material_gpu(renderer);
        std::cout<<"Material variants, prefab overrides, texture sharing and reload checks passed\n";
    } catch(const std::exception& error) {
        std::cerr<<error.what()<<'\n';const char* required=std::getenv("KIN_REQUIRE_GPU_TESTS");
        return ready || (required && std::string_view(required)!="0") ? 1 : 77;
    }
}
