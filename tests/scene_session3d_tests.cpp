#include <kin/scene/scene_session3d.hpp>
#include <kin/ecs/system.hpp>
#include <kin/assets/content.hpp>
#include <kin/platform/app.hpp>
#include <kin/renderer/renderer2d.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace kin;
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Scratch {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("kin-session3d-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Scratch() { std::filesystem::create_directories(path); }
    ~Scratch() { std::error_code error; std::filesystem::remove_all(path, error); }
};
void write(const std::filesystem::path& path, std::string_view text) {
    std::ofstream(path, std::ios::binary) << text;
    // File systems can have coarse timestamp resolution. Deliberately advance
    // it for deterministic polling tests without sleeping.
    static int revision = 0;
    std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(++revision));
}
std::string read(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
constexpr auto prefab = R"({"schema":"kin.prefab/1","id":"prop","root":"root","entities":[
  {"id":"root","components":{"Transform3D":{},"ModelRenderer3D":{"asset":"parts.glb"},
  "Collider3D":{"kind":"box","half_extents":[0.5,0.5,0.5]},"RigidBody3D":{"motion":"dynamic"}}}]})";
constexpr auto scene = R"({"schema":"kin.scene/1","entities":[
  {"id":"camera","components":{"Transform3D":{"position":[0,0,8]},"SceneCamera3D":{"fov":60}}},
  {"id":"environment","components":{"SceneEnvironment3D":{"samples":1,"ambient":[1,1,1]}}}],
  "prefabs":[{"id":"prop","asset":"prop.kinprefab"}]})";
constexpr auto textured = R"({"schema":"kin.scene/1","entities":[
  {"id":"quad","components":{"Transform3D":{},"ModelRenderer3D":{"asset":"shared-a.gltf"}}}]})";

void cpu() {
    Scratch scratch; kin::EcsWorld world; register_builtin_editor_components(world);
    write(scratch.path/"prop.kinprefab",prefab); write(scratch.path/"test.kinscene",scene);
    SceneAssetRuntime runtime;
    check(runtime.load(scratch.path/"test.kinscene",scratch.path,world.components(),{4096,3}), "bounded shared scene load failed");
    check(runtime.entity_count()==3, "expanded entity budget is incorrect");
    check(!runtime.load(scratch.path/"test.kinscene",scratch.path,world.components(),{4096,2}), "expanded entity limit ignored");
    check(!runtime.load(scratch.path/"test.kinscene",scratch.path,world.components(),{8,3}), "source byte limit ignored");
    check(!runtime.load(scratch.path/"missing.kinscene",scratch.path/"elsewhere",world.components()), "missing load accepted");
    check(runtime.scene_path()==scratch.path/"test.kinscene" && runtime.asset_root()==scratch.path && runtime.entity_count()==3,
        "failed load replaced previous source metadata");
    check(runtime.instantiate(world), "shared runtime instantiate failed");
    auto root=runtime.instance().by_authored_id.at("prop");
    check(!runtime.instantiate(world) && root.alive(), "repeated instantiate orphaned existing entities");
    std::filesystem::remove(scratch.path/"prop.kinprefab");
    check(runtime.poll_changes()==SceneAssetRuntimeChange::PrefabAsset, "deleted prefab not detected");
    runtime.clear(world); check(!root.alive(), "shared runtime clear failed");
    std::filesystem::remove(scratch.path/"test.kinscene");
    check(runtime.poll_changes()==SceneAssetRuntimeChange::SceneAsset, "deleted scene not detected");
}

