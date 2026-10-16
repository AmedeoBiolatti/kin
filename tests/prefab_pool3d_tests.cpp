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
#include <stdexcept>

namespace {
using namespace kin;
void check(bool value,const char* why){if(!value)throw std::runtime_error(why);}
struct Scratch {
    std::filesystem::path path=std::filesystem::temp_directory_path()/("kin-pool3d-"+
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Scratch(){std::filesystem::create_directories(path);}
    ~Scratch(){std::error_code error;std::filesystem::remove_all(path,error);}
};
void write(const std::filesystem::path& path,std::string_view text){std::ofstream(path)<<text;}
struct Game {int health=3;float timer=0;std::shared_ptr<int> resource;};
constexpr auto level=R"({"entities":[{"id":"floor","components":{"Transform3D":{"position":[0,-1,0]},"Collider3D":{"half_extents":[20,0.5,20]}}}]})";
constexpr auto object=R"({"id":"pooled","root":"root","entities":[
 {"id":"root","components":{"Transform3D":{},"ModelRenderer3D":{"asset":"parts.glb"},"ModelMaterials3D":{"asset":"colors.kinmaterials"},"Collider3D":{"asset":"box.kincollider"},"RigidBody3D":{"motion":"dynamic","gravity_factor":0,"linear_velocity":[1,2,0]},"PoolLink":{"target":"fx"}}},
 {"id":"fx","parent":"root","components":{"Transform3D":{},"ParticleEmitter3D":{"seed":17,"rate":10,"max_particles":8,"lifetime":[0.1,0.2],"texture":"pixel.png"}}}]})";
