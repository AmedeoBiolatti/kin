#include <kin/scene/scene_session3d.hpp>
#include <kin/ecs/system.hpp>
#include <kin/platform/app.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using namespace kin;
void check(bool value,const char* why){if(!value)throw std::runtime_error(why);}
bool close(float a,float b){return std::abs(a-b)<.0002f;}
struct Scratch {
    std::filesystem::path path=std::filesystem::temp_directory_path()/("kin-pipeline3d-"+
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Scratch(){std::filesystem::create_directories(path);}
    ~Scratch(){std::error_code error;std::filesystem::remove_all(path,error);}
};
constexpr auto level=R"({"entities":[
 {"id":"mover","components":{"Transform3D":{"position":[-10,3,0]},"Collider3D":{"kind":"sphere","radius":0.1},"RigidBody3D":{"gravity_factor":0,"allow_sleeping":false,"linear_velocity":[3,0,0]}}},
 {"id":"target","components":{"Transform3D":{"position":[0.28,3,0]},"Collider3D":{"kind":"sphere","radius":0.1}}},
 {"id":"fx","components":{"Transform3D":{},"ParticleEmitter3D":{"rate":0,"max_particles":4,"lifetime":[1,1],"speed":[0,0]}}}]})";
constexpr auto shot=R"({"id":"shot","root":"root","entities":[{"id":"root","components":{
 "Transform3D":{},"Collider3D":{"kind":"sphere","radius":0.1},"RigidBody3D":{"gravity_factor":0,"sensor":true,"allow_sleeping":false,"linear_velocity":[6,0,0]},
 "ParticleEmitter3D":{"rate":0,"max_particles":4,"lifetime":[1,1],"speed":[0,0]}}}]})";
