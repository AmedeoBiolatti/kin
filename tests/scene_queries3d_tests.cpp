#include <kin/scene/scene_session3d.hpp>
#include <kin/platform/app.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/core/instrumentation.hpp>
#include <array>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <thread>

namespace {
using namespace kin;
void check(bool value,const char* why){if(!value)throw std::runtime_error(why);}
struct Scratch {
    std::filesystem::path path=std::filesystem::temp_directory_path()/("kin-query3d-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Scratch(){std::filesystem::create_directories(path);}
    ~Scratch(){std::error_code error;std::filesystem::remove_all(path,error);}
};
constexpr auto level=R"({"entities":[
 {"id":"target","components":{"Transform3D":{"position":[2,1,0]},"Collider3D":{"half_extents":[0.2,0.2,0.2]}}},
 {"id":"sensor","components":{"Transform3D":{"position":[1,1,0]},"Collider3D":{"radius":0.2,"kind":"sphere"},"RigidBody3D":{"motion":"static","sensor":true,"layer":4}}},
 {"id":"player","components":{"Transform3D":{"position":[-2,0,0]},"CharacterController3D":{}}}]})";
constexpr auto prefab=R"({"id":"box","root":"box","entities":[{"id":"box","components":{"Transform3D":{},"Collider3D":{"half_extents":[0.2,0.2,0.2]}}}]})";
SceneSession3DOptions options() {
    SceneSession3DOptions options;options.viewport={160,120};options.view.samples=1;options.warm_up=false;
    options.physics=World3DConfig{.max_bodies=16,.max_body_pairs=32,.max_contact_constraints=32,.temp_memory_bytes=1024*1024,.single_threaded=true};return options;
}
void queries(Renderer2D& renderer) {
    Scratch scratch;std::ofstream(scratch.path/"level.kinscene")<<level;std::ofstream(scratch.path/"box.kinprefab")<<prefab;
    Renderer3D r3d{renderer};Assets3D assets{r3d,scratch.path};auto config=options();SceneQueries3D captured;
    config.initialize=[&](auto& context){captured=context.queries;check(!captured,"candidate queries were available before activation");};
    SceneSession3D session{r3d,assets,config};check(!session.queries()&&session.load("level.kinscene"),"scene query load failed");
    auto query=session.queries();check(bool(query)&&bool(captured),"initialized endpoint did not activate");
    const Vec3f from{-1,1,0},to{4,1,0};
    auto hit=query.raycast(from,to);check(hit&&query.resolve(*hit).id()==session.entity("target").id(),"closest mapped hit incorrect");
    const auto target=*hit;auto sensed=query.raycast(from,to,{.sensors=true});
    check(sensed&&query.resolve(*sensed).id()==session.entity("sensor").id(),"sensor option ignored");
    auto ignored=query.raycast(from,to,{.ignore=sensed->entity,.sensors=true});check(ignored&&ignored->entity==target.entity,"checked entity ignore failed");
    check(!query.raycast(from,to,{.mask=layer_bit(layers3d::Sensor)}),"sensor hit without opt-in");
    std::array<SceneRayHit3D,3> rays;rays[2].physics.user_id=1234;
    auto result=query.raycast_all(from,to,std::span(rays).first(1),{.sensors=true});
    check(result.valid&&result.count==1&&result.overflow&&query.resolve(rays[0]).id()==session.entity("sensor").id()&&rays[2].physics.user_id==1234,"bounded scene ray order/overflow/write range wrong");
    result=query.raycast_all(from,to,std::span<SceneRayHit3D>{},{.sensors=true});check(result.valid&&result.overflow&&result.count==0,"zero-capacity scene ray probe failed");
    auto foreign=target.entity;++foreign.session;
    check(!query.raycast_all(from,to,rays,{.ignore=foreign}).valid,"foreign ignore handle was accepted");
    SceneEntity3D malformed;malformed.entity.generation=1;
    check(!query.raycast_all(from,to,rays,{.ignore=malformed}).valid,"partial ignore handle was accepted");
    const auto sphere=Shape3D::sphere(.8f);std::array<SceneOverlapHit3D,4> overlaps;
    result=query.overlap(sphere,{1.5f,1,0},{},std::span(overlaps).first(1),{.sensors=true});check(result.valid&&result.count==1&&result.overflow&&bool(query.resolve(overlaps[0])),"bounded scene overlap failed");
    const auto cast=query.shape_cast(Shape3D::sphere(.1f),from,{},to-from);check(cast&&cast->entity==target.entity&&query.resolve(*cast).id()==session.entity("target").id(),"scene shape cast failed");
    const auto player=query.handle(session.entity("player"));
    check(query.raycast({-2,3,0},{-2,-1,0},{.mask=layer_bit(layers3d::Character)}).has_value(),"character query fixture missing");
    check(!query.raycast({-2,3,0},{-2,-1,0},{.ignore=player}),"ignore player did not exclude native capsule");
    const auto native=session.physics()->create_body({.shape=Shape3D::sphere(.1f),.motion=Body3DMotion::Static,.position={0,1,0},.user_id=session.entity("target").id()});
    const auto unmapped=query.raycast(from,to);check(unmapped&&unmapped->physics.body==native&&!unmapped->entity&&!query.resolve(*unmapped),"unmapped/spoofed native body did not remain an obstruction");
    session.physics()->destroy_body(native);
    // Stale native hits fail even if their scene entity and user id remain live.
    check(session.reconfigure<Collider3D>(target.entity,[](auto& collider){collider.half_extents={.25f,.25f,.25f};}),"query body rebuild failed");
    check(target.entity.resolve(*session.world())&&!query.resolve(target),"old native hit resolved after body rebuild");
    auto prepared=session.preload("box.kinprefab");auto pool=session.create_pool(prepared,{.capacity=1});check(bool(pool),"query pool creation failed");
    auto lease=session.spawn(pool,{.position=Vec3f{0,3,0}});check(bool(lease),"query pool acquisition failed");
    const auto pooled=query.raycast({-1,3,0},{1,3,0});check(pooled&&pooled->entity==lease.root&&session.despawn(lease),"query pool hit/release failed");
    auto renewed=session.spawn(pool,{.position=Vec3f{0,3,0}});check(renewed.root.entity==lease.root.entity&&renewed.root!=lease.root&&!query.resolve(*pooled),"pooled hit aliasing a renewed lease");
    const auto fresh=query.raycast({-1,3,0},{1,3,0});check(fresh&&query.resolve(*fresh).id()==session.resolve(renewed.root).id(),"new lease hit did not resolve");
    result=query.raycast_all(from,to,rays,{.ignore=pooled->entity});check(!result.valid&&result.count==0,"stale ignore handle did not fail closed");
    bool rejected=false;std::thread worker([&]{rejected=!query&&!query.raycast_all(from,to,rays).valid;});worker.join();check(rejected,"worker-thread native queries were not rejected");
    // Warm typed bridge caches, then fail operator new on this caller thread.
    query.raycast_all(from,to,rays);query.overlap(sphere,{1.5f,1,0},{},overlaps);
    {
        AllocationScope guard{true};
        for(int i=0;i<1000;++i) {
            check(query.raycast_all(from,to,rays).valid,"allocation guard ray failed");
            check(query.overlap(sphere,{1.5f,1,0},{},overlaps).valid,"allocation guard overlap failed");
        }
    }
    check(!session.prepare("missing.kinscene")&&bool(query),"failed candidate invalidated active query endpoint");
    check(session.reload()&&!query&&!captured.resolve(*fresh)&&!query.raycast_all(from,to,rays).valid&&!fresh->entity.resolve(*session.world()),"reload retained old query endpoint/hit");
    auto live=session.queries();session.unload();check(!live&&!live.raycast(from,to),"unload retained a query world");
    auto no_physics=options();no_physics.physics.reset();SceneSession3D visual{r3d,assets,no_physics};check(visual.load("level.kinscene")&&!visual.queries(),"no-physics scene exposed queries");
}
}
int main() {
    bool ready=false;
    try {
        App app;auto& window=app.create_window({.title="scene-query-tests",.width=160,.height=120,.hidden=true});
        auto backend=make_render_backend(window,false,true);if(!backend||backend->name()!="SDL_GPU")throw std::runtime_error("GPU backend required");
        ready=true;Renderer2D renderer{std::move(backend)};queries(renderer);std::cout<<"scene query filters, native/scene identity, bounded storage, lifecycle and allocation guards passed\n";
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';const auto* required=std::getenv("KIN_REQUIRE_GPU_TESTS");return ready||(required&&std::string_view(required)!="0")?1:77;}
}
