#include <kin/physics3d/world3d.hpp>
#include <kin/core/instrumentation.hpp>
#include <array>
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
}
}
int main(){bounded();compound();std::cout<<"bounded deterministic queries, overflow, filters, compound deduplication and zero new allocations passed\n";}