void gpu_live_edits(Renderer2D& renderer) {
    Scratch scratch;
    std::filesystem::copy_file(std::filesystem::path(KIN_TEST_MODELS)/"parts.glb",scratch.path/"parts.glb");
    write(scratch.path/"prop.kinprefab",prefab);
    write(scratch.path/"paint.kinmaterials",R"({"schema":"kin.materials3d/1","variants":{"red":{"material":{"color":[255,0,0,255]}},"blue":{"material":{"color":[0,0,255,255]}}}})");
    write(scratch.path/"edits.kinscene",R"({"entities":[
      {"id":"group","components":{"Transform3D":{}}},
      {"id":"child","parent":"group","components":{"Transform3D":{"position":[0,0,-4]},"Collider3D":{"half_extents":[0.5,0.5,0.5]}}},
      {"id":"smoke","components":{"Transform3D":{},"ParticleEmitter3D":{"rate":10,"max_particles":32}}},
      {"id":"smoke2","components":{"Transform3D":{},"ParticleEmitter3D":{"max_particles":32}}},
      {"id":"controller","components":{"Transform3D":{"position":[10,0,0]},"CharacterController3D":{}}},
      {"id":"camera","components":{"Transform3D":{"position":[0,0,8]},"SceneCamera3D":{}}},
      {"id":"environment","components":{"SceneEnvironment3D":{"samples":1}}}],
      "prefabs":[{"id":"left","asset":"prop.kinprefab","overrides":{"root":{"Transform3D":{"position":[-3,3,0]},"ModelMaterials3D":{"asset":"paint.kinmaterials","variant":"red"}}}},
                 {"id":"right","asset":"prop.kinprefab","overrides":{"root":{"Transform3D":{"position":[3,3,0]},"ModelMaterials3D":{"asset":"paint.kinmaterials","variant":"red"}}}}]})");
    Renderer3D r3d{renderer};Assets3D assets{r3d,scratch.path};
    SceneSession3DOptions options;options.viewport={160,120};options.view.samples=1;options.warm_up=false;
    options.physics=World3DConfig{.max_bodies=16,.max_body_pairs=64,.max_contact_constraints=64,.temp_memory_bytes=1024*1024,.single_threaded=true};
    options.particle_limits={4,64,80};options.max_edit_entities=16;options.max_edit_fields=8;
    SceneSession3D session{r3d,assets,options};
    const auto ok=[&](bool result,const char* why){if(!result)for(const auto& e:session.diagnostics())std::cerr<<e<<'\n';check(result,why);};
    ok(session.load("edits.kinscene"),"live edit fixture failed");
    const auto left=session.entity("left"),right=session.entity("right"),smoke=session.entity("smoke"),group=session.entity("group"),child=session.entity("child");
    auto* world=session.world();const auto generation=session.generation();auto* view=session.view()->backend();
    auto& registry=world->components();
    const auto change=[&](kin::EcsEntity e,const char* c,const char* f,ComponentFieldValue v){return registry.patch_field(e,c,f,v);};
    const auto right_body=session.bodies()->body_of(right.raw());
    auto left_body=session.bodies()->body_of(left.raw());
    session.physics()->set_linear_velocity(left_body,{2,1,0});session.physics()->set_angular_velocity(left_body,{0,.4f,0});
    const auto model=left.get<ModelRenderer3D>()->model;
    ok(change(left,"Transform3D","scale",Vec3f{2,1,1}),"registry scale edit failed");
    check(session.world()==world&&session.generation()==generation&&session.view()->backend()==view,"field edit replaced scene/view");
    check(left.get<Transform3D>()->scale==Vec3f{2,1,1}&&right.get<Transform3D>()->scale==Vec3f{1,1,1},"scale edit changed prefab sibling");
    const auto resized=session.bodies()->body_of(left.raw());
    check(resized!=left_body&&!session.physics()->contains(left_body)&&session.physics()->contains(resized),"collider was not replaced immediately");
    check(session.bodies()->body_of(right.raw())==right_body,"unrelated body was rebuilt");
    check(session.physics()->user_id(resized)==left.id(),"replacement body lost active entity identity");
    check(session.physics()->linear_velocity(resized)==Vec3f{2,1,0}&&session.physics()->angular_velocity(resized)==Vec3f{0,.4f,0},"body edit lost velocity");
    check(session.physics()->raycast({-3.8f,5,0},{-3.8f,1,0}).has_value(),"resized collision geometry stayed narrow");
    const auto red=left.get<ModelMaterials3D>()->binding;
    ok(change(left,"ModelMaterials3D","variant",std::string("blue")),"material edit failed");
    check(left.get<ModelMaterials3D>()->binding!=red&&right.get<ModelMaterials3D>()->binding==red&&left.get<ModelRenderer3D>()->model==model,"material edit lost sharing/isolation");
    const auto blue=left.get<ModelMaterials3D>()->binding;
    auto before=world->events().dirty_state().last_event_sequence;
    check(!change(left,"ModelMaterials3D","variant",std::string("missing")),"unknown material variant accepted");
    check(!change(left,"ModelMaterials3D","variant",std::string(65537,'x')),"oversized edit text accepted");
    check(left.get<ModelMaterials3D>()->variant=="blue"&&left.get<ModelMaterials3D>()->binding==blue&&world->events().since(before).empty(),"failed material edit changed data/events");
    check(!change(left,"ModelRenderer3D","asset",ComponentAssetRef{"absent.glb"})&&left.get<ModelRenderer3D>()->model==model&&left.get<ModelRenderer3D>()->asset.id=="parts.glb","bad model edit changed binding/reference");
    const SceneFieldEdit3D invalid_batch[]={{left,"Transform3D","scale",Vec3f{3,3,3}},{smoke,"ParticleEmitter3D","max_particles",u32{65}}};
    check(!session.edit(invalid_batch)&&left.get<Transform3D>()->scale==Vec3f{2,1,1}&&session.bodies()->body_of(left.raw())==resized,"failed batch partially committed");
    check(!change(smoke,"ParticleEmitter3D","max_particles",u32{64+1}),"per-emitter particle cap ignored");
    check(!change(smoke,"ParticleEmitter3D","max_particles",u32{64}),"global particle budget omitted untouched emitters");
    // Individual caps pass, but scene-wide capacity must include untouched smoke2.
    const SceneFieldEdit3D capacity[]={{smoke,"ParticleEmitter3D","max_particles",u32{64}},{session.entity("smoke2"),"ParticleEmitter3D","max_particles",u32{64}}};
    check(!session.edit(capacity)&&smoke.get<ParticleEmitter3D>()->system.desc.max_particles==32,"global particle cap ignored");
    ok(session.step(.1f),"live fixture step failed");
    const auto count=smoke.get<ParticleEmitter3D>()->system.count();check(count>0,"particle fixture did not emit");
    ok(change(smoke,"ParticleEmitter3D","rate",f32{3}),"particle rate edit failed");
    check(smoke.get<ParticleEmitter3D>()->system.count()==count,"particle edit reset living particles");
    const auto saved=build_scene_asset(*world);
    check(saved.ok()&&saved.scene->prefabs.size()==2,"live edits broke linked scene saving");
    ok(change(smoke,"ParticleEmitter3D","emitting",false),"stop emitting failed");
    std::filesystem::copy_file(scratch.path/"paint.kinmaterials",scratch.path/"paint2.kinmaterials");
    ok(change(left,"ModelMaterials3D","asset",ComponentAssetRef{"paint2.kinmaterials"}),"live material reference edit failed");
    write(scratch.path/"paint2.kinmaterials",read(scratch.path/"paint2.kinmaterials")+"\n");
    ok(change(smoke,"ParticleEmitter3D","rate",f32{2}),"unrelated particle edit failed");
    check(session.changed_sources()==std::vector<std::filesystem::path>{scratch.path/"paint2.kinmaterials"},"live watch missing or unrelated source change acknowledged");
    check(!change(smoke,"ParticleEmitter3D","texture",ComponentAssetRef{"missing.png"})&&smoke.get<ParticleEmitter3D>()->texture_asset.id.empty(),"bad particle texture changed reference");
    const auto child_body=session.bodies()->body_of(child.raw());
    ok(change(group,"Transform3D","scale",Vec3f{2,2,2}),"parent transform edit failed");
    check(session.bodies()->body_of(child.raw())!=child_body&&session.physics()->raycast({.8f,3,-8},{.8f,-3,-8}).has_value(),"descendant collider did not follow parent edit");
    check(!change(group,"Transform3D","scale",Vec3f{-1,1,1}),"negative transform scale accepted");
    const auto controller=session.entity("controller");const auto character=session.bodies()->character_of(controller.raw());
    check(!change(controller,"CharacterController3D","radius",f32{.4f})&&session.bodies()->character_of(controller.raw())==character,"unsupported live controller edit was applied");
    auto camera=session.entity("camera");
    check(!change(camera,"SceneCamera3D","near",f32{2000})&&camera.get<SceneCamera3D>()->camera.z_near<1,"invalid camera range committed");
    const SceneFieldEdit3D camera_range[]={{camera,"SceneCamera3D","near",f32{2000}},{camera,"SceneCamera3D","far",f32{3000}}};
    ok(session.edit(camera_range),"valid cross-field camera batch rejected");
    const auto environment=session.entity("environment");
    check(!change(environment,"SceneEnvironment3D","samples",u32{3})&&session.view()->backend()==view,"bad view edit committed");
    ok(change(environment,"SceneEnvironment3D","samples",u32{2}),"view edit failed");
    check(session.view()->backend()!=view,"view edit did not prepare replacement");
    // A later failed body in the same batch must discard earlier candidates.
    const auto body_before=session.bodies()->body_of(left.raw());const auto live_count=session.physics()->body_count();
    const SceneFieldEdit3D bad_bodies[]={{left,"Collider3D","half_extents",Vec3f{1,1,1}},{right,"Collider3D","radius",f32{-1}},{right,"Collider3D","kind",std::string("sphere")}};
    check(!session.edit(bad_bodies)&&session.bodies()->body_of(left.raw())==body_before&&session.physics()->body_count()==live_count,"body failure leaked/replaced candidates");
    const auto constraint=session.physics()->create_constraint({.a=body_before,.b=right_body});
    check(constraint.valid(),"constraint fixture failed");
    check(!change(left,"Collider3D","half_extents",Vec3f{1,1,1})&&session.physics()->contains(body_before),"constrained body was replaced");
    session.physics()->destroy_constraint(constraint);
    kin::EcsWorld foreign;register_builtin_editor_components(foreign);auto other=foreign.entity().set(Transform3D{});
    const SceneFieldEdit3D foreign_edit{other,"Transform3D","scale",Vec3f{2,2,2}};
    check(!session.edit(std::span(&foreign_edit,1)),"foreign-world edit accepted");
    auto spawned_asset=parse_prefab_asset(R"({"id":"spawned","root":"root","entities":[{"id":"root","components":{"Transform3D":{"position":[8,0,0]},"Collider3D":{"half_extents":[0.3,0.3,0.3]}}}]})",registry);
    check(spawned_asset.ok(),"spawn fixture parse failed");
    auto spawned=instantiate_prefab(*world,*spawned_asset.prefab,{},registry);
    check(spawned.ok()&&spawned.root.get<Collider3D>()->half_extents==Vec3f{.3f,.3f,.3f},"live edit policy intercepted prefab construction");
    session.bodies()->sync_bodies();
    const SceneFieldEdit3D runtime_edit{spawned.root,"Collider3D","half_extents",Vec3f{.6f,.3f,.3f}};
    ok(session.edit(std::span(&runtime_edit,1)),"explicit runtime entity editing failed");
    auto parent=group;
    for(int i=0;i<17;++i)parent=world->entity().set(Transform3D{}).child_of(parent);
    check(!change(group,"Transform3D","scale",Vec3f{3,3,3})&&group.get<Transform3D>()->scale==Vec3f{2,2,2},"affected entity/ancestor budget ignored");
    world->raw().defer_begin();check(!change(left,"Transform3D","scale",Vec3f{2,2,2}),"deferred edit accepted");world->raw().defer_end();
    renderer.clear();ok(session.render(),"edited scene could not render");renderer.present();
    ok(session.prepare("edits.kinscene"),"pending edit fixture failed");
    check(!change(left,"ModelMaterials3D","variant",std::string("red")),"edit accepted with prepared replacement");session.cancel();
    // Capacity failure: one body fills this world; old one must stay queryable.
    write(scratch.path/"full.kinscene",R"({"entities":[{"id":"box","components":{"Transform3D":{},"Collider3D":{}}}]})");
    options.physics->max_bodies=1;
    SceneSession3D full{r3d,assets,options};check(full.load("full.kinscene"),"full capacity fixture failed");
    const auto box=full.entity("box");const auto body=full.bodies()->body_of(box.raw());
    check(!full.world()->components().patch_field(box,"Collider3D","radius",f32{2})&&full.bodies()->body_of(box.raw())==body&&full.physics()->body_count()==1,"no-spare-capacity edit damaged active body");
}

