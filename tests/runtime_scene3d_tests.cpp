#include <kin/scene/scene_session3d.hpp>
#include <kin/ecs/system.hpp>
#include <kin/assets/content.hpp>
#include <kin/platform/app.hpp>
#include <kin/renderer/renderer2d.hpp>
#include "../examples/projectile_range/gameplay.hpp"

#include <chrono>
#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace kin;
void check(const auto& value,const char* message){if(!value)throw std::runtime_error(message);}
struct Scratch {
    std::filesystem::path path=std::filesystem::temp_directory_path()/(
        "kin-runtime3d-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Scratch(){std::filesystem::create_directories(path);}
    ~Scratch(){std::error_code error;std::filesystem::remove_all(path,error);}
};
void write(const std::filesystem::path& path,std::string_view text){std::ofstream(path)<<text;}
// This is ordinary C++ gameplay state, deliberately absent from the reflected
// component registry. A native system and prefab constructor still use it.
struct Gameplay { int value=0,ticks=0;std::shared_ptr<int> lifetime; };
constexpr auto level=R"({"entities":[
 {"id":"anchor","components":{"Transform3D":{"position":[10,3,0]},"ModelRenderer3D":{"asset":"parts.glb"},"Collider3D":{},"RigidBody3D":{"motion":"dynamic"}}},
 {"id":"floor","components":{"Transform3D":{"position":[0,-1,0]},"Collider3D":{"half_extents":[20,0.5,20]}}}]})";
constexpr auto object=R"({"id":"object","root":"root","entities":[
 {"id":"root","components":{"Transform3D":{"scale":[2,2,2]},"ModelRenderer3D":{"asset":"parts.glb"},"Collider3D":{"asset":"box.kincollider"},"RigidBody3D":{"motion":"dynamic","mass":2}}},
 {"id":"lamp","parent":"root","components":{"Transform3D":{"position":[0,1,0]},"PointLight3D":{"radius":2}}}]})";