SceneSession3DOptions settings() {
    SceneSession3DOptions options;options.viewport={160,120};options.view.samples=1;options.warm_up=false;
    options.max_spawn_entities=4;options.max_runtime_entities=32;options.max_runtime_prefabs=16;
    options.physics=World3DConfig{.max_bodies=32,.max_body_pairs=64,.max_contact_constraints=64,.temp_memory_bytes=1024*1024,.single_threaded=true};
    options.register_components=[](kin::EcsWorld& world){
        world.component<Game>("PoolGame");world.components().data("PoolLink").field_entity_ref("target");
        world.systems().register_native<Game>({.id="pool.timer",.run=[](kin::EcsEntity,Game& game,SystemContext& context){game.timer+=context.dt;}});
    };return options;
}
long rss_kib() {
    std::ifstream input("/proc/self/status");std::string line;
    while(std::getline(input,line))if(line.starts_with("VmRSS:"))return std::stol(line.substr(6));return 0;
}
void pooling(Renderer2D& renderer,bool benchmark) {
    Scratch scratch;
    std::filesystem::copy_file(std::filesystem::path(KIN_TEST_MODELS)/"parts.glb",scratch.path/"parts.glb");
    std::filesystem::copy_file(std::filesystem::path(KIN_TEST_MODELS)/"shared pixel.png",scratch.path/"pixel.png");
    write(scratch.path/"level.kinscene",level);write(scratch.path/"object.kinprefab",object);
    write(scratch.path/"colors.kinmaterials",R"({"schema":"kin.materials3d/1","material":{"color":[220,110,90,255],"texture":{"asset":"pixel.png"}}})");
    write(scratch.path/"box.kincollider",R"({"schema":"kin.collider3d/1","shape":{"kind":"box","half_extents":[0.2,0.2,0.2]}})");
    Renderer3D r3d{renderer};Assets3D assets{r3d,scratch.path};SceneSession3D session{r3d,assets,settings()};
    const auto ok=[&](bool value,const char* why){if(!value)for(const auto& error:session.diagnostics())std::cerr<<error<<'\n';check(value,why);};
    ok(session.load("level.kinscene"),"pool level failed");const auto baseline=session.physics()->body_count();
    check(!session.preload("absent.kinprefab")&&session.reserved_entities()==0,"failed preload leaked");
    auto ready=session.preload("object.kinprefab");ok(bool(ready),"preload failed");
    check(session.runtime_prefabs()==0&&session.reserved_entities()==2&&session.physics()->body_count()==baseline,"preload entered simulation");
    bool reject=false;int resets=0,returns=0;
    ScenePrefabPoolOptions3D options{.capacity=2,
        .reset=[&](auto& context){++resets;context.root.set(Game{});if(reject)throw std::runtime_error("reset rejected");},
        .release=[&](auto& context){++returns;context.root.set(Game{});}};
    auto pool=session.create_pool(ready,options);ok(bool(pool),"pool warm-up failed");
    check(resets==2&&session.pool_stats(pool).available==2&&session.reserved_entities()==6&&session.runtime_entities()==0,"pool warm-up counts wrong");
    check(!session.release(ready),"prepared reference released beneath pool");
    auto one=session.spawn(pool,{.position=Vec3f{-3,5,0}}),two=session.spawn(pool,{.position=Vec3f{3,5,0}});
    ok(bool(one)&&bool(two),"pool acquisition failed");
    const auto stable=session.world()->entities().all().size();
    check(!session.spawn(pool)&&session.pool_stats(pool).exhausted==1&&session.world()->entities().all().size()==stable,"pool grew at capacity");
    auto entity=session.resolve(one.root);auto child=session.entity(one,"fx");const auto raw_id=entity.id();
    check(std::get<ComponentEntityRef>(session.world()->components().snapshot(entity,"PoolLink")->fields[0].value).id==child.entity.id,"prepared copy retained reference prototype IDs");
    const auto original_body=session.bodies()->body_of(entity.raw());
    session.physics()->set_linear_velocity(original_body,{9,8,7});session.physics()->set_angular_velocity(original_body,{1,2,3});
    entity.get_mut<Game>()->health=0;entity.get_mut<Game>()->timer=99;
    auto token=std::make_shared<int>(42);std::weak_ptr<int> weak=token;entity.get_mut<Game>()->resource=token;token.reset();
    session.resolve(child).get_mut<ParticleEmitter3D>()->system.burst(4);
    entity.get_mut<ModelRenderer3D>()->tint=Color::rgb(0,255,0);
    ok(session.step(1.f/60),"pool step failed");ok(session.collect(),"pool collection failed");
    check(!session.scene()->draws().empty()&&session.scene()->instances().size()>=2&&session.scene()->particle_sprites().size()>=4,"reused slots lost model/particle draw participation");
    ok(session.despawn(one),"pool return failed");
    check(!session.resolve(one.root)&&!one.root.resolve(*session.world())&&!session.resolve(child)&&weak.expired(),"old lease or per-lease resources survived return");
    check(session.physics()->body_count()==baseline+1&&!session.physics()->contains(original_body)&&session.pool_stats(pool).active==1,"idle slot retained contacts/body");
    ok(session.step(1.f/60),"idle pool step failed");
    check(session.world()->entities().resolve(one.root.entity).get<Game>()->timer==0,"idle pool entity ran gameplay systems");
    auto again=session.spawn(pool,{.position=Vec3f{-3,5,0}});ok(bool(again),"pool reuse failed");
    ok(session.collect(),"reused pool collection failed");
    check(session.scene()->instances().size()>=2,"reacquired pool slot disappeared from rendering");
    entity=again.root.resolve(*session.world());const auto next_body=session.bodies()->body_of(entity.raw());
    check(entity.id()==raw_id&&again.id!=one.id&&!session.resolve(one.root)&&!one.root.resolve(*session.world()),"pooled handle ABA protection failed");
    check(entity.get<Game>()->health==3&&entity.get<Game>()->timer==0&&session.physics()->linear_velocity(next_body)==Vec3f{1,2,0}&&
        session.physics()->angular_velocity(next_body)==Vec3f{}&&session.resolve(session.entity(again,"fx")).get<ParticleEmitter3D>()->system.count()==0&&
        session.resolve(session.entity(again,"fx")).get<ParticleEmitter3D>()->system.time()==0,"native/game/particle state was not reset");
    check(entity.get<ModelRenderer3D>()->tint==colors::white&&entity.get<ModelMaterials3D>()->binding==session.resolve(two.root).get<ModelMaterials3D>()->binding,"material/render state did not reset/share");
    check(!session.despawn(one),"old lease returned a new occupant");
    ok(session.despawn(again)&&session.despawn(two),"reuse cleanup failed");
    reject=true;check(!session.spawn(pool)&&session.pool_stats(pool).available==2&&session.physics()->body_count()==baseline,"failed reset leaked an active slot");reject=false;
    check(!session.spawn(pool,{.scale=Vec3f{2,2,2}})&&session.pool_stats(pool).available==2,"pool accepted changed collision scale");
    check(!session.spawn(pool,{.configure=[](auto& context){context.root.template get_mut<ModelRenderer3D>()->asset.id="missing.glb";}}),"pool accepted an unprepared asset change");
    check(!session.spawn(pool,{.configure=[](auto& context){context.entity("fx").template get_mut<ParticleEmitter3D>()->system.desc.max_particles=1000000;}})&&session.step(1.f/60),"failed reset left invalid particle configuration");
    auto attached=session.spawn(pool);auto foreign_child=session.world()->entity();foreign_child.child_of(session.resolve(attached.root));
    check(!session.despawn(attached)&&session.alive(attached)&&!session.release(pool),"pool destroyed a foreign child");
    session.world()->entities().detach_parent(foreign_child);session.world()->entities().destroy(foreign_child);ok(session.despawn(attached),"foreign-child fixture cleanup failed");
    auto foreign=pool;++foreign.session;check(!session.spawn(foreign)&&!session.release(foreign),"foreign pool accepted");
    auto foreign_ready=ready;++foreign_ready.session;check(!session.spawn(foreign_ready)&&!session.release(foreign_ready),"foreign preload accepted");
    check(!session.create_pool(ready,{.capacity=0})&&!session.create_pool(ready,{.capacity=1000000}),"pool budget ignored");
    EcsCommandBuffer commands;std::string error;
    auto request=session.commands().spawn(commands,pool,{.position=Vec3f{0,5,0}});
    check(commands.flush(*session.world(),error)&&request.status()==SceneRequestStatus3D::Pending,"pool request executed at flush");
    ok(session.step(1.f/60),"queued pool step failed");check(request.status()==SceneRequestStatus3D::Succeeded,"queued pool acquisition failed");
    auto removal=session.commands().despawn(commands,request);
    check(commands.flush(*session.world(),error)&&session.step(1.f/60)&&removal.status()==SceneRequestStatus3D::Succeeded&&!request.prefab().root.resolve(*session.world()),"queued pool return failed");
    // Warm art in all three cases; this compares file recipe/binding work against
    // frozen prepared spawning and complete ECS slot reuse, not cold disk/GPU upload.
    const auto loads=assets.stats();
    const int cycles=benchmark?10000:1000;
    std::ofstream csv;
    if(const char* file=std::getenv("KIN_POOL_PROFILE")){csv.open(file);csv<<"mode,cycle,acquire_us,return_us,total_us,entities,bodies,rss_kib\n";}
    using Clock=std::chrono::steady_clock;
    for(int mode=0;mode<3;++mode) {
        std::vector<double> acquire,release,total;acquire.reserve(cycles);release.reserve(cycles);total.reserve(cycles);
        for(int i=-32;i<cycles;++i) {
            session.world()->events().begin_frame();
            SceneSpawn3D spawn{.position=Vec3f{0,5,0}};if(mode!=2)spawn.configure=[](auto& context){context.root.set(Game{});};
            const auto start=Clock::now();
            auto instance=mode==0?session.spawn("object.kinprefab",spawn):mode==1?session.spawn(ready,spawn):session.spawn(pool,spawn);
            const auto born=Clock::now();ok(bool(instance),"profile acquisition failed");
            ok(session.despawn(instance),"profile return failed");const auto done=Clock::now();
            check(session.world()->entities().all().size()==stable&&session.physics()->body_count()==baseline&&session.runtime_prefabs()==0,"repeated pool lifecycle grew entity/native counts");
            if(i<0)continue;
            const double a=std::chrono::duration<double,std::micro>(born-start).count(),b=std::chrono::duration<double,std::micro>(done-born).count();
            acquire.push_back(a);release.push_back(b);total.push_back(a+b);
            if(csv)csv<<mode<<','<<i<<','<<a<<','<<b<<','<<a+b<<','<<stable<<','<<baseline<<','<<(i%1000==0?rss_kib():0)<<'\n';
        }
        const auto report=[&](std::vector<double> values,int percent){std::ranges::sort(values);return values[(values.size()-1)*percent/100];};
        std::cout<<"pool profile mode="<<mode<<" cycles="<<cycles<<" acquire_p50_us="<<report(acquire,50)<<" acquire_p95_us="<<report(acquire,95)
            <<" return_p95_us="<<report(release,95)<<" total_p50_us="<<report(total,50)<<" total_p95_us="<<report(total,95)<<" rss_kib="<<rss_kib()<<'\n';
    }
    check(assets.stats().model_loads==loads.model_loads&&assets.stats().texture_loads==loads.texture_loads&&assets.stats().material_loads==loads.material_loads,"warm lifecycle reloaded model/material/texture assets");
    // A true frozen preload/pool must remain usable without any source files.
    for(const auto* file:{"object.kinprefab","parts.glb","box.kincollider","colors.kinmaterials","pixel.png"})std::filesystem::rename(scratch.path/file,scratch.path/(std::string(file)+".away"));
    for(int i=0;i<1000;++i){auto instance=session.spawn(pool);ok(bool(instance)&&session.despawn(instance),"pool touched missing source files");}
    auto frozen=session.spawn(ready);ok(bool(frozen)&&session.despawn(frozen),"prepared spawn touched missing source files");
    for(const auto* file:{"object.kinprefab","parts.glb","box.kincollider","colors.kinmaterials","pixel.png"})std::filesystem::rename(scratch.path/(std::string(file)+".away"),scratch.path/file);
    check(session.world()->entities().all().size()==stable&&session.pool_stats(pool).reused>=2000,"reuse stress failed");
    auto pending=session.commands().spawn(commands,pool);check(session.reload(),"pool reload failed");
    check(pending.status()==SceneRequestStatus3D::Cancelled&&!session.spawn(pool)&&!session.spawn(ready)&&session.reserved_entities()==0,"reload retained pool/preload ownership");commands.clear();
    ready=session.preload("object.kinprefab");pool=session.create_pool(ready,options);ok(bool(pool),"release fixture failed");
    auto active=session.spawn(pool);std::weak_ptr<const Model> model=session.resolve(active.root).get<ModelRenderer3D>()->model;
    ok(session.release(pool),"explicit pool release failed");check(!session.resolve(active.root)&&session.reserved_entities()==2&&!model.expired(),"pool release left slots or dropped prepared art");
    pool=session.create_pool(ready,options);active=session.spawn(pool);ok(bool(active),"damaged pool fixture failed");
    session.world()->entities().destroy(session.resolve(active.root));
    ok(session.release(pool),"pool could not clean remaining ownership after direct native destruction");
    ok(session.release(ready),"explicit preload release failed");check(model.expired()&&session.reserved_entities()==0&&session.physics()->body_count()==baseline,"last prepared owner pinned model/native objects");
    // Disabled retained emitters count toward memory budgets too.
    auto limited=settings();limited.particle_limits.total_particles=16;
    SceneSession3D small{r3d,assets,limited};check(small.load("level.kinscene"),"small pool level failed");
    auto small_ready=small.preload("object.kinprefab");check(bool(small_ready)&&!small.create_pool(small_ready,{.capacity=2})&&small.reserved_entities()==2,"inactive particle budget or warm-up rollback failed");
    auto constrained=settings();constrained.physics->max_bodies=2;
    SceneSession3D full{r3d,assets,constrained};check(full.load("level.kinscene"),"full physics level failed");
    auto full_ready=full.preload("object.kinprefab");auto full_pool=full.create_pool(full_ready,{.capacity=1});auto occupying=full.spawn(full_ready);
    check(bool(occupying)&&!full.spawn(full_pool)&&full.pool_stats(full_pool).available==1&&full.physics()->body_count()==2,"native capacity failure damaged pool/live bodies");
    check(full.despawn(occupying)&&bool(full.spawn(full_pool)),"pool did not recover after native capacity refusal");
    // Shared content-pack loader follows the same frozen preload/pool path.
    Scratch storage;
    check(write_content_pack(scratch.path,storage.path/"game.kinpak").ok,"pool pack creation failed");
    const auto mount=storage.path/"mounted";mount_content_pack(mount,ContentPack::open(storage.path/"game.kinpak"));
    {Assets3D packed_assets{r3d,mount};SceneSession3D packed{r3d,packed_assets,settings()};
     check(packed.load("level.kinscene"),"packed pool level failed");auto p=packed.preload("object.kinprefab");auto pool=packed.create_pool(p,{.capacity=1});auto lease=packed.spawn(pool);
     check(bool(lease)&&packed.despawn(lease)&&packed.release(pool)&&packed.release(p)&&packed.reserved_entities()==0,"packed pool lifecycle failed");}
    unmount_content_pack(mount);
    if(r3d.capabilities().skinning) {
        Assets3D art{r3d,KIN_TEST_SUMMIT};SceneSession3D animated{r3d,art,settings()};
        check(animated.load("scenes/projectile-range.kinscene"),"animated pool level failed");
        auto p=animated.preload("prefabs/survivor.kinprefab");auto pool=animated.create_pool(p,{.capacity=1});auto lease=animated.spawn(pool);
        check(bool(lease)&&animated.step(.1f),"animated pool failed");auto actor=animated.resolve(lease.root);
        check(actor.get<Animator3D>()->player.time()>0,"pooled animation did not advance");
        actor.get_mut<Animator3D>()->params.set_float("pool-test",7.f);actor.get_mut<Animator3D>()->state="old-state";
        check(animated.despawn(lease),"animated pool return failed");lease=animated.spawn(pool);actor=animated.resolve(lease.root);
        check(actor&&actor.get<Animator3D>()->player.time()==0&&actor.get<Animator3D>()->state!="old-state"&&actor.get<Animator3D>()->params.get_float("pool-test")==0,"animation state leaked across leases");
    }
}
}
int main(int argc,char**) {
    bool ready=false;
    try {
        App app;auto& window=app.create_window({.title="prefab-pool-tests",.width=160,.height=120,.hidden=true});
        auto backend=make_render_backend(window,false,true);if(!backend||backend->name()!="SDL_GPU")throw std::runtime_error("GPU backend required");
        ready=true;Renderer2D renderer{std::move(backend)};pooling(renderer,argc>1);std::cout<<"preload/pool lifecycle, resets, budgets, reuse and release passed\n";
    } catch(const std::exception& error){std::cerr<<error.what()<<'\n';const auto* required=std::getenv("KIN_REQUIRE_GPU_TESTS");return ready||(required&&std::string_view(required)!="0")?1:77;}
}