void gpu_session(Renderer2D& renderer) {
    Scratch scratch;
    for (auto file : {"parts.glb","shared-a.gltf","shared pixel.png"})
        std::filesystem::copy_file(std::filesystem::path(KIN_TEST_MODELS)/file,scratch.path/file);
    write(scratch.path/"prop.kinprefab",prefab); write(scratch.path/"test.kinscene",scene);
    write(scratch.path/"textured.kinscene",textured); write(scratch.path/"custom.txt","ready");
    Renderer3D r3d{renderer}; Assets3D assets{r3d,scratch.path};
    bool fail=false, bad_body=false; int initialized=0, composed=0, stepped=0;
    std::weak_ptr<IMeshBackend> prepared_mesh;
    SceneSession3DOptions options;
    options.viewport={160,120}; options.view.samples=1;
    options.physics=World3DConfig{.max_bodies=128,.max_body_pairs=256,.max_contact_constraints=256,
        .temp_memory_bytes=1024*1024,.single_threaded=true,.max_events=128};
    options.register_components=[&](kin::EcsWorld& world) {
        world.components().data("GameMarker").field_bool("ready",true);
        world.systems().register_native<SceneCamera3D>({.id="game.camera.tick",.writes={"SceneCamera3D"},
            .run=[&](kin::EcsEntity,SceneCamera3D&,SystemContext&) { ++stepped; }});
    };
    options.initialize=[&](SceneSession3DContext& context) {
        ++initialized;
        check(context.world.components().find("GameMarker")!=nullptr,"game component registration omitted");
        context.dependencies.push_back("custom.txt");
        if (bad_body) context.world.entity("bad").set(Transform3D{}).set(Collider3D{.kind=Collider3D::Kind::Sphere,.radius=-1});
        if (fail) throw std::runtime_error("game setup rejected");
    };
    options.compose=[&](SceneSession3DContext& context,Scene3D&) {
        ++composed;
        if(auto entity=context.world.entities().find_by_authored_id("candidate"))
            prepared_mesh=entity.get<ModelRenderer3D>()->model->parts[0].mesh.backend();
    };
    SceneSession3D session{r3d,assets,options};
    const auto ok=[&](bool result,const char* message) {
        if (!result) for (const auto& error:session.diagnostics()) std::cerr<<error<<'\n';
        check(result,message);
    };
    check(!session.active() && !session.step(.01f) && !session.activate(),"empty session accepted simulation/activation");
    ok(session.prepare("test.kinscene"),"first prepare failed");
    check(session.prepared() && !session.active() && session.generation()==0,"prepare activated early");
    ok(session.activate(),"first activate failed");
    check(session.physics() && session.physics()->body_count()==1 && session.bodies(),"optional physics not initialized");
    auto* initial_world=session.world(); const auto first=session.entity("prop");
    check(first && session.entity("camera") && session.instance()->by_authored_id.size()==3,"authored entity lookup failed");
    auto model=first.get<ModelRenderer3D>()->model;
    check(model && model->parts.size()==3,"incomplete model binding");
    const auto original_y=first.get<Transform3D>()->position.y;
    ok(session.step(1.f/60),"scene step failed");
    check(stepped==1,"registered game systems were not stepped");
    check(first.get<Transform3D>()->position.y<original_y,"scene step did not simulate physics");
    check(first.raw().owns<Transform3D>() && first.raw().target(flecs::IsA).get<Transform3D>()->position.y==original_y,
        "physics writeback changed shared prefab transform");
    check(!session.step(std::numeric_limits<float>::quiet_NaN()),"NaN dt accepted");
    renderer.clear(); ok(session.render(),"scene render failed"); renderer.present();
    check(r3d.stats().instances==3 && composed>=2,"scene collection incomplete");
    auto* target=session.view()->backend(); auto generation=session.generation();
    ok(session.prepare("test.kinscene"),"second prepare failed");
    check(session.world()==initial_world && first.alive() && session.generation()==generation,"prepare touched active world");
    check(!session.resize({200,120}),"resize accepted while candidate retained");
    session.cancel(); check(!session.prepared() && session.world()==initial_world,"cancel touched active world");
    std::filesystem::copy_file(scratch.path/"parts.glb",scratch.path/"candidate.glb");
    write(scratch.path/"candidate.kinscene",R"({"entities":[{"id":"candidate","components":{"Transform3D":{},"ModelRenderer3D":{"asset":"candidate.glb"}}}]})");
    ok(session.prepare("candidate.kinscene"),"unique candidate preparation failed");
    check(!prepared_mesh.expired(),"candidate was not bound"); session.cancel();
    check(prepared_mesh.expired() && session.world()==initial_world,"cancel retained candidate GPU resources");
    ok(session.reload(),"unchanged reload failed");
    check(session.generation()==generation+1 && session.view()->backend()==target && session.entity("prop").get<ModelRenderer3D>()->model==model,
        "unchanged reload duplicated art/view or failed to advance generation");
    check(session.changed_sources().empty() && session.reload_if_changed()==SceneSession3DReload::Unchanged,"unchanged sources reloaded");

    const auto preserved=[&] {
        check(session.world()==initial_world && session.view()->backend()==target && session.generation()==generation &&
            session.entity("prop").get<ModelRenderer3D>()->model==model && !session.prepared() && !session.diagnostics().empty(),
            "failed replacement modified active scene or omitted diagnostics");
    };
    initial_world=session.world(); generation=session.generation();
    write(scratch.path/"test.kinscene","broken");
    check(session.reload_if_changed()==SceneSession3DReload::Failed,"broken scene reloaded"); preserved();
    write(scratch.path/"test.kinscene",scene);
    std::filesystem::remove(scratch.path/"prop.kinprefab");
    check(!session.reload(),"deleted prefab reloaded"); preserved();
    write(scratch.path/"prop.kinprefab",prefab);
    write(scratch.path/"bad.kinscene",R"({"schema":"kin.scene/1","entities":[{"id":"x","components":{"Transform3D":{},"ModelRenderer3D":{"asset":"absent.glb"}}}]})");
    check(!session.load("bad.kinscene"),"missing art accepted"); preserved();
    write(scratch.path/"bad.kinscene",R"({"schema":"kin.scene/1","entities":[{"id":"x","components":{"SceneEnvironment3D":{"samples":64}}}]})");
    check(!session.load("bad.kinscene"),"bad view settings accepted"); preserved();
    write(scratch.path/"bad.kinscene",R"({"schema":"kin.scene/1","script":"logic.lua","entities":[]})");
    check(!session.load("bad.kinscene"),"Lua script silently ignored"); preserved();
    fail=true; check(!session.reload(),"failed game initialization accepted"); preserved(); fail=false;
    bad_body=true; check(!session.reload(),"invalid physics accepted"); preserved(); bad_body=false;
    ok(session.reload(),"valid scene could not recover from failures");
    write(scratch.path/"custom.txt","updated");
    check(session.changed_sources()==std::vector<std::filesystem::path>{scratch.path/"custom.txt"},"custom dependency missing");
    check(session.reload_if_changed()==SceneSession3DReload::Activated,"custom dependency reload failed");
    write(scratch.path/"prop.kinprefab",prefab);
    check(session.reload_if_changed()==SceneSession3DReload::Activated,"prefab edit reload failed");
    check(initialized>=8,"initialization not rerun on replacement");
    ok(session.resize({192,128}),"valid viewport resize failed");
    check(session.view()->size()==Vec2i{192,128},"view resize ignored");

    // A corrupt texture must reject replacement even though the low-level model
    // loader permits materials without textures. The old scene stays usable.
    ok(session.load("textured.kinscene"),"textured scene failed");
    auto textured_model=session.entity("quad").get<ModelRenderer3D>()->model;
    check(std::ranges::find(textured_model->source_files,scratch.path/"shared pixel.png")!=textured_model->source_files.end(),"decoded external image dependency absent");
    const auto png=read(scratch.path/"shared pixel.png");
    write(scratch.path/"shared pixel.png","bad png");
    check(session.reload_if_changed()==SceneSession3DReload::Failed && session.entity("quad").get<ModelRenderer3D>()->model==textured_model,
        "corrupt external texture replaced active model");
    renderer.clear(); ok(session.render(),"last good textured scene stopped rendering"); renderer.present();
    write(scratch.path/"shared pixel.png",png);
    check(session.reload_if_changed()==SceneSession3DReload::Activated && session.entity("quad").get<ModelRenderer3D>()->model!=textured_model,
        "external image reload reused stale model");
    write(scratch.path/"shared-a.gltf",read(scratch.path/"shared-a.gltf")+"\n");
    check(session.reload_if_changed()==SceneSession3DReload::Activated,"model source edit not reloaded");

    // Turn the tiny quad's embedded buffer into an external buffer without
    // another checked-in fixture. The layout is 12 position floats, 8 UV floats,
    // followed by six 32-bit indices (the fixture's existing buffer views).
    auto external=read(scratch.path/"shared-a.gltf");
    const auto uri=external.find("data:application/octet-stream;base64,");
    check(uri!=std::string::npos,"fixture buffer URI missing");
    external.replace(uri,external.find('"',uri)-uri,"quad.bin");
    const float vertices[]={-1,-1,0,1,-1,0,1,1,0,-1,1,0,0,0,1,1,1,0,0,0};
    const u32 indices[]={0,1,2,0,2,3};
    { std::ofstream bin(scratch.path/"quad.bin",std::ios::binary);
      bin.write(reinterpret_cast<const char*>(vertices),sizeof(vertices));
      bin.write(reinterpret_cast<const char*>(indices),sizeof(indices)); }
    write(scratch.path/"shared-a.gltf",external);
    check(session.reload_if_changed()==SceneSession3DReload::Activated,"external buffer scene failed");
    write(scratch.path/"quad.bin",read(scratch.path/"quad.bin")+std::string(1,'\0'));
    check(session.reload_if_changed()==SceneSession3DReload::Activated,"external buffer edit not reloaded");
    std::filesystem::remove(scratch.path/"quad.bin");
    check(session.reload_if_changed()==SceneSession3DReload::Failed,"missing external buffer accepted");
    // Restore the original embedded buffer before archive checks.
    write(scratch.path/"shared-a.gltf",read(std::filesystem::path(KIN_TEST_MODELS)/"shared-a.gltf"));
    check(session.reload_if_changed()==SceneSession3DReload::Activated,"buffer failure did not recover");

    std::weak_ptr<IMeshBackend> weak=session.entity("quad").get<ModelRenderer3D>()->model->parts[0].mesh.backend();
    session.unload();
    check(!session.active() && !session.world() && !session.view() && weak.expired(),"unload pinned world/mesh resources");
    model.reset(); textured_model.reset(); assets.trim();
    check(assets.stats().model_entries==0 && assets.stats().texture_entries==0,"weak asset cache retained scene resources");
    for (int i=0;i<4;++i) { ok(session.load("test.kinscene"),"repeated load failed"); session.unload(); }
    check(assets.stats().model_entries==0,"repeated load/unload retained models");

    SceneSession3DOptions bounded; bounded.viewport={64,64}; bounded.view.samples=1; bounded.warm_up=false;
    bounded.limits={4096,2};
    SceneSession3D limited{r3d,assets,bounded};
    check(limited.load("textured.kinscene") && !limited.physics(),"default session unexpectedly allocated physics");
    auto* limited_world=limited.world();
    check(!limited.load("test.kinscene") && limited.world()==limited_world,"entity limit destroyed active scene");
    bounded.max_dependencies=1; SceneSession3D dependencies{r3d,assets,bounded};
    check(!dependencies.load("textured.kinscene") && !dependencies.active(),"dependency limit ignored");

    // Pack remounts use the existing content utilities. Ownership identity must
    // detect a new mapping even when archive size and timestamps are identical.
    Scratch archives;
    const auto archive=archives.path/"scene.kinpak";
    check(write_content_pack(scratch.path,archive).ok,"scene archive write failed");
    const auto mount=archives.path/"virtual";
    mount_content_pack(mount,ContentPack::open(archive));
    { Assets3D packed_assets{r3d,mount};
      // Use a normal dependency budget for the packed model's external image.
      SceneSession3DOptions pack_options; pack_options.viewport={64,64}; pack_options.view.samples=1;
      SceneSession3D packed_session{r3d,packed_assets,pack_options};
      check(packed_session.load("textured.kinscene"),"packed scene load failed");
      mount_content_pack(mount,ContentPack::open(archive));
      check(!packed_session.changed_sources().empty() && packed_session.reload_if_changed()==SceneSession3DReload::Activated,
        "content pack remount not reloaded"); }
    unmount_content_pack(mount);

    // A nested prefab participates in the same transactional source watches.
    const auto leaf=R"({"id":"leaf","root":"root","entities":[{"id":"root","components":{"Transform3D":{},"Collider3D":{}}}]})";
    write(scratch.path/"leaf.kinprefab",leaf);
    write(scratch.path/"nested.kinprefab",R"({"id":"container","root":"root","entities":[{"id":"root","components":{"Transform3D":{}}}],"prefabs":[{"id":"child","asset":"leaf.kinprefab"}]})");
    write(scratch.path/"nested.kinscene",R"({"prefabs":[{"id":"nested","asset":"nested.kinprefab"}]})");
    {
        SceneSession3D nested{r3d,assets,options};
        check(nested.load("nested.kinscene")&&nested.entity("nested.child")&&nested.physics()->body_count()==1,"composed physics scene failed");
        auto* active=nested.world();const auto generation=nested.generation();
        write(scratch.path/"leaf.kinprefab","broken");
        check(nested.reload_if_changed()==SceneSession3DReload::Failed&&nested.world()==active&&nested.generation()==generation&&nested.step(1.f/60),"bad nested edit discarded active simulation");
        write(scratch.path/"leaf.kinprefab",std::string(leaf)+"\n");
        check(nested.reload_if_changed()==SceneSession3DReload::Activated&&nested.entity("nested.child")&&nested.physics()->body_count()==1,"nested edit did not recover");
    }

    // CPU collision assets participate in the same transactional dependency
    // pipeline even when their source model has no visible renderer component.
    write(scratch.path/"solid.kincollider",R"({"schema":"kin.collider3d/1","shape":{"kind":"convex_hull","mesh":"parts.glb"}})");
    write(scratch.path/"collision.kinscene",R"({"entities":[{"id":"solid","components":{"Transform3D":{},"Collider3D":{"asset":"solid.kincollider"}}}]})");
    {
        SceneSession3D collision{r3d,assets,options};
        check(collision.load("collision.kinscene"),"collision asset scene failed");
        auto* active=collision.world();auto shape=collision.entity("solid").get<Collider3D>()->asset_binding;
        const auto source_model=read(scratch.path/"parts.glb");
        write(scratch.path/"parts.glb",source_model+" ");
        check(!collision.changed_sources().empty(),"invisible collision model dependency not watched");
        write(scratch.path/"parts.glb",source_model);
        write(scratch.path/"solid.kincollider",R"({"schema":"kin.collider3d/1","shape":{"kind":"box","scale":[0,1,1]}})");
        check(collision.reload_if_changed()==SceneSession3DReload::Failed&&collision.world()==active&&
            collision.entity("solid").get<Collider3D>()->asset_binding==shape&&collision.step(1.f/60),"bad collision edit replaced running world");
        write(scratch.path/"solid.kincollider",R"({"schema":"kin.collider3d/1","shape":{"kind":"box"}})");
        check(collision.reload_if_changed()==SceneSession3DReload::Activated&&collision.world()!=active,"collision reload did not recover");
    }

    if (r3d.capabilities().skinning) {
        const auto content=std::filesystem::path(KIN_TEST_SUMMIT);
        { Assets3D camp_assets{r3d,content}; SceneSession3D camp{r3d,camp_assets,options};
          check(camp.load("scenes/camp.kinscene") && camp.entity("survivor") && camp.step(1.f/60),"real camp scene failed");
          auto hero=camp.entity("survivor");const auto time=hero.get<Animator3D>()->player.time();
          const auto model=hero.get<SkinnedMeshRenderer>()->model;
          check(camp.world()->components().patch_field(hero,"SkinnedMeshRenderer","tint",Color::rgb(220,230,255)),"live skinned tint edit failed");
          check(hero.get<Animator3D>()->player.time()==time&&hero.get<SkinnedMeshRenderer>()->model==model,"compatible skinned edit reset animation/geometry");
          renderer.clear(); check(camp.render(),"real camp render failed");
          renderer.draw_texture(camp.view()->texture(),{0,0,160,120});
          std::vector<u8> pixels; Vec2i size;
          check(renderer.read_rgba({0,0,160,120},pixels,size) && size==Vec2i{160,120},"camp readback failed");
          // A textured camp must contain visible detail, not just a cleared view.
          const auto [minimum,maximum]=std::minmax_element(pixels.begin(),pixels.end());
          check(maximum!=pixels.end() && int(*maximum)-int(*minimum)>40 && r3d.stats().instances>=5,"camp render is incomplete");
          if(const auto* capture=std::getenv("KIN_TEST_SESSION_CAPTURE")) check(renderer.save_png(capture),"camp capture failed");
          renderer.present(); }

        for (auto dir:{"kenney-character","animations"}) std::filesystem::create_directories(scratch.path/dir);
        for (auto file:{"kenney-character/survivor.glb","animations/survivor.kinanim","animations/survivor.kinanimator"})
            std::filesystem::copy_file(content/file,scratch.path/file);
        write(scratch.path/"hero.kinprefab",read(content/"prefabs/survivor.kinprefab"));
        write(scratch.path/"hero.kinscene",R"({"prefabs":[{"id":"hero","asset":"hero.kinprefab"}]})");
        ok(session.load("hero.kinscene"),"animated scene failed");
        check(session.entity("hero").get<AnimatorAsset3D>()->configuration->machine_asset==scratch.path/"animations/survivor.kinanim",
            "machine dependency path absent");
        write(scratch.path/"animations/survivor.kinanim",read(scratch.path/"animations/survivor.kinanim")+"\n");
        check(session.reload_if_changed()==SceneSession3DReload::Activated,"machine edit not reloaded");
        const auto config=read(scratch.path/"animations/survivor.kinanimator");
        write(scratch.path/"animations/survivor.kinanimator",config+"\n");
        check(session.reload_if_changed()==SceneSession3DReload::Activated,"animator config edit not reloaded");
        auto* running=session.world();
        write(scratch.path/"animations/survivor.kinanimator","broken");
        check(session.reload_if_changed()==SceneSession3DReload::Failed && session.world()==running,"broken animator replaced scene");
        write(scratch.path/"animations/survivor.kinanimator",config);
        check(session.reload_if_changed()==SceneSession3DReload::Activated,"animator failure did not recover");
    }
}
} // namespace

int main(int argc,char** argv) {
    try { cpu(); }
    catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
    if(argc>1 && std::string_view(argv[1])=="cpu") return 0;
    bool ready=false;
    try {
        App app{}; auto& window=app.create_window({.title="scene-session-tests",.width=192,.height=128,.hidden=true});
        auto backend=make_render_backend(window,false,true);
        if(!backend || backend->name()!="SDL_GPU") throw std::runtime_error("GPU backend required for lifecycle checks");
        ready=true; Renderer2D renderer{std::move(backend)}; gpu_live_edits(renderer); gpu_session(renderer);
        std::cout<<"Transactional scene lifecycle, reload, budgets and ownership checks passed\n";
    } catch(const std::exception& error) {
        std::cerr<<error.what()<<'\n'; const char* required=std::getenv("KIN_REQUIRE_GPU_TESTS");
        return ready || (required && std::string_view(required)!="0") ? 1 : 77;
    }
}