void runtime(Renderer2D& renderer) {
    Scratch scratch;
    std::filesystem::copy_file(std::filesystem::path(KIN_TEST_MODELS)/"parts.glb",scratch.path/"parts.glb");
    write(scratch.path/"level.kinscene",level);write(scratch.path/"object.kinprefab",object);
    write(scratch.path/"box.kincollider",R"({"schema":"kin.collider3d/1","shape":{"kind":"box","half_extents":[0.5,0.5,0.5]}})");
    Renderer3D r3d{renderer};Assets3D assets{r3d,scratch.path};
    SceneSession3DOptions options;options.viewport={160,120};options.view.samples=1;options.warm_up=false;
    options.max_spawn_entities=4;options.max_runtime_entities=12;options.max_runtime_prefabs=4;
    options.physics=World3DConfig{.max_bodies=128,.max_body_pairs=256,.max_contact_constraints=256,
        .temp_memory_bytes=1024*1024,.single_threaded=true};
    options.register_components=[](kin::EcsWorld& world) {
        world.component<Gameplay>("Gameplay");
        world.systems().register_native<Gameplay>({.id="game.tick",.run=[](kin::EcsEntity,Gameplay& game,SystemContext&){++game.ticks;}});
    };
    SceneSession3D session{r3d,assets,options};
    const auto ok=[&](const auto& result,const char* why){if(!result)for(const auto& e:session.diagnostics())std::cerr<<e<<'\n';check(result,why);};
    ok(session.load("level.kinscene"),"runtime level failed");
    check(!session.world()->components().find("Gameplay"),"gameplay unexpectedly required inspector metadata");
    const auto anchor=session.handle(session.entity("anchor"));
    auto anchor_body=session.bodies()->body_of(session.resolve(anchor).raw());
    const auto initial_bodies=session.physics()->body_count();
    session.bodies()->rebuild(session.resolve(anchor).raw());
    session.resolve(anchor).get_mut<ModelRenderer3D>()->asset.id="unrelated-missing.glb";
    int constructions=0;std::weak_ptr<int> lifetime;
    SceneSpawn3D spawn;spawn.position=Vec3f{-3,3,0};
    spawn.configure=[&](SceneSpawnContext3D& ctx) {
        ++constructions;auto token=std::make_shared<int>(42);lifetime=token;
        ctx.root.set(Gameplay{12,0,token});
        check(bool(ctx.entity("lamp")),"local prefab lookup missing");
    };
    const auto first=session.spawn("object.kinprefab",spawn).value_or({});
    ok(bool(first),"runtime spawn failed");
    check(constructions==1&&session.runtime_prefabs()==1&&session.runtime_entities()==2,"constructor count/ownership incorrect");
    check(session.resolve(first.root).get<Transform3D>()->position==Vec3f{-3,3,0}&&session.resolve(first.root).get<Transform3D>()->scale==Vec3f{2,2,2},"spawn placement lost prefab defaults");
    check(session.resolve(first.root).get<Gameplay>()->value==12&&!lifetime.expired(),"native gameplay component lost");
    check(session.bodies()->body_of(session.resolve(anchor).raw())==anchor_body,"spawn consumed an unrelated rebuild request");
    session.bodies()->sync_bodies();
    check(session.bodies()->body_of(session.resolve(anchor).raw())!=anchor_body,"unrelated rebuild request was dropped");
    anchor_body=session.bodies()->body_of(session.resolve(anchor).raw());
    const auto second=session.spawn("object.kinprefab",{.position=Vec3f{3,3,0}}).value_or({});
    ok(bool(second),"second spawn failed");
    check(session.resolve(first.root).get<ModelRenderer3D>()->model==session.resolve(second.root).get<ModelRenderer3D>()->model,"spawns duplicated model geometry");
    check(session.physics()->body_count()==initial_bodies+2,"spawn did not create physics immediately");
    ok(session.step(1.f/60),"runtime system step failed");
    check(session.resolve(first.root).get<Gameplay>()->ticks==1,"native gameplay system did not run");
    auto body=session.bodies()->body_of(session.resolve(first.root).raw());
    session.physics()->set_linear_velocity(body,{1,2,0});
    ok(session.reconfigure<Transform3D>(first.root,[](auto& t){t.scale={3,3,3};}),"typed configuration failed");
    auto changed=session.bodies()->body_of(session.resolve(first.root).raw());
    check(changed!=body&&session.physics()->linear_velocity(changed)==Vec3f{1,2,0},"typed configuration did not preserve body velocity");
    check(!session.reconfigure<Transform3D>(first.root,[](auto& t){t.scale={-1,1,1};})&&session.bodies()->body_of(session.resolve(first.root).raw())==changed,"bad typed configuration changed physics");
    check(!session.reconfigure<Transform3D>(first.root,[](auto&){throw std::runtime_error("rejected");}),"throwing configuration accepted");
    ok(session.reconfigure<Transform3D>(first.root,[](auto&){}),"unchanged typed configuration failed");
    check(session.bodies()->body_of(session.resolve(first.root).raw())==changed,"no-op configuration rebuilt a body");
    check(!session.reconfigure<Gameplay>(first.root,[](auto& g){g.value=99;})&&session.resolve(first.root).get<Gameplay>()->value==12,"runtime-only state used reflected configuration");
    // Failed constructors/uploads/native physics must remove partial objects.
    const auto count=session.world()->entities().all().size();const auto bodies=session.physics()->body_count();
    write(scratch.path/"bad.kinprefab",R"({"id":"bad","root":"root","entities":[{"id":"root","components":{"Transform3D":{},"ModelRenderer3D":{"asset":"absent.glb"}}}]})");
    check(!session.spawn("bad.kinprefab").value_or({})&&session.world()->entities().all().size()==count&&session.physics()->body_count()==bodies,"failed upload leaked objects");
    check(!session.spawn("object.kinprefab",{.configure=[](auto&){throw std::runtime_error("constructor failed");}}).value_or({})&&session.runtime_prefabs()==2,"throwing constructor leaked ownership");
    check(!session.spawn("object.kinprefab",{.configure=[](auto& ctx){ctx.root.template get_mut<Collider3D>()->asset={};ctx.root.template get_mut<Collider3D>()->half_extents={-1,1,1};}}).value_or({})&&session.physics()->body_count()==bodies,"invalid shape leaked native bodies");
    std::weak_ptr<int> failed_lifetime;
    check(!session.spawn("object.kinprefab",{.configure=[&](SceneSpawnContext3D& ctx) {
        auto token=std::make_shared<int>(1);failed_lifetime=token;ctx.root.set(Gameplay{1,0,token});
        ctx.entity("lamp").set(Collider3D{.half_extents={-1,1,1}});
    }}).value_or({})&&failed_lifetime.expired()&&session.physics()->body_count()==bodies,"partial native creation leaked bodies/gameplay state");
    check(session.bodies()->body_of(session.resolve(anchor).raw())==anchor_body,"failed spawn rebuilt an unrelated body");
    const auto child=session.spawn("object.kinprefab",{.parent=first.root,.position=Vec3f{0,2,0}}).value_or({});
    ok(bool(child),"nested runtime spawn failed");
    auto forged=first;forged.root=anchor;
    check(!session.alive(forged)&&!session.despawn(forged),"forged prefab handle accepted");
    auto authored=session.entity("floor");session.world()->entities().reparent(authored,session.resolve(first.root));
    check(!session.despawn(first)&&session.alive(first)&&authored.alive(),"despawn destroyed an authored child");
    session.world()->entities().detach_parent(authored);
    renderer.clear();ok(session.render(),"runtime render failed");renderer.present();
    const auto lamp=session.entity(first,"lamp");
    ok(session.despawn(first),"cascading despawn failed");
    check(lifetime.expired()&&!session.resolve(first.root)&&!session.resolve(lamp)&&!session.alive(child),"despawn retained state/descendants/handles");
    check(session.runtime_prefabs()==1&&session.runtime_entities()==2&&session.alive(second),"despawn removed unrelated instance");
    ok(session.despawn(second),"second despawn failed");
    check(session.physics()->body_count()==initial_bodies&&session.runtime_entities()==0,"despawn retained native bodies");
    check(!session.despawn(second),"stale instance despawn accepted");
    std::filesystem::copy_file(scratch.path/"parts.glb",scratch.path/"unique.glb");
    const auto unique=session.spawn("object.kinprefab",{.configure=[](auto& ctx){ctx.root.template get_mut<ModelRenderer3D>()->asset.id="unique.glb";}}).value_or({});
    ok(bool(unique),"unique mesh fixture failed");
    std::weak_ptr<const Model3D> model_weak=session.resolve(unique.root).get<ModelRenderer3D>()->model;
    std::weak_ptr<IMeshBackend3D> mesh_weak=session.resolve(unique.root).get<ModelRenderer3D>()->model->parts[0].mesh.backend();
    renderer.clear();ok(session.render(),"unique mesh render failed");renderer.present();
    ok(session.despawn(unique),"unique mesh despawn failed");
    check(model_weak.expired()&&mesh_weak.expired(),"despawn retained model/mesh through collection buckets");
    auto raw_removed=session.spawn("object.kinprefab",{.configure=[](auto& ctx){ctx.root.template get_mut<ModelRenderer3D>()->asset.id="unique.glb";}}).value_or({});
    ok(bool(raw_removed),"raw removal fixture failed");
    mesh_weak=session.resolve(raw_removed.root).get<ModelRenderer3D>()->model->parts[0].mesh.backend();
    renderer.clear();ok(session.render(),"raw removal render failed");renderer.present();
    session.world()->entities().destroy(session.resolve(raw_removed.root));
    renderer.clear();ok(session.render(),"collect after raw removal failed");renderer.present();
    check(mesh_weak.expired(),"empty collection buckets pinned removed meshes");
    ok(session.despawn(raw_removed),"dead-root ownership cleanup failed");
    for(int i=0;i<24;++i){auto p=session.spawn("object.kinprefab").value_or({});ok(bool(p),"repeated spawn failed");ok(session.despawn(p),"repeated despawn failed");}
    check(session.world()->entities().all().size()==2&&session.physics()->body_count()==initial_bodies,"repeated spawn/despawn accumulated live objects");
    auto saved=build_scene_asset(*session.world());check(saved.ok()&&saved.scene->entities.size()==2,"runtime objects polluted authored saving");
    auto old=session.spawn("object.kinprefab").value_or({});ok(bool(old),"reload handle fixture failed");
    ok(session.reload(),"runtime level reset failed");
    check(!session.resolve(old.root)&&!session.resolve(anchor)&&session.runtime_prefabs()==0&&session.physics()->body_count()==initial_bodies,"reload retained stale handles/ownership");
    // Controllers and native-only rope components use the same lifecycle.
    write(scratch.path/"character.kinprefab",R"({"id":"character","root":"root","entities":[{"id":"root","components":{"Transform3D":{},"CharacterController3D":{}}}]})");
    auto character=session.spawn("character.kinprefab",{.position=Vec3f{0,2,0}}).value_or({});ok(bool(character),"runtime character failed");
    const auto capsule=session.bodies()->character_of(session.resolve(character.root).raw());
    check(capsule.valid(),"runtime character not initialized");ok(session.despawn(character),"character despawn failed");
    check(!session.physics()->contains(capsule)&&session.bodies()->character_count()==0,"character retained native capsule");
    write(scratch.path/"rope.kinprefab",R"({"id":"rope","root":"root","entities":[{"id":"root","components":{"Transform3D":{}}}]})");
    auto rope=session.spawn("rope.kinprefab",{.configure=[](auto& ctx){Rope3D rope;rope.length=2;rope.segments=2;rope.start.kind=RopeAttach3D::Kind::World;rope.start.point={0,3,0};rope.end.point={0,1,0};ctx.root.set(rope);}}).value_or({});
    ok(bool(rope),"native runtime rope failed");const auto rope_handle=session.bodies()->rope_of(session.resolve(rope.root).raw());
    const auto segment=session.physics()->rope_segment_body(rope_handle,0);
    check(session.bodies()->entity_of_body(segment).id()==session.resolve(rope.root).id(),"rope query body ownership failed");
    ok(session.despawn(rope),"runtime rope despawn failed");check(!session.physics()->contains(rope_handle)&&session.physics()->body_count()==initial_bodies,"rope bodies leaked");
    // Explicit limits and scene/session identities.
    auto small_options=options;small_options.max_runtime_entities=3;small_options.limits.max_source_bytes=1024;
    SceneSession3D small{r3d,assets,small_options};check(small.load("level.kinscene"),"small level failed");
    auto one=small.spawn("object.kinprefab").value_or({});check(bool(one)&&!small.spawn("object.kinprefab").value_or({}),"runtime capacity ignored");
    write(scratch.path/"oversized.kinprefab",std::string(1025,' '));
    check(!small.spawn("oversized.kinprefab").value_or({})&&small.runtime_entities()==2,"source budget ignored or existing population changed");
    write(scratch.path/"wide.kinprefab",R"({"id":"wide","root":"a","entities":[{"id":"a"},{"id":"b"},{"id":"c"},{"id":"d"},{"id":"e"}]})");
    check(!session.spawn("wide.kinprefab").value_or({})&&session.runtime_entities()==0,"per-spawn expanded entity cap ignored");
    std::array<ScenePrefab3D,4> limited;
    for(auto& instance:limited){instance=session.spawn("object.kinprefab").value_or({});ok(bool(instance),"instance budget fixture failed");}
    check(!session.spawn("object.kinprefab").value_or({})&&session.runtime_prefabs()==4,"runtime instance cap ignored");
    for(auto instance:limited)ok(session.despawn(instance),"instance budget cleanup failed");
    check(!session.resolve(one.root)&&!session.despawn(one),"foreign session handles accepted");
    check(small.despawn(one)&&!small.spawn("object.kinprefab",{.parent=old.root}).value_or({}),"foreign/stale parent accepted");
    small_options.physics->max_bodies=2;SceneSession3D full{r3d,assets,small_options};
    check(full.load("level.kinscene")&&!full.spawn("object.kinprefab").value_or({})&&full.runtime_entities()==0&&full.physics()->body_count()==2,"full physics capacity damaged level");
    session.world()->raw().defer_begin();check(!session.spawn("object.kinprefab").value_or({}),"deferred spawn accepted");session.world()->raw().defer_end();
    ok(session.prepare("level.kinscene"),"prepared lifecycle fixture failed");check(!session.spawn("object.kinprefab").value_or({}),"spawn accepted during prepared replacement");session.cancel();
    session.unload();check(!session.resolve(old.root)&&!session.spawn("object.kinprefab").value_or({}),"unloaded session accepted handles/spawn");
    {
        Scratch storage;const auto archive=storage.path/"game.kinpak";
        check(write_content_pack(scratch.path,archive).ok,"runtime content pack creation failed");
        const auto mount=storage.path/"mounted";mount_content_pack(mount,ContentPack::open(archive));
        {Assets3D packed_assets{r3d,mount};SceneSession3D packed{r3d,packed_assets,options};
         check(packed.load("level.kinscene"),"packed runtime level failed");
         auto instance=packed.spawn("object.kinprefab").value_or({});
         check(bool(instance)&&packed.physics()->body_count()==initial_bodies+1&&packed.despawn(instance)&&packed.physics()->body_count()==initial_bodies,"packed spawn/despawn failed");}
        unmount_content_pack(mount);
    }
    if(r3d.capabilities().skinning) {
        Assets3D art{r3d,KIN_TEST_SUMMIT};SceneSession3D game{r3d,art,options};
        check(game.load("scenes/camp.kinscene"),"real runtime camp failed");
        auto hero=game.spawn("prefabs/survivor.kinprefab",{.position=Vec3f{4,0,0},.configure=[](auto& ctx){ctx.root.set(Ragdoll3D{});}}).value_or({});
        check(bool(hero)&&game.bodies()->ragdoll_of(game.resolve(hero.root).raw()).valid(),"native runtime ragdoll failed");
        auto ragdoll=game.bodies()->ragdoll_of(game.resolve(hero.root).raw());
        for(auto body:game.physics()->ragdoll_bodies(ragdoll))if(body)
            check(game.bodies()->entity_of_body(body).id()==game.resolve(hero.root).id(),"ragdoll query body ownership failed");
        check(game.step(1.f/60)&&game.despawn(hero)&&!game.physics()->contains(ragdoll),"runtime ragdoll lifecycle failed");
    }
}
void deferred(Renderer2D& renderer) {
    using Status=SceneRequestStatus3D;
    Scratch scratch;
    std::filesystem::copy_file(std::filesystem::path(KIN_TEST_MODELS)/"parts.glb",scratch.path/"parts.glb");
    write(scratch.path/"level.kinscene",level);write(scratch.path/"object.kinprefab",object);
    write(scratch.path/"box.kincollider",R"({"schema":"kin.collider3d/1","shape":{"kind":"box","half_extents":[0.5,0.5,0.5]}})");
    Renderer3D r3d{renderer};Assets3D assets{r3d,scratch.path};
    SceneSession3DOptions options;options.viewport={160,120};options.view.samples=1;options.warm_up=false;
    options.max_pending_commands=4;
    options.physics=World3DConfig{.max_bodies=32,.max_body_pairs=64,.max_contact_constraints=64,
        .temp_memory_bytes=1024*1024,.single_threaded=true};
    options.register_components=[](kin::EcsWorld& world){world.component<Gameplay>("Gameplay");};
    SceneSession3D session{r3d,assets,options};check(session.load("level.kinscene"),"deferred level failed");
    const auto initial=session.physics()->body_count();
    auto commands=session.commands();EcsCommandBuffer buffer;std::string error;
    int constructors=0;
    auto birth=commands.spawn(buffer,"object.kinprefab",{.position=Vec3f{0,5,0},.configure=[&](auto& ctx){
        ++constructors;auto* body=ctx.root.template get_mut<RigidBody3D>();body->gravity_factor=0;body->linear_velocity={3,0,0};ctx.root.set(Gameplay{});
    }});
    check(birth.status()==Status::Pending&&!birth.prefab()&&constructors==0,"request executed at submission");
    check(buffer.flush(*session.world(),error)&&birth.status()==Status::Pending&&session.runtime_prefabs()==0,"request executed at ECS flush");
    check(session.step(1.f/60)&&birth.status()==Status::Succeeded&&constructors==1,"request did not execute at scene boundary");
    auto spawned=session.resolve(birth.prefab().root);
    check(spawned&&spawned.get<Transform3D>()->position.x>.04f&&session.physics()->body_count()==initial+1,"new body missed same-frame physics");
    auto removal=commands.despawn(buffer,birth);
    check(buffer.flush(*session.world(),error)&&session.step(1.f/60)&&removal.status()==Status::Succeeded&&!session.alive(birth.prefab()),"deferred ticket despawn failed");
    check(session.physics()->body_count()==initial,"deferred native teardown leaked");
    auto stale=commands.despawn(buffer,birth.prefab());
    auto missing=commands.spawn(buffer,"missing.kinprefab");
    auto thrown=commands.spawn(buffer,"object.kinprefab",{.configure=[](auto&){throw std::runtime_error("constructor rejected");}});
    check(buffer.flush(*session.world(),error)&&session.step(1.f/60),"individual request failure aborted frame");
    check(stale.status()==Status::Failed&&missing.status()==Status::Failed&&thrown.status()==Status::Failed&&!thrown.error().empty()&&session.runtime_prefabs()==0,"request errors or rollback lost");
    auto transient=commands.spawn(buffer,"object.kinprefab");auto removed=commands.despawn(buffer,transient);
    check(buffer.flush(*session.world(),error)&&session.step(1.f/60)&&transient.status()==Status::Succeeded&&removed.status()==Status::Succeeded&&session.runtime_prefabs()==0,"same-boundary spawn/despawn ordering failed");
    // Unflushed commands count against the fixed capacity. Dropping the buffer
    // cancels tickets and releases both constructor captures and capacity.
    std::array<SceneRequest3D,4> pending;
    auto owner=std::make_shared<int>(1);std::weak_ptr<int> weak=owner;
    for(auto& ticket:pending)ticket=commands.spawn(buffer,"object.kinprefab",{.configure=[owner](auto&){}});
    owner.reset();auto overflow=commands.spawn(buffer,"object.kinprefab");
    check(overflow.status()==Status::Failed&&buffer.size()==4&&!weak.expired(),"pending command budget ignored");
    buffer.clear();
    for(auto& ticket:pending)check(ticket.status()==Status::Cancelled,"discarded ticket remained pending");
    check(weak.expired(),"cancelled constructor captures retained by result");
    // Distinct worker buffers can reserve concurrently; flushing establishes
    // execution order and constructors still run only on this scene thread.
    std::array<EcsCommandBuffer,4> workers;std::array<std::thread,4> threads;
    const auto scene_thread=std::this_thread::get_id();
    for(size_t i=0;i<4;++i)threads[i]=std::thread([&,i]{pending[i]=commands.spawn(workers[i],"object.kinprefab",{
        .position=Vec3f{float(i)*3,5,0},.configure=[&,i](auto&){check(std::this_thread::get_id()==scene_thread,"constructor ran on worker");check(constructors==1+int(i),"scheduler flush ordering changed");++constructors;}});});
    for(auto& thread:threads)thread.join();
    for(auto& worker:workers)check(worker.flush(*session.world(),error),"worker request flush failed");
    check(session.step(1.f/60)&&constructors==5,"parallel submissions failed");
    for(auto& ticket:pending){check(ticket.status()==Status::Succeeded,"parallel result missing");commands.despawn(buffer,ticket);}
    check(buffer.flush(*session.world(),error)&&session.step(1.f/60)&&session.runtime_prefabs()==0,"parallel fixture cleanup failed");
    auto once=commands.spawn(buffer,"object.kinprefab",{.configure=[&](auto&){++constructors;}});
    auto copied=buffer;
    check(buffer.flush(*session.world(),error)&&copied.flush(*session.world(),error)&&session.step(1.f/60)&&constructors==6&&session.runtime_prefabs()==1,"copied buffer executed one request twice");
    check(session.despawn(once.prefab()),"copied buffer cleanup failed");
    kin::EcsWorld foreign;
    auto wrong=commands.spawn(buffer,"object.kinprefab");
    check(buffer.flush(foreign,error)&&wrong.status()==Status::Failed,"foreign world accepted request");
    SceneRequest3D queued,unflushed;
    queued=commands.spawn(buffer,"object.kinprefab");check(buffer.flush(*session.world(),error),"reload queue fixture failed");
    unflushed=commands.spawn(buffer,"object.kinprefab");
    check(session.reload()&&queued.status()==Status::Cancelled&&unflushed.status()==Status::Cancelled,"reload did not cancel all requests");
    check(buffer.flush(*session.world(),error)&&commands.spawn(buffer,"object.kinprefab").status()==Status::Cancelled,"stale endpoint accepted request");
    commands=session.commands();
    auto held=commands.spawn(buffer,"object.kinprefab");check(buffer.flush(*session.world(),error),"prepared fixture failed");
    check(session.prepare("level.kinscene")&&session.step(1.f/60)&&held.status()==Status::Cancelled,"prepared replacement request was executed");session.cancel();
    // Real system callbacks see Pending even after an earlier system's command
    // flush. The constructor runs only once all systems have finished.
    SceneRequest3D system_birth;
    session.world()->systems().register_task({.id="request.submit"},[&](SystemContext& ctx){system_birth=commands.spawn(*ctx.commands,"object.kinprefab");});
    session.world()->systems().register_task({.id="request.observe",.order=1},[&](SystemContext&){
        check(system_birth.status()==Status::Pending&&session.runtime_prefabs()==0,"lifecycle ran before final system");
        check(!session.spawn("object.kinprefab").value_or({}),"direct recursive spawn was accepted");
    });
    check(session.step(1.f/60)&&system_birth.status()==Status::Succeeded,"system request did not complete");
    check(session.reload(),"system fixture reset failed");commands=session.commands();
    SceneRequest3D good,bad;
    session.world()->systems().register_task({.id="request.good"},[&](SystemContext& ctx){good=commands.spawn(*ctx.commands,"object.kinprefab");});
    session.world()->systems().register_task({.id="request.throw",.order=1},[&](SystemContext& ctx){bad=commands.spawn(*ctx.commands,"object.kinprefab");throw std::runtime_error("system failed");});
    check(!session.step(1.f/60)&&good.status()==Status::Cancelled&&bad.status()==Status::Cancelled&&session.runtime_prefabs()==0,"failed systems left executable requests");
    check(session.reload(),"failure fixture reset failed");commands=session.commands();
    session.world()->systems().register_task({.id="request.flush-failure"},[&](SystemContext& ctx){
        good=commands.spawn(*ctx.commands,"object.kinprefab");
        ctx.commands->defer([](kin::EcsWorld&,std::string& error){error="later command failed";return false;});
    });
    check(!session.step(1.f/60)&&good.status()==Status::Cancelled&&session.runtime_prefabs()==0,"partial flush failure retained lifecycle requests");
    check(session.reload(),"partial flush reset failed");commands=session.commands();
    owner=std::make_shared<int>(2);weak=owner;
    auto cancelled=commands.spawn(buffer,"object.kinprefab",{.configure=[owner](auto&){}});owner.reset();session.unload();
    check(cancelled.status()==Status::Cancelled&&weak.expired()&&commands.spawn(buffer,"object.kinprefab").status()==Status::Cancelled,"unload left live requests or constructor captures");
    buffer.clear();
    SceneRequest3D default_request;check(default_request.status()==Status::Invalid&&!default_request.prefab(),"default ticket is live");
}

