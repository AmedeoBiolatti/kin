#include <kin/physics3d/assets3d.hpp>
#include <kin/ecs/physics3d.hpp>
#include <kin/ecs/editor.hpp>
#include <kin/scene/scene_asset_runtime.hpp>
#include <kin/assets/content.hpp>
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
using namespace kin;
void check(bool ok,const std::string& message) {if(!ok)throw std::runtime_error(message);}
struct Scratch {
    std::filesystem::path path=std::filesystem::temp_directory_path()/
        ("kin-physics-assets-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Scratch(){std::filesystem::create_directories(path);}
    ~Scratch(){std::error_code e;std::filesystem::remove_all(path,e);}
    void write(std::string_view name,std::string_view text){std::ofstream(path/name)<<text;}
};
std::string recipe(std::string_view shape){return "{\"schema\":\"kin.collider3d/1\",\"shape\":"+std::string(shape)+"}";}
World3DConfig config(){World3DConfig c;c.single_threaded=true;c.max_bodies=64;c.max_body_pairs=256;c.max_contact_constraints=256;c.temp_memory_bytes=2*1024*1024;return c;}
void parser_and_cache() {
    for(auto text:{R"({"kind":"oops"})",R"({"kind":"box","radius":-1})",R"({"kind":"box","scale":[0,1,1]})",
        R"({"kind":"compound","children":[]})",R"({"kind":"sphere","unknown":1})",
        R"({"kind":"triangle_mesh","points":[[0,0,0],[1,0,0],[0,1,0]],"indices":[0,1,9]})"})
        check(!parse_collider3d_asset(recipe(text)).ok(),"invalid recipe accepted");
    auto small=ColliderAssetLimits3D{};small.nodes=1;
    check(!parse_collider3d_asset(recipe(R"({"kind":"compound","children":[{"kind":"box"}]})"),small).ok(),"node budget ignored");
    small={};small.source_bytes=8;check(!parse_collider3d_asset(recipe(R"({"kind":"box"})"),small).ok(),"source budget ignored");
    Scratch scratch;scratch.write("box.kincollider",recipe(R"({"kind":"box","half_extents":[1,2,3]})"));
    PhysicsAssets3D assets(scratch.path);std::string error;
    auto a=assets.load_collider("box.kincollider",{1,1,1},&error);check(bool(a),error);
    auto b=assets.load_collider("./box.kincollider");
    check(a.shared()==b.shared()&&assets.stats().cooks==1&&assets.stats().hits==1,"shape cache does not share path aliases");
    auto scaled=assets.load_collider("box.kincollider",{2,1,3});check(bool(scaled),"scaled box failed");
    check(scaled.shared()!=a.shared()&&length(scaled->shape.bounds_max()-Vec3f{2,2,9})<.001f,"scale not baked once");
    scratch.write("box.kincollider",recipe(R"({"kind":"box","scale":[-1,1,1]})"));assets.invalidate("box.kincollider");
    check(!assets.load_collider("box.kincollider",{1,1,1},&error)&&!error.empty(),"bad recook accepted");
    check(a->shape.valid(),"invalidation destroyed live shape");
    std::weak_ptr<const CookedCollider3D> weak=a.shared();Shape3D retained=a->shape;a={};b={};scaled={};assets.trim();
    check(weak.expired()&&assets.stats().entries==0&&retained.valid(),"cache retains collision assets or shape died");
    scratch.write("hull.kincollider",recipe(R"({"kind":"convex_hull","points":[[0,0,0],[1,0,0],[0,1,0],[0,0,1]]})"));
    auto hull=assets.load_collider("hull.kincollider",{1,1,1},&error);check(bool(hull),error);
    check(hull->shape.volume()>.1f&&!hull->contains_surface,"convex hull is not volumetric");
    scratch.write("plane.kincollider",recipe(R"({"kind":"triangle_mesh","points":[[0,0,0],[1,0,0],[0,0,1]],"indices":[0,1,2]})"));
    auto plane=assets.load_collider("plane.kincollider",{1,1,1},&error);check(bool(plane)&&plane->contains_surface,error);
    scratch.write("flat.kincollider",recipe(R"({"kind":"convex_hull","points":[[0,0,0],[1,0,0],[0,0,1],[1,0,1]]})"));
    check(!assets.load_collider("flat.kincollider",{1,1,1},&error)&&!error.empty(),"degenerate hull accepted");
    scratch.write("missing.kincollider",recipe(R"({"kind":"convex_hull","mesh":"missing.glb"})"));
    check(!assets.load_collider("missing.kincollider",{1,1,1},&error)&&error.find("missing.glb")!=std::string::npos,"geometry diagnostic lost");
    kin::EcsWorld ecs;register_builtin_editor_components(ecs);
    auto entity=ecs.entities().create({.name="surface"}).set(Transform3D{}).set(Collider3D{.asset={"plane.kincollider"}}).set(RigidBody3D{});
    check(!resolve_physics3d_assets(ecs,assets).empty()&&!entity.get<Collider3D>()->asset_binding,"dynamic mesh accepted");
    entity.get_mut<RigidBody3D>()->motion=Body3DMotion::Static;
    check(resolve_physics3d_assets(ecs,assets).empty(),"static surface rejected");
    const auto bound=entity.get<Collider3D>()->asset_binding;
    entity.get_mut<Transform3D>()->scale={-1,1,1};
    check(!resolve_physics3d_assets(ecs,assets).empty()&&entity.get<Collider3D>()->asset_binding==bound,"failed binding lost last good shape");
}
void geometry_and_camp() {
    const std::filesystem::path root=KIN_TEST_SUMMIT;
    ModelGeometry3D geometry;std::string error;
    check(read_model_geometry3d(root/"kenney-survival/barrel.glb",geometry,{},&error),error);
    check(!geometry.vertices.empty()&&!geometry.indices.empty()&&geometry.source_files.size()==1,"CPU geometry missing");
    const auto count=geometry.vertices.size();auto tiny=ModelGeometryLimits3D{};tiny.vertices=1;
    check(!read_model_geometry3d(root/"kenney-survival/barrel.glb",geometry,tiny,&error)&&geometry.vertices.size()==count,"geometry budget or last good output lost");
    check(!read_model_geometry3d(root/"kenney-character/survivor.glb",geometry,{},&error)&&error.find("skinned")!=std::string::npos,"animated visual accepted as static collision");
    PhysicsAssets3D assets(root);kin::EcsWorld ecs;register_builtin_editor_components(ecs);SceneAssetRuntime source;
    const bool loaded=source.load(root/"scenes/camp-play.kinscene",root,ecs.components());
    check(loaded,source.diagnostics().empty()?"camp scene parse failed":source.diagnostics().front());
    check(source.instantiate(ecs),"camp scene instantiate failed");
    const auto errors=resolve_physics3d_assets(ecs,assets);
    check(errors.empty(),errors.empty()?"":errors.front());
    auto player=source.instance().by_authored_id.at("player");
    auto brass=source.instance().by_authored_id.at("brass-barrel");
    auto blue=source.instance().by_authored_id.at("blue-barrel");
    check(brass.get<Collider3D>()->asset_binding==blue.get<Collider3D>()->asset_binding,"prefab barrels do not share cooked shape");
    check(assets.stats().cooks==3,"unexpected camp cooking count");
    check(blue.get<Collider3D>()->asset_binding->source_files.size()==2,"mesh dependency not recorded");
    World3D physics(config());WorldPhysicsState3D bodies(ecs,physics);bodies.sync_bodies();
    check(bodies.body_count()==4&&bool(bodies.character_of(ecs.raw().entity(player.id()))),"camp physics objects missing");
    const auto barrel_hit=physics.raycast({2.2f,.4f,2},{2.2f,.4f,-2});
    check(barrel_hit&&barrel_hit->body==bodies.body_of(blue.raw()),"cooked barrel does not block a ray");
    for(int i=0;i<90;++i)bodies.step(1.f/60);
    check(player.get<CharacterController3D>()->state.ground==CharacterGround3D::OnGround,"player did not settle on compound floor");
    const auto start=player.get<Transform3D>()->position;
    for(int i=0;i<20;++i){player.get_mut<CharacterController3D>()->input.move={-1,0};bodies.step(1.f/60);}
    check(player.get<Transform3D>()->position.x<start.x-.3f,"authored player cannot move");
    player.get_mut<CharacterController3D>()->input={.jump=true};bodies.step(1.f/60);
    check(player.get<CharacterController3D>()->state.jumped,"authored player cannot jump");
    player.get_mut<CharacterController3D>()->input={};
    for(int i=0;i<100;++i)bodies.step(1.f/60);
    check(player.get<CharacterController3D>()->state.ground==CharacterGround3D::OnGround,"player did not land");
    auto saved=build_scene_asset(ecs);check(saved.ok(),"physics scene save failed");std::ostringstream text;write_scene_asset(text,*saved.scene);
    check(parse_scene_asset(text.str(),ecs.components()).ok(),"physics scene save no longer parses");
    check(text.str().find("asset_binding")==std::string::npos&&text.str().find("ground_speed")==std::string::npos,"runtime state serialized");
    player.get_mut<CharacterController3D>()->desc.height=.1f;
    check(!resolve_physics3d_assets(ecs,assets).empty(),"invalid capsule dimensions accepted");
}
void external_geometry_and_packs() {
    Scratch scratch;const float vertices[]={0,0,0,1,0,0,0,1,0};
    {std::ofstream file(scratch.path/"mesh.bin",std::ios::binary);file.write(reinterpret_cast<const char*>(vertices),sizeof(vertices));}
    const std::string gltf=R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":36,"uri":"mesh.bin"}],"bufferViews":[{"buffer":0,"byteLength":36}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]}],"images":[{"uri":"missing.png"}],"meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],"nodes":[{"mesh":0,"translation":[2,3,4]}],"scenes":[{"nodes":[0]}],"scene":0})";
    scratch.write("mesh.gltf",gltf);ModelGeometry3D geometry;std::string error;
    check(read_model_geometry3d(scratch.path/"mesh.gltf",geometry,{},&error),error);
    check(geometry.vertices[0]==Vec3f{2,3,4}&&geometry.vertices[1]==Vec3f{3,3,4}&&geometry.source_files.size()==2,"node transform or external buffer dependency missing");
    auto limits=ModelGeometryLimits3D{};limits.buffer_bytes=35;
    check(!read_model_geometry3d(scratch.path/"mesh.gltf",geometry,limits,&error)&&error.find("budget")!=std::string::npos,"buffer budget ignored");
    scratch.write("mesh.kincollider",recipe(R"({"kind":"triangle_mesh","mesh":"mesh.gltf"})"));
    ColliderAssetLimits3D bounded;bounded.cache_entries=1;PhysicsAssets3D assets(scratch.path,bounded);
    auto a=assets.load_collider("mesh.kincollider",{1,1,1},&error);check(bool(a),error);
    auto b=assets.load_collider("mesh.kincollider",{2,2,2},&error);check(bool(b)&&assets.stats().entries==1&&a->shape.valid(),"cache bound evicted live asset");
    assets.invalidate("mesh.bin");check(assets.stats().entries==0&&b->shape.valid(),"external buffer invalidation failed");
    Scratch archive;check(write_content_pack(scratch.path,archive.path/"collision.kinpak").ok,"collision pack write failed");
    const auto mount=archive.path/"virtual";mount_content_pack(mount,ContentPack::open(archive.path/"collision.kinpak"));
    {PhysicsAssets3D packed(mount);auto shape=packed.load_collider("mesh.kincollider",{1,1,1},&error);check(bool(shape)&&shape->source_files.size()==3,error);}
    unmount_content_pack(mount);
}
void prefab_character_isolation() {
    kin::EcsWorld ecs;register_builtin_editor_components(ecs);
    auto source=parse_prefab_asset(R"({"schema":"kin.prefab/1","id":"hero","root":"hero","entities":[{"id":"hero","components":{"Transform3D":{},"CharacterController3D":{"walk_speed":2},"Animator3D":{},"CharacterAnimationLink3D":{}}}]})",ecs.components());
    check(source.ok(),"controller prefab parse failed");
    auto a=instantiate_prefab(ecs,*source.prefab,{},ecs.components());auto b=instantiate_prefab(ecs,*source.prefab,{},ecs.components());
    check(a.ok()&&b.ok(),"controller prefab instantiate failed");
    b.root.get_mut<Transform3D>()->position={3,0,0};
    ecs.entities().create().set(Transform3D{.position={0,-.5f,0}}).set(Collider3D{.half_extents={10,.5f,10}});
    World3D physics(config());WorldPhysicsState3D bodies(ecs,physics);bodies.sync_bodies();
    a.root.get_mut<CharacterController3D>()->input.move={1,0};
    for(int i=0;i<60;++i)bodies.step(1.f/60);
    check(a.root.get<Transform3D>()->position.x>1&&std::abs(b.root.get<Transform3D>()->position.x-3)<.01f,"prefab controller simulation leaked between instances");
    check(a.root.get<CharacterController3D>()->state.ground_speed>1&&b.root.get<CharacterController3D>()->state.ground_speed<.01f,"controller state not per instance");
    check(a.root.get<Animator3D>()!=b.root.get<Animator3D>(),"animation runtime still inherited");
}
}
int main(){try{parser_and_cache();geometry_and_camp();external_geometry_and_packs();prefab_character_isolation();std::cout<<"physics3d assets: passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
