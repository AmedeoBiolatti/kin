#include <kin/physics3d/world3d.hpp>
#include <kin/core/instrumentation.hpp>
#include <array>
#include <cmath>
#include <cassert>
#include <iostream>
#include <limits>

namespace {
using namespace kin;
World3DConfig config(){World3DConfig c;c.max_bodies=128;c.temp_memory_bytes=1024*1024;c.single_threaded=true;return c;}
bool close(float a,float b){return std::abs(a-b)<1e-5f;}
void bounded() {
    World3D world{config()};const auto box=Shape3D::box({.3f,.3f,.3f});
    for(int i=0;i<48;++i)world.create_body({.shape=box,.motion=Body3DMotion::Static,.position={0,float(i),0},.user_id=u64(i+1)});
    const auto sensor=world.create_body({.shape=box,.motion=Body3DMotion::Static,.position={0,50,0},.layer=layers3d::Sensor,.sensor=true,.user_id=99});
    world.optimize_broad_phase();const Vec3f from{0,60,0},to{0,-2,0};
    const auto all=world.raycast_all(from,to);assert(all.size()==48);
    std::array<RayHit3D,52> hits;hits.back().user_id=1234;
    auto result=world.raycast_all(from,to,std::span(hits).first(4));
    assert(result.valid&&result.overflow&&result.count==4&&hits.back().user_id==1234);
    for(size_t i=0;i<result.count;++i)assert(hits[i].body==all[i].body&&close(hits[i].distance,all[i].distance)&&hits[i].sub_shape_id==all[i].sub_shape_id);
    result=world.raycast_all(from,to,std::span(hits).first(48));assert(result.valid&&!result.overflow&&result.count==48);
    result=world.raycast_all(from,to,std::span<RayHit3D>{});assert(result.valid&&result.overflow&&result.count==0);
    result=world.raycast_all({4,60,0},{4,-2,0},std::span<RayHit3D>{});assert(result.valid&&!result.overflow&&result.count==0);
    result=world.raycast_all(from,to,hits,{.sensors=true,.ignore_user_id=48});assert(result.count==48&&hits[0].body==sensor&&hits[1].user_id==47);
    assert(!world.raycast(from,to,{.mask=layer_bit(layers3d::Sensor),.sensors=true,.ignore_user_id=99}));
    const auto sphere=Shape3D::sphere(30);const auto overlapping=world.overlap(sphere,{0,24,0},{});assert(overlapping.size()==48);
    std::array<OverlapHit3D,52> overlaps;overlaps.back().user_id=1234;
    auto overlap=world.overlap(sphere,{0,24,0},{},std::span(overlaps).first(4));
    assert(overlap.valid&&overlap.overflow&&overlap.count==4&&overlaps.back().user_id==1234);
    for(size_t i=0;i<overlap.count;++i)assert(overlaps[i].body==overlapping[i].body&&close(overlaps[i].penetration,overlapping[i].penetration));
    overlap=world.overlap(sphere,{0,24,0},{},std::span(overlaps).first(48));assert(!overlap.overflow&&overlap.count==48);
    overlap=world.overlap(sphere,{0,24,0},{},std::span<OverlapHit3D>{});assert(overlap.overflow&&overlap.count==0);
    assert(!world.raycast_all(from,from,hits).valid&&!world.overlap({},from,{},overlaps).valid);
    assert(!world.raycast_all({std::numeric_limits<float>::quiet_NaN(),0,0},to,hits).valid);
    // Warm query shapes and collision code; the bounded result collectors must
    // never allocate a hidden list proportional to all 48+ hits.
    for(int i=0;i<8;++i){world.raycast_all(from,to,hits);world.overlap(sphere,{0,24,0},{},overlaps);}
    AllocationScope allocation;
    for(int i=0;i<1000;++i) {
        auto a=world.raycast_all(from,to,std::span(hits).first(4));auto b=world.overlap(sphere,{0,24,0},{},std::span(overlaps).first(4));
        assert(a.count==4&&a.overflow&&b.count==4&&b.overflow);
    }
    assert(allocation.snapshot().count==0);
}
void compound() {
    World3D world{config()};const auto box=Shape3D::box({.4f,.4f,.4f});
    const std::array<ShapeChild3D,2> children{{{.shape=box,.position={-.2f,0,0}},{.shape=box,.position={.2f,0,0}}}};
    const auto shape=Shape3D::compound(children);world.create_body({.shape=shape,.motion=Body3DMotion::Static});
    std::array<OverlapHit3D,1> out;const auto sphere=Shape3D::sphere(.8f);
    const auto raw=world.overlap(sphere,{0,0,0},{});const auto result=world.overlap(sphere,{0,0,0},{},out);
    assert(raw.size()==1&&result.count==1&&!result.overflow&&close(raw[0].penetration,out[0].penetration)&&raw[0].sub_shape_id==out[0].sub_shape_id);
    const auto rays=world.raycast_all({-3,0,0},{3,0,0});std::array<RayHit3D,8> hits;
    const auto rays_result=world.raycast_all({-3,0,0},{3,0,0},hits);assert(rays_result.count==rays.size()&&!rays_result.overflow);
    for(size_t i=0;i<rays.size();++i)assert(rays[i].sub_shape_id==hits[i].sub_shape_id&&rays[i].sub_shape==hits[i].sub_shape);
    // The compound placed with an offset (one child: Jolt makes a decorated shape of it): the children are still told apart.
    const std::array<ShapeChild3D,1> placed{{{.shape=shape,.position={0,0,4}}}};
    world.create_body({.shape=Shape3D::compound(placed),.motion=Body3DMotion::Static});
    const auto right=world.raycast({.5f,3,4},{.5f,-3,4});const auto left=world.raycast({-.5f,3,4},{-.5f,-3,4});
    assert(right&&right->sub_shape==1&&left&&left->sub_shape==0);
    const auto touched=world.overlap(Shape3D::sphere(.1f),{.5f,0,4},{});assert(touched.size()==1&&touched[0].sub_shape==1);
    const auto swept=world.shape_cast(Shape3D::sphere(.1f),{.5f,3,4},{},{0,-6,0});assert(swept&&swept->sub_shape==1);
}
// shape_cast_all: each body once at its first time of impact, by fraction then body number; the bounded form keeps the prefix.
void event_storage_reuse() {
    auto cfg = config();
    cfg.gravity = {};
    cfg.persist_events = true;
    cfg.contact_events = true;
    World3D world{cfg};
    const auto box = Shape3D::box({1, 1, 1});
    world.create_body({.shape=box, .motion=Body3DMotion::Static, .sensor=true});
    world.create_body({.shape=box, .allow_sleeping=false});
    std::vector<ContactEvent3D> events;
    for (int i=0; i<8; ++i) { world.step(1.f/60); world.drain_events(events); }
    assert(!events.empty());
    AllocationScope allocation;
    for (int i=0; i<32; ++i) {
        world.step(1.f/60);
        world.drain_events(events);
        assert(events.size()==1 && events[0].kind==ContactKind3D::Persist && world.events().empty());
    }
    assert(allocation.snapshot().count==0);
}

void shape_casts() {
    World3D world{config()};const auto box=Shape3D::box({.3f,.3f,.3f});const auto ball=Shape3D::sphere(.2f);
    for(int i=0;i<10;++i)world.create_body({.shape=box,.motion=Body3DMotion::Static,.position={0,0,-1.f-2.f*float(i)},.user_id=u64(i+1)});
    world.optimize_broad_phase();const Vec3f from{0,0,2},along{0,0,-30};
    const auto all=world.shape_cast_all(ball,from,{},along);assert(all.size()==10);
    for(size_t i=0;i<all.size();++i)assert(all[i].user_id==i+1&&(i==0||all[i-1].fraction<all[i].fraction));
    const auto first=world.shape_cast(ball,from,{},along);assert(first&&first->body==all[0].body&&close(first->fraction,all[0].fraction));
    assert(std::abs(all[0].position.z-(-1.f+.3f+.2f))<2e-3f&&std::abs(all[0].normal.z-1.f)<1e-3f);
    std::array<ShapeHit3D,12> hits;hits.back().user_id=1234;
    auto result=world.shape_cast_all(ball,from,{},along,std::span(hits).first(4));
    assert(result.valid&&result.overflow&&result.count==4&&hits.back().user_id==1234);
    for(size_t i=0;i<result.count;++i)assert(hits[i].body==all[i].body&&close(hits[i].fraction,all[i].fraction)&&hits[i].sub_shape_id==all[i].sub_shape_id);
    result=world.shape_cast_all(ball,from,{},along,std::span(hits).first(10));assert(result.valid&&!result.overflow&&result.count==10);
    result=world.shape_cast_all(ball,from,{},along,std::span<ShapeHit3D>{});assert(result.valid&&result.overflow&&result.count==0);
    result=world.shape_cast_all(ball,{5,0,2},{},along,std::span<ShapeHit3D>{});assert(result.valid&&!result.overflow&&result.count==0);
    result=world.shape_cast_all(ball,from,{},along,hits,{.ignore_user_id=1});assert(result.count==9&&hits[0].user_id==2);
    // Invalid input, as shape_cast refuses it: no shape, a mesh, a number that is not finite.
    const std::array<Vec3f,3> tri{{{0,0,0},{1,0,0},{0,0,1}}};const std::array<u32,3> idx{0,1,2};
    assert(!world.shape_cast_all({},from,{},along,hits).valid&&world.shape_cast_all({},from,{},along).empty());
    assert(!world.shape_cast_all(Shape3D::triangle_mesh(tri,idx),from,{},along,hits).valid);
    assert(!world.shape_cast_all(ball,from,{},{0,std::numeric_limits<float>::quiet_NaN(),0},hits).valid);
    // The bounded form allocates nothing once the shapes are warm.
    for(int i=0;i<8;++i)world.shape_cast_all(ball,from,{},along,hits);
    {
        AllocationScope allocation;
        for(int i=0;i<1000;++i){auto r=world.shape_cast_all(ball,from,{},along,std::span(hits).first(4));assert(r.count==4&&r.overflow);}
        assert(allocation.snapshot().count==0);
    }
    // Ties: two boxes in one place are met at the same fraction and come by body number; a compound is one entry, at its
    // nearer child.
    World3D ties{config()};
    const auto a=ties.create_body({.shape=box,.motion=Body3DMotion::Static,.position={0,0,-2},.user_id=1});
    const auto b=ties.create_body({.shape=box,.motion=Body3DMotion::Static,.position={0,0,-2},.user_id=2});
    const std::array<ShapeChild3D,2> children{{{.shape=box,.position={0,0,-1}},{.shape=box,.position={0,0,1}}}};
    const auto pair=ties.create_body({.shape=Shape3D::compound(children),.motion=Body3DMotion::Static,.position={0,0,-8},.user_id=3});
    const auto met=ties.shape_cast_all(ball,{0,0,2},{},{0,0,-20});
    assert(met.size()==3&&met[0].body==a&&met[1].body==b&&met[0].fraction==met[1].fraction&&met[2].body==pair);
    assert(met[2].sub_shape==1&&std::abs(met[2].position.z-(-8.f+1.f+.3f+.2f))<2e-3f);
    std::array<ShapeHit3D,1> one;const auto r1=ties.shape_cast_all(ball,{0,0,2},{},{0,0,-20},one);
    assert(r1.count==1&&r1.overflow&&one[0].body==a);
}
}
int main(){bounded();compound();shape_casts();event_storage_reuse();std::cout<<"bounded deterministic queries, overflow, filters, compound deduplication, shape casts, reused event storage and zero new allocations passed\n";}