void projectile_game(Renderer2D& renderer) {
    Renderer3D r3d{renderer};Assets3D assets{r3d,KIN_TEST_SUMMIT};
    SceneSession3DOptions options;options.viewport={160,120};options.view.samples=1;options.warm_up=false;
    options.max_pending_commands=24;options.max_runtime_entities=24;options.max_runtime_prefabs=24;
    options.physics=World3DConfig{.max_bodies=32,.max_body_pairs=128,.max_contact_constraints=128,
        .temp_memory_bytes=1024*1024,.single_threaded=true,.contact_events=true,.max_events=128};
    options.initialize=[](auto& context){projectile_range::install(context);};
    SceneSession3D session{r3d,assets,options};check(session.load("scenes/projectile-range.kinscene"),"projectile level failed");
    check(projectile_range::prepare(session),"projectile preload/pool preparation failed");
    const auto baseline=session.physics()->body_count();size_t peak=0;
    for(int i=0;i<900;++i) {
        if(!session.step(1.f/60)){for(const auto& e:session.diagnostics())std::cerr<<e<<'\n';check(false,"projectile gameplay system failed");}
        peak=std::max(peak,session.runtime_prefabs());
        check(session.runtime_prefabs()<=17&&session.physics()->body_count()==baseline+session.runtime_prefabs(),"gameplay population/native bindings leaked");
    }
    const auto* stats=session.world()->raw().get<projectile_range::Stats>();
    check(stats->post_physics_steps==900,"contact phase skipped or ran more than once per step");
    check(session.world()->systems().snapshot("range.contacts").phase==SystemPhase::PostPhysics,
        "range contacts are not scheduled after native physics");
    std::cout<<"range: shots="<<stats->shots<<" hits="<<stats->hits<<" deaths="<<stats->deaths<<" respawns="<<stats->respawns<<" peak="<<peak<<'\n';
    check(stats->hits>=12&&stats->deaths>=4&&stats->respawns>=4&&stats->shots>stats->hits&&stats->expired>0,"contact damage/destruction/respawn/expiry loop failed");
    check(session.reload()&&session.runtime_prefabs()==0&&session.physics()->body_count()==baseline&&session.world()->raw().get<projectile_range::Stats>()->hits==0,"range reset retained gameplay state");
}
}
int main() {
    bool ready=false;
    try {
        App app;auto& window=app.create_window({.title="runtime-gameplay-tests",.width=160,.height=120,.hidden=true});
        auto backend=make_render_backend(window,false,true);
        if(!backend||backend->name()!="SDL_GPU")throw std::runtime_error("GPU backend required");
        ready=true;Renderer2D renderer{std::move(backend)};runtime(renderer);deferred(renderer);projectile_game(renderer);
        std::cout<<"C++ prefab gameplay lifecycle, handles, typed configuration and cleanup passed\n";
    } catch(const std::exception& error){
        std::cerr<<error.what()<<'\n';const auto* required=std::getenv("KIN_REQUIRE_GPU_TESTS");
        return ready||(required&&std::string_view(required)!="0")?1:77;
    }
}