SceneSession3DOptions options() {
    SceneSession3DOptions options;options.viewport={160,120};options.view.samples=1;options.warm_up=false;
    options.physics=World3DConfig{.max_bodies=8,.max_body_pairs=32,.max_contact_constraints=32,.temp_memory_bytes=1024*1024,.single_threaded=true};
    return options;
}
void pipeline(Renderer2D& renderer) {
    Scratch scratch;std::ofstream(scratch.path/"level.kinscene")<<level;std::ofstream(scratch.path/"shot.kinprefab")<<shot;
    Renderer3D r3d{renderer};Assets3D assets{r3d,scratch.path};SceneSession3D session{r3d,assets,options()};
    const auto ok=[&](bool value,const char* why){if(!value)for(const auto& error:session.diagnostics())std::cerr<<error<<'\n';check(value,why);};
    ok(session.load("level.kinscene"),"pipeline level failed");
    constexpr float dt=1.f/60;
    // No user systems in any phase: host boundaries must still advance once.
    ok(session.step(dt),"empty phase step failed");
    check(close(session.entity("mover").get<Transform3D>()->position.x,-10+3*dt)&&
        close(float(session.entity("fx").get<ParticleEmitter3D>()->system.time()),dt),"empty phases skipped/doubled native simulation");
    ok(session.reload(),"pipeline reset failed");
    auto commands=session.commands();SceneRequest3D birth,removal,late;
    int frame=0,damage=0;std::array<int,system_phase_count> runs{};std::vector<SystemPhase> order;const auto phases=system_phases();
    for(auto phase:system_phases()) {
        const auto id="pipeline."+std::string(system_phase_name(phase));
        session.world()->systems().register_task({.id=id,.phase=phase},[&,phase](SystemContext& context){
            ++runs[std::size_t(phase)];order.push_back(phase);
            auto mover=session.entity("mover"),fx=session.entity("fx");
            switch(phase) {
            case SystemPhase::Input:++frame;break;
            case SystemPhase::Update:
                if(frame==1)birth=commands.spawn(*context.commands,"shot.kinprefab",{.position=Vec3f{.1f,3,0}});
                break;
            case SystemPhase::Physics:
                check(close(mover.get<Transform3D>()->position.x,-10+3*dt*(frame-1)),"physics advanced before Physics systems");
                if(frame==1)check(birth.status()==SceneRequestStatus3D::Pending&&session.physics()->body_count()==2,"birth committed before the physics boundary");
                break;
            case SystemPhase::PostPhysics:
                check(close(mover.get<Transform3D>()->position.x,-10+3*dt*frame),"native physics did not advance exactly once");
                check(close(mover.get<WorldTransform3D>()->matrix.m[12],mover.get<Transform3D>()->position.x),"PostPhysics saw stale world transforms");
                if(frame==1) {
                    auto entity=birth.prefab().root.resolve(*context.world);
                    check(birth.status()==SceneRequestStatus3D::Succeeded&&entity&&close(entity.get<Transform3D>()->position.x,.1f+6*dt),"new birth missed same-step physics");
                    const auto* contacts=context.world->raw().get<PhysicsContacts3D>();bool hit=false;
                    const auto body=entity.get<PhysicsBody3D>()->body,target=session.bodies()->body_of(session.entity("target").raw());
                    if(contacts)for(const auto& contact:contacts->events)
                        if(contact.kind==ContactKind3D::Begin&&((contact.a==body&&contact.b==target)||(contact.b==body&&contact.a==target)))hit=true;
                    check(hit,"first-step contacts missing from PostPhysics");++damage;
                    removal=commands.despawn(*context.commands,birth);
                    check(removal.status()==SceneRequestStatus3D::Pending&&bool(entity),"contact removal executed inside iteration");
                }
                break;
            case SystemPhase::Animation:
                if(frame==1) {
                    check(damage==1&&removal.status()==SceneRequestStatus3D::Succeeded&&!session.resolve(birth.prefab().root)&&session.physics()->body_count()==2,
                        "contact-driven teardown missed the post-physics boundary");
                    fx.get_mut<ParticleEmitter3D>()->system.burst(1);
                }
                check(close(float(fx.get<ParticleEmitter3D>()->system.time()),dt*(frame-1)),"native particles ran before Animation systems");
                break;
            case SystemPhase::Audio:
                check(close(float(fx.get<ParticleEmitter3D>()->system.time()),dt*frame)&&fx.get<ParticleEmitter3D>()->system.count()==1,
                    "Audio did not see current native particle state");break;
            case SystemPhase::PreRender:
                if(frame==1)late=commands.spawn(*context.commands,"shot.kinprefab",{.position=Vec3f{-5,3,0},
                    .configure=[](auto& context){context.root.template get_mut<ParticleEmitter3D>()->system.burst(1);}});
                break;
            case SystemPhase::Editor:
                if(frame==1)check(late.status()==SceneRequestStatus3D::Pending,"late birth committed before the final boundary");
                break;
            default:break;
            }
        });
    }
    for(int i=0;i<3;++i) {
        order.clear();ok(session.step(dt),"phased pipeline failed");
        check(order==std::vector<SystemPhase>(phases.begin(),phases.end()),"phase order/count changed");
        auto entity=session.resolve(late.prefab().root);
        check(late.status()==SceneRequestStatus3D::Succeeded&&entity&&close(entity.get<Transform3D>()->position.x,-5+6*dt*i),"late birth was advanced twice or skipped next physics");
        check(close(float(entity.get<ParticleEmitter3D>()->system.time()),dt*i),"late particles were advanced during their birth step");
    }
    for(auto count:runs)check(count==3,"phase ran more/less than once per step");
    check(damage==1&&session.world()->systems().schedule_snapshot().batches.size()==system_phase_count,"full-frame diagnostic snapshot was lost");
    ok(session.collect(),"phased collection failed");check(session.scene()->particle_sprites().size()==2,"post/late lifecycle changes missed collection");
    // A later graph error must be detected before any phase/native simulation.
    ok(session.reload(),"invalid graph reset failed");commands=session.commands();
    EcsCommandBuffer buffer;std::string error;auto queued=commands.spawn(buffer,"shot.kinprefab");check(buffer.flush(*session.world(),error),"graph failure fixture flush failed");
    int early=0;
    session.world()->systems().register_task({.id="graph.early"},[&](auto&){++early;});
    session.world()->systems().register_task({.id="graph.a",.phase=SystemPhase::PostPhysics,.before={"graph.b"}},[](auto&){});
    session.world()->systems().register_task({.id="graph.b",.phase=SystemPhase::PostPhysics,.before={"graph.a"}},[](auto&){});
    check(!session.step(dt)&&early==0&&queued.status()==SceneRequestStatus3D::Cancelled&&session.entity("mover").get<Transform3D>()->position.x==-10,
        "invalid later graph allowed partial simulation");
    // Failed early systems cancel work and never reach native physics.
    ok(session.reload(),"early failure reset failed");commands=session.commands();
    session.world()->systems().register_task({.id="failure.birth"},[&](auto& context){birth=commands.spawn(*context.commands,"shot.kinprefab");});
    session.world()->systems().register_task({.id="failure.early",.phase=SystemPhase::PrePhysics},[](auto&){throw std::runtime_error("pre-physics failure");});
    check(!session.step(dt)&&birth.status()==SceneRequestStatus3D::Cancelled&&session.runtime_prefabs()==0&&session.entity("mover").get<Transform3D>()->position.x==-10,
        "early failure committed lifecycle work or advanced physics");
    // Native physics and earlier commits cannot be undone by a later failure.
    ok(session.reload(),"post failure reset failed");commands=session.commands();
    session.world()->systems().register_task({.id="failure.birth"},[&](auto& context){birth=commands.spawn(*context.commands,"shot.kinprefab",{.position=Vec3f{-5,3,0}});});
    session.world()->systems().register_task({.id="failure.post",.phase=SystemPhase::PostPhysics},[&](auto& context){removal=commands.despawn(*context.commands,birth);throw std::runtime_error("post-physics failure");});
    check(!session.step(dt)&&birth.status()==SceneRequestStatus3D::Succeeded&&removal.status()==SceneRequestStatus3D::Cancelled&&session.alive(birth.prefab())&&
        close(session.entity("mover").get<Transform3D>()->position.x,-10+3*dt)&&session.entity("fx").get<ParticleEmitter3D>()->system.time()==0,
        "post failure replayed/rolled back physics, leaked work or advanced animation");
    ok(session.reload(),"no-physics reset failed");session.unload();
    auto no_physics=options();no_physics.physics.reset();SceneSession3D visual{r3d,assets,no_physics};check(visual.load("level.kinscene"),"no-physics pipeline level failed");
    int post=0;visual.world()->systems().register_task({.id="visual.post",.phase=SystemPhase::PostPhysics},[&](auto&){++post;});
    check(visual.step(dt)&&post==1&&close(float(visual.entity("fx").get<ParticleEmitter3D>()->system.time()),dt),"no-physics phases/native particles did not run");
}
}
int main() {
    bool ready=false;
    try {
        App app;auto& window=app.create_window({.title="phase-pipeline-tests",.width=160,.height=120,.hidden=true});
        auto backend=make_render_backend(window,false,true);if(!backend||backend->name()!="SDL_GPU")throw std::runtime_error("GPU backend required");
        ready=true;Renderer2D renderer{std::move(backend)};pipeline(renderer);std::cout<<"phase boundaries, same-step contacts, single native steps and failure ordering passed\n";
    } catch(const std::exception& error){std::cerr<<error.what()<<'\n';const auto* required=std::getenv("KIN_REQUIRE_GPU_TESTS");return ready||(required&&std::string_view(required)!="0")?1:77;}
}
