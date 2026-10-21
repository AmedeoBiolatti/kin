#include "result3d_helpers.hpp"
// Skeletons, clips, poses and the glTF skin reader (kin/render3d/skeleton3d.hpp, animation3d.hpp, model.hpp), without a
// GPU: keyframe sampling against hand-computed glTF values, blends, additive clips, masks, joint matrices of a known
// chain, root motion, the player's crossfade, bounds that no posed vertex leaves, and the loader on glTF files made in
// code.

#include "../examples/sponza_walk/ik_rig.hpp"
#include "skin_fixtures.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <kin/assets/content.hpp>
#include <kin/ecs/render3d.hpp>
#include <kin/ecs/world.hpp>
#include <kin/platform/log.hpp>
#include <kin/render3d/animation3d.hpp>
#include <kin/render3d/ik3d.hpp>
#include <kin/render3d/model3d.hpp>
#include <kin/render3d/skeleton3d.hpp>
#include <random>
#include <span>

namespace {

using namespace kin;
using skin_fixtures::GltfOptions;

bool close(float a, float b, float eps = 1e-5f) {
    return std::fabs(a - b) <= eps;
}
bool close(Vec3f a, Vec3f b, float eps = 1e-5f) {
    return close(a.x, b.x, eps) && close(a.y, b.y, eps) && close(a.z, b.z, eps);
}
bool close(Quatf a, Quatf b, float eps = 1e-5f) {
    // The same rotation whichever sign.
    const float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    return std::fabs(std::fabs(d) - 1.0f) <= eps;
}
float length_of(Quatf q) {
    return std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
}

Quatf rz(float degrees) {
    return Quatf::axis_angle({0, 0, 1}, degrees);
}

// --- sampling
// --------------------------------------------------------------------------------------------------------------

void test_sampling_linear_and_step() {
    Track3D<Vec3f> t;
    t.interpolation = Interpolation3D::Linear;
    t.times = {0.0f, 1.0f, 3.0f};
    t.values = {{0, 0, 0}, {2, 4, 6}, {2, 0, 0}};
    const Vec3f fallback{9, 9, 9};
    assert(close(sample_track(t, 0.5f, fallback), {1, 2, 3}));
    assert(close(sample_track(t, 2.0f, fallback), {2, 2, 3}));  // halfway between keys 1 and 3 s
    assert(close(sample_track(t, -1.0f, fallback), {0, 0, 0})); // before the first key: the first value
    assert(close(sample_track(t, 3.0f, fallback), {2, 0, 0}));  // at and after the last: the last value
    assert(close(sample_track(t, 100.0f, fallback), {2, 0, 0}));
    assert(close(sample_track(t, 1.0f, fallback), {2, 4, 6})); // exactly on a key
    t.interpolation = Interpolation3D::Step;
    assert(close(sample_track(t, 0.999f, fallback), {0, 0, 0}));
    assert(close(sample_track(t, 1.0f, fallback), {2, 4, 6}));
    assert(close(sample_track(t, 2.9f, fallback), {2, 4, 6}));
    // Empty or ill-formed tracks leave the fallback.
    Track3D<Vec3f> none;
    assert(close(sample_track(none, 1.0f, fallback), fallback));
    Track3D<Vec3f> bad;
    bad.times = {0.0f, 1.0f};
    bad.values = {{1, 1, 1}};
    assert(!bad.valid() && close(sample_track(bad, 0.5f, fallback), fallback));
    // One key: that value always.
    Track3D<Vec3f> one;
    one.times = {5.0f};
    one.values = {{3, 3, 3}};
    assert(close(sample_track(one, 0.0f, fallback), {3, 3, 3}) && close(sample_track(one, 9.0f, fallback), {3, 3, 3}));
    // Two keys at the same time do not divide by zero.
    Track3D<Vec3f> twin;
    twin.times = {1.0f, 1.0f, 2.0f};
    twin.values = {{1, 0, 0}, {5, 0, 0}, {9, 0, 0}};
    const Vec3f at = sample_track(twin, 1.0f, fallback);
    assert(std::isfinite(at.x) && at.x >= 1.0f && at.x <= 9.0f);
}

void test_sampling_cubic_spline() {
    // glTF's spline between keys k and k + 1 (spec, appendix C): with s = (t - t_k) / dt,
    //   p = (2s^3 - 3s^2 + 1) v_k + (s^3 - 2s^2 + s) dt b_k + (-2s^3 + 3s^2) v_k+1 + (s^3 - s^2) dt a_k+1
    // each key stored as in-tangent a, value v, out-tangent b. Worked by hand for dt = 1, s = 0.5: the weights are 0.5,
    // 0.125, 0.5, -0.125.
    Track3D<Vec3f> t;
    t.interpolation = Interpolation3D::CubicSpline;
    t.times = {0.0f, 1.0f};
    t.values = {{7, 7, 7}, {0, 0, 0}, {1, 2, 0},  // key 0: in (unused), value, out
                {0, 0, 4}, {2, 4, 6}, {7, 7, 7}}; // key 1: in, value, out (unused)
    Vec3f p = sample_track(t, 0.5f, {});
    // x: 0.5*0 + 0.125*1 + 0.5*2 + (-0.125)*0 = 1.125; y: 0 + 0.25 + 2 + 0 = 2.25; z: 0 + 0 + 3 + (-0.125*4) = 2.5
    assert(close(p, {1.125f, 2.25f, 2.5f}));
    // On the keys: the values (not the tangents).
    assert(close(sample_track(t, 0.0f, {}), {0, 0, 0}) && close(sample_track(t, 1.0f, {}), {2, 4, 6}));
    assert(close(sample_track(t, -3.0f, {}), {0, 0, 0}) && close(sample_track(t, 9.0f, {}), {2, 4, 6}));
    // dt scales the tangents: the same keys over two seconds, sampled at the middle (s = 0.5): tangent terms twice as
    // big.
    t.times = {0.0f, 2.0f};
    p = sample_track(t, 1.0f, {});
    assert(close(p, {0.125f * 2 * 1 + 1.0f, 0.125f * 2 * 2 + 2.0f, 3.0f - 0.125f * 2 * 4}));
    // s = 0.25: h00 = 2/64 - 3/16 + 1 = 0.84375, h10 = 1/64 - 2/16 + 1/4 = 0.140625, h01 = 0.15625, h11 = 1/64 - 1/16 =
    // -0.046875.
    t.times = {0.0f, 1.0f};
    p = sample_track(t, 0.25f, {});
    assert(close(p.x, 0.140625f * 1 + 0.15625f * 2));
    // A spline through straight keys with zero tangents is a smoothstep: slower at the ends than a lerp.
    Track3D<Vec3f> ease;
    ease.interpolation = Interpolation3D::CubicSpline;
    ease.times = {0.0f, 1.0f};
    ease.values = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {1, 0, 0}, {0, 0, 0}};
    assert(close(sample_track(ease, 0.25f, {}).x, 0.15625f) && close(sample_track(ease, 0.5f, {}).x, 0.5f));
    // Rotations: the spline is the same sum over the four components, normalised.
    Track3D<Quatf> q;
    q.interpolation = Interpolation3D::CubicSpline;
    q.times = {0.0f, 1.0f};
    const Quatf a = rz(0), b = rz(90);
    q.values = {{0, 0, 0, 0}, a, {0, 0, 0, 0}, {0, 0, 0, 0}, b, {0, 0, 0, 0}};
    const Quatf mid = sample_track(q, 0.5f, {});
    assert(close(length_of(mid), 1.0f) && close(mid, rz(45)));
}

void test_slerp_properties() {
    const Quatf a = rz(0), b = rz(90);
    assert(close(slerp(a, b, 0.0f), a) && close(slerp(a, b, 1.0f), b));
    assert(close(slerp(a, b, 0.5f), rz(45)));
    // Unit length throughout, at a steady rate.
    for (int i = 0; i <= 20; ++i) {
        const float t = static_cast<float>(i) / 20.0f;
        const Quatf q = slerp(a, b, t);
        assert(close(length_of(q), 1.0f, 1e-5f));
        assert(close(q, rz(90.0f * t), 1e-4f));
    }
    // The short way: the negated target is the same rotation and gives the same path.
    const Quatf nb{-b.x, -b.y, -b.z, -b.w};
    for (int i = 0; i <= 10; ++i) {
        const float t = static_cast<float>(i) / 10.0f;
        assert(close(slerp(a, nb, t), slerp(a, b, t), 1e-4f));
    }
    // 270 degrees as a quaternion is -90 the short way round.
    assert(close(slerp(a, rz(270), 0.5f), rz(-45), 1e-4f));
    // nlerp: unit length, endpoints, and nearly the same for small angles.
    assert(close(nlerp(a, b, 0.0f), a) && close(nlerp(a, b, 1.0f), b) && close(length_of(nlerp(a, b, 0.3f)), 1.0f));
    assert(close(nlerp(a, rz(10), 0.5f), rz(5), 1e-4f));
    assert(close(nlerp(a, nb, 0.5f), rz(45), 1e-4f)); // the short way too
    // A rotation track samples as slerp does between its keys, and a key that is not normalised is.
    Track3D<Quatf> t;
    t.times = {0.0f, 2.0f};
    t.values = {a, Quatf{b.x * 3, b.y * 3, b.z * 3, b.w * 3}};
    assert(close(sample_track(t, 1.0f, {}), rz(45)) && close(length_of(sample_track(t, 2.0f, {})), 1.0f));
}

void test_clip_time() {
    AnimClip3D clip;
    clip.duration = 2.0f;
    assert(close(clip_time(clip, 0.5f, true), 0.5f) && close(clip_time(clip, 2.5f, true), 0.5f) &&
           close(clip_time(clip, 4.0f, true), 0.0f));
    assert(close(clip_time(clip, -0.5f, true), 1.5f));
    assert(close(clip_time(clip, 2.5f, false), 2.0f) && close(clip_time(clip, -1.0f, false), 0.0f));
    clip.duration = 0.0f;
    assert(clip_time(clip, 3.0f, true) == 0.0f && clip_time(clip, 3.0f, false) == 0.0f);
    clip.duration = 1.0f;
    assert(clip_time(clip, std::nanf(""), true) == 0.0f);
}

// A clip of the tube's three joints: the middle joint's rotation 0, 90, 0 degrees over two seconds; the base slides
// along x.
AnimClip3D make_clip(const Skeleton3D& sk) {
    AnimClip3D clip;
    clip.name = "bend";
    clip.channels.resize(sk.joint_count());
    clip.channels[1].rotation.times = {0.0f, 1.0f, 2.0f};
    clip.channels[1].rotation.values = {rz(0), rz(90), rz(0)};
    clip.channels[0].translation.times = {0.0f, 2.0f};
    clip.channels[0].translation.values = {{0, 0, 0}, {4, 1, 2}};
    clip.duration = clip.key_duration();
    return clip;
}

void test_sample_clip() {
    const auto tube = skin_fixtures::make_tube();
    const AnimClip3D clip = make_clip(tube.skeleton);
    assert(close(clip.duration, 2.0f) && close(AnimClip3D{}.key_duration(), 0.0f));
    Pose3D pose;
    sample_clip(clip, tube.skeleton, 0.5f, true, pose);
    assert(pose.joints.size() == 3);
    assert(close(pose.joints[1].rotation, rz(45)));
    assert(close(pose.joints[0].translation, {1, 0.25f, 0.5f}));
    // A joint with no tracks keeps its rest value (the top joint, and the rest translation of the middle one).
    assert(close(pose.joints[2].translation, tube.skeleton.rest[2].translation) &&
           close(pose.joints[1].translation, tube.skeleton.rest[1].translation));
    sample_clip(clip, tube.skeleton, 2.5f, true, pose); // wrapped to 0.5
    assert(close(pose.joints[1].rotation, rz(45)));
    sample_clip(clip, tube.skeleton, 2.5f, false, pose); // held at the end
    assert(close(pose.joints[0].translation, {4, 1, 2}));
    // A clip with channels for fewer joints than the skeleton, an empty clip, a clip with no duration.
    AnimClip3D short_clip;
    short_clip.channels.resize(1);
    sample_clip(short_clip, tube.skeleton, 1.0f, true, pose);
    assert(pose.joints.size() == 3 && close(pose.joints[2].translation, tube.skeleton.rest[2].translation));
    sample_clip(AnimClip3D{}, tube.skeleton, 1.0f, true, pose);
    assert(pose.joints.size() == 3);
    AnimClip3D instant;
    instant.channels.resize(3);
    instant.channels[2].translation.times = {0.0f};
    instant.channels[2].translation.values = {{5, 5, 5}};
    sample_clip(instant, tube.skeleton, 7.0f, true, pose);
    assert(close(pose.joints[2].translation, {5, 5, 5}));
}

// --- combining
// -------------------------------------------------------------------------------------------------------------

void test_blend_masks_and_additive() {
    const auto tube = skin_fixtures::make_tube();
    const Skeleton3D& sk = tube.skeleton;
    Pose3D a = sk.rest_pose(), b = sk.rest_pose();
    a.joints[0].translation = {0, 0, 0};
    b.joints[0].translation = {4, 0, 0};
    a.joints[1].rotation = rz(0);
    b.joints[1].rotation = rz(90);
    a.joints[2].scale = {1, 1, 1};
    b.joints[2].scale = {3, 1, 1};
    const Pose3D half = blend_poses(a, b, 0.5f);
    assert(close(half.joints[0].translation, {2, 0, 0}) && close(half.joints[1].rotation, rz(45)) &&
           close(half.joints[2].scale, {2, 1, 1}));
    assert(close(blend_poses(a, b, 0.0f).joints[0].translation, {0, 0, 0}) &&
           close(blend_poses(a, b, 1.0f).joints[0].translation, {4, 0, 0}));

    // Masks.
    const PoseMask3D arm = PoseMask3D::subtree(sk, 1);
    assert(arm.weights == (std::vector<float>{0, 1, 1}));
    assert(PoseMask3D::subtree(sk, 1, 1.0f, false).weights == (std::vector<float>{0, 0, 1}));
    assert(PoseMask3D::subtree(sk, 0, 0.5f).weights == (std::vector<float>{0.5f, 0.5f, 0.5f}));
    assert(PoseMask3D::subtree(sk, 7).weights == (std::vector<float>{0, 0, 0}));
    const std::string_view names[] = {"top", "nothing"};
    assert(PoseMask3D::joints(sk, names).weights == (std::vector<float>{0, 0, 1}));
    PoseMask3D both = PoseMask3D::joints(sk, names);
    both.add(PoseMask3D::subtree(sk, 1));
    assert(both.weights == (std::vector<float>{0, 1, 1}));
    Pose3D base = a;
    blend_masked(base, b, arm, 1.0f);
    assert(close(base.joints[0].translation, {0, 0, 0})); // outside the mask: untouched
    assert(close(base.joints[1].rotation, rz(90)) && close(base.joints[2].scale, {3, 1, 1}));
    base = a;
    blend_masked(base, b, arm, 0.5f); // weight times the mask
    assert(close(base.joints[1].rotation, rz(45)) && close(base.joints[0].translation, {0, 0, 0}));
    base = a;
    blend_masked(base, b, {}, 1.0f); // an empty mask is every joint
    assert(close(base.joints[0].translation, {4, 0, 0}));
    base = a;
    blend_masked(base, b, PoseMask3D{{0.0f, 0.25f}}, 1.0f); // a short mask: the rest are 0
    assert(close(base.joints[1].rotation, rz(22.5f), 1e-4f) && close(base.joints[2].scale, {1, 1, 1}));

    // Additive: the change of `add` from `reference`, put on a base that is somewhere else.
    Pose3D reference = sk.rest_pose();
    Pose3D add = reference;
    add.joints[0].translation = {1, 2, 3};
    add.joints[1].rotation = rz(90);
    add.joints[2].scale = {2, 1, 1};
    Pose3D target = sk.rest_pose();
    target.joints[0].translation = {10, 0, 0};
    target.joints[1].rotation = rz(30);
    target.joints[2].scale = {1.5f, 1, 1};
    Pose3D out = target;
    apply_additive(out, add, reference, {}, 1.0f);
    assert(close(out.joints[0].translation, {11, 2, 3}));
    assert(close(out.joints[1].rotation, rz(120)));
    assert(close(out.joints[2].scale, {2.5f, 1, 1}));
    out = target;
    apply_additive(out, add, reference, {}, 0.5f);
    assert(close(out.joints[0].translation, {10.5f, 1, 1.5f}) && close(out.joints[1].rotation, rz(75), 1e-4f) &&
           close(out.joints[2].scale, {2.0f, 1, 1}));
    out = target;
    apply_additive(out, add, reference, arm, 1.0f);
    assert(close(out.joints[0].translation, {10, 0, 0}) && close(out.joints[1].rotation, rz(120)));
    out = target;
    apply_additive(out, reference, reference, {}, 1.0f); // the reference on itself adds nothing
    assert(close(out.joints[0].translation, target.joints[0].translation) &&
           close(out.joints[1].rotation, target.joints[1].rotation));
}

// --- joint matrices
// --------------------------------------------------------------------------------------------------------

void test_joint_matrices() {
    const auto tube = skin_fixtures::make_tube(25, 12, 3.0f);
    const Skeleton3D& sk = tube.skeleton;
    std::string why;
    assert(kin::test::valid3d(sk.validate(), why));
    // At rest every matrix is the identity: a skinned mesh draws as modelled.
    std::vector<Mat4f> m;
    compute_joint_matrices(sk, sk.rest_pose(), m);
    assert(m.size() == 3);
    for (const Mat4f& mat : m) {
        for (int i = 0; i < 16; ++i) {
            assert(close(mat.m[i], Mat4f{}.m[i], 1e-6f));
        }
    }
    // The middle joint turned 90 degrees about Z: the joint at (0, 1, 0) stays; the top joint's origin, (0, 2, 0) in
    // the mesh, goes to (0, 1, 0) + Rz90 (0, 1, 0) = (-1, 1, 0); and a vertex at (0, 3, 0), bound to the top, to (-2,
    // 1, 0).
    Pose3D pose = skin_fixtures::bent(tube, 90.0f, 0.0f);
    compute_joint_matrices(sk, pose, m);
    assert(close(m[1].apply({0, 1, 0}), {0, 1, 0}, 1e-5f));
    assert(close(m[2].apply({0, 2, 0}), {-1, 1, 0}, 1e-5f));
    assert(close(m[2].apply({0, 3, 0}), {-2, 1, 0}, 1e-5f));
    assert(close(m[0].apply({0, 3, 0}), {0, 3, 0}));
    // The top turned as well, 90 degrees more: (0, 3, 0) is one up from the top joint, rotated by 180: (-1, 0, 0) from
    // (-1, 1, 0).
    pose = skin_fixtures::bent(tube, 90.0f, 90.0f);
    compute_joint_matrices(sk, pose, m);
    // Rz(180) (0, 1, 0) = (0, -1, 0): from (-1, 1, 0) that is (-1, 0, 0).
    assert(close(m[2].apply({0, 3, 0}), {-1, 0, 0}, 1e-5f));
    // The base moved: everything moves with it.
    pose = skin_fixtures::bent(tube, 0.0f, 0.0f, {5, 0, 0});
    compute_joint_matrices(sk, pose, m);
    for (const Mat4f& mat : m) {
        assert(close(mat.apply({1, 2, 3}), {6, 2, 3}, 1e-5f));
    }
    // A root transform (the file's nodes above the joints) is part of every world matrix.
    Skeleton3D moved = sk;
    moved.root_transform = Mat4f::translation({0, 0, 7});
    compute_joint_matrices(moved, moved.rest_pose(), m);
    assert(close(m[2].apply({0, 0, 0}), {0, 0, 7}));
    // Into room the caller keeps, and a pose that is short: the rest pose fills in.
    std::vector<Mat4f> room(3);
    Pose3D short_pose;
    short_pose.joints = {pose.joints[0]};
    compute_joint_matrices(sk, short_pose, std::span<Mat4f>{room});
    assert(close(room[2].apply({0, 0, 0}), {5, 0, 0}));
    // Validation: a parent must come first; too many joints are refused.
    Skeleton3D bad = sk;
    bad.parents = {1, 0, 1};
    assert(!kin::test::valid3d(bad.validate(), why) && !why.empty());
    Skeleton3D big;
    big.parents.assign(257, -1);
    big.rest.resize(257);
    big.inverse_bind.resize(257);
    assert(!kin::test::valid3d(big.validate(), why));
    assert(!kin::test::valid3d(Skeleton3D{}.validate(), why));
    // depth, find, subtree
    assert(sk.find("top") == 2 && sk.find("nope") == -1 && sk.depth(2) == 2 && sk.depth(0) == 0 &&
           sk.root_joint() == 0);
    assert(sk.subtree(1) == (std::vector<u8>{0, 1, 1}));
}

// --- root motion
// -----------------------------------------------------------------------------------------------------------

void test_root_motion() {
    const auto tube = skin_fixtures::make_tube();
    const Skeleton3D& sk = tube.skeleton;
    // The root moves 2 m/s in x along a wobble in y, and turns 90 degrees over the 2-second clip.
    AnimClip3D clip;
    clip.channels.resize(3);
    clip.channels[0].translation.times = {0.0f, 1.0f, 2.0f};
    clip.channels[0].translation.values = {{0, 0, 0}, {2, 1, 0}, {4, 0, 0}};
    clip.channels[0].rotation.times = {0.0f, 2.0f};
    clip.channels[0].rotation.values = {rz(0), rz(90)};
    clip.duration = 2.0f;
    RootMotion3D m = extract_root_motion(clip, sk, 0.0f, 1.0f, true);
    assert(close(m.translation, {2, 0, 0})); // y is not carried by default
    m = extract_root_motion(clip, sk, 0.5f, 1.5f, true, {.x = true, .y = true, .z = false, .rotation = true});
    assert(close(m.translation, {2, 0.0f, 0}, 1e-5f)); // 1 -> 3 in x; y: 0.5 -> 0.5
    assert(close(m.rotation, rz(45), 1e-4f));
    // Across the end of a looping clip: the end's motion from `from`, then the start's to `to`.
    m = extract_root_motion(clip, sk, 1.5f, 2.5f, true, {.x = true, .y = false, .z = true, .rotation = true});
    assert(close(m.translation, {2.0f, 0, 0}, 1e-5f)); // 3 -> 4 (1) and 0 -> 1 (1)
    assert(close(m.rotation, rz(45), 1e-4f));          // 67.5 -> 90, then 0 -> 22.5
    // Backwards across the start (a negative speed): the start's motion from `from`, then the end's to `to`, not the
    // whole clip's travel forwards.
    m = extract_root_motion(clip, sk, 0.5f, -0.5f, true, {.x = true, .y = false, .z = true, .rotation = true});
    assert(close(m.translation, {-2.0f, 0, 0}, 1e-5f)); // 1 -> 0 (-1) and 4 -> 3 (-1)
    assert(close(m.rotation, rz(-45), 1e-4f));          // 22.5 -> 0, then 90 -> 67.5
    // A frame can cross several complete cycles, in either direction, including equal wrapped times.
    for (const auto [from, to] :
         {std::pair{0.5f, 8.5f}, std::pair{8.5f, 0.5f}, std::pair{1.5f, 8.5f}, std::pair{-8.5f, -1.5f}}) {
        m = extract_root_motion(clip, sk, from, to, true, {.rotation = true});
        assert(close(m.translation, {2.0f * (to - from), 0, 0}, 1e-4f));
        assert(close(m.rotation, rz(45.0f * (to - from)), 1e-4f));
    }
    // The number of complete cycles is not capped or converted to an overflowing integer.
    // An exact half turn keeps this a cycle-count test, independent of trigonometric rounding.
    auto many = clip;
    many.channels[0].rotation.values.back() = Quatf{0, 0, 1, 0};
    m = extract_root_motion(many, sk, 0, std::ldexp(2.0f, 40), true, {.rotation = true});
    assert(close(m.rotation, Quatf{}, 1e-4f));
    // Not looping: held at the end, no motion past it.
    m = extract_root_motion(clip, sk, 1.5f, 3.0f, false);
    assert(close(m.translation, {1.0f, 0, 0}));
    // The motion of a whole loop is the sum of its steps.
    float sum = 0.0f;
    float t = 0.0f;
    for (int i = 0; i < 7; ++i) {
        sum += extract_root_motion(clip, sk, t, t + 0.3f, true).translation.x;
        t += 0.3f;
    }
    assert(close(sum, 4.2f, 1e-3f)); // 2.1 s: one loop (4 m) and 0.1 s (0.2 m)
    // A clip with no duration moves nothing.
    assert(close(extract_root_motion(AnimClip3D{}, sk, 0, 1, true).translation, {0, 0, 0}));
    // Removing it from the pose: the root is held at rest in the chosen axes, others untouched.
    Pose3D pose;
    sample_clip(clip, sk, 1.0f, true, pose);
    pose.joints[1].rotation = rz(10);
    remove_root_motion(pose, sk);
    assert(close(pose.joints[0].translation, {0, 1, 0}) && close(pose.joints[1].rotation, rz(10)));
    sample_clip(clip, sk, 1.0f, true, pose);
    remove_root_motion(pose, sk, {.x = true, .y = true, .z = true, .rotation = true});
    assert(close(pose.joints[0].translation, {0, 0, 0}) && close(pose.joints[0].rotation, Quatf{}));
}

// --- the player
// ------------------------------------------------------------------------------------------------------------

AnimClip3D constant_clip(const Skeleton3D& sk, Vec3f where, float duration = 1.0f) {
    AnimClip3D clip;
    clip.channels.resize(sk.joint_count());
    clip.channels[0].translation.times = {0.0f};
    clip.channels[0].translation.values = {where};
    clip.duration = duration;
    return clip;
}

void test_player_crossfade() {
    const auto tube = skin_fixtures::make_tube();
    const Skeleton3D& sk = tube.skeleton;
    const AnimClip3D a = constant_clip(sk, {0, 0, 0}), b = constant_clip(sk, {10, 0, 0});
    AnimationPlayer3D player{&sk};
    assert(player.pose().joints.size() == 3 && !player.finished());
    player.play(&a);
    player.update(0.1f);
    assert(close(player.pose().joints[0].translation, {0, 0, 0}) && player.clip() == &a &&
           player.fading_from() == nullptr);
    player.crossfade_to(&b, 1.0f);
    assert(player.clip() == &b && player.fading_from() == &a && close(player.fade_progress(), 0.0f));
    player.update(0.25f);
    assert(close(player.pose().joints[0].translation, {2.5f, 0, 0}) && close(player.fade_progress(), 0.25f));
    player.update(0.25f);
    assert(close(player.pose().joints[0].translation, {5, 0, 0}));
    player.update(0.5f); // done: all of b, and a is dropped
    assert(close(player.pose().joints[0].translation, {10, 0, 0}) && player.fading_from() == nullptr &&
           close(player.fade_progress(), 1.0f));
    player.update(0.5f);
    assert(close(player.pose().joints[0].translation, {10, 0, 0}));
    // A fade begun in the middle of another fades from the picture then: no jump.
    const AnimClip3D c = constant_clip(sk, {-4, 0, 0});
    player.play(&a);
    player.crossfade_to(&b, 1.0f);
    player.update(0.5f);
    const Vec3f before = player.pose().joints[0].translation; // {5, 0, 0}
    player.crossfade_to(&c, 1.0f);
    player.update(0.0f);
    assert(close(player.pose().joints[0].translation, before));
    player.update(0.5f);
    assert(close(player.pose().joints[0].translation, {0.5f, 0, 0}, 1e-5f)); // halfway from 5 to -4
    player.update(0.5f);
    assert(close(player.pose().joints[0].translation, {-4, 0, 0}));
    // A zero-second fade is a cut.
    player.crossfade_to(&b, 0.0f);
    player.update(0.0f);
    assert(close(player.pose().joints[0].translation, {10, 0, 0}));
    // Speed, looping, and a clip that ends.
    AnimClip3D ramp;
    ramp.channels.resize(3);
    ramp.channels[0].translation.times = {0.0f, 2.0f};
    ramp.channels[0].translation.values = {{0, 0, 0}, {2, 0, 0}};
    ramp.duration = 2.0f;
    player.play(&ramp, /*loop=*/true);
    player.set_speed(2.0f);
    player.update(0.5f);
    assert(close(player.time(), 1.0f) && close(player.pose().joints[0].translation, {1, 0, 0}));
    player.update(0.75f); // at speed 2: 1.5 s more, 2.5 s of clip, wrapped to 0.5
    assert(close(player.time(), 0.5f) && close(player.pose().joints[0].translation, {0.5f, 0, 0}));
    player.set_speed(1.0f);
    player.play(&ramp, /*loop=*/false);
    player.update(1.5f);
    assert(!player.finished());
    player.update(1.5f);
    assert(player.finished() && close(player.pose().joints[0].translation, {2, 0, 0}));
    // No clip, no skeleton: the rest pose and no crash.
    AnimationPlayer3D empty{&sk};
    empty.update(1.0f);
    assert(close(empty.pose().joints[1].translation, sk.rest[1].translation));
    AnimationPlayer3D nothing;
    nothing.update(1.0f);
    std::vector<Mat4f> matrices;
    nothing.joint_matrices(matrices);
    assert(matrices.empty() && nothing.pose().joints.empty());
}

void test_player_layers_and_root_motion() {
    const auto tube = skin_fixtures::make_tube();
    const Skeleton3D& sk = tube.skeleton;
    AnimClip3D base_clip = constant_clip(sk, {1, 0, 0});
    AnimClip3D wave;
    wave.channels.resize(3);
    wave.channels[0].translation.times = {0.0f};
    wave.channels[0].translation.values = {{9, 9, 9}}; // outside the mask
    wave.channels[1].rotation.times = {0.0f, 1.0f};
    wave.channels[1].rotation.values = {rz(0), rz(80)};
    wave.duration = 1.0f;
    AnimationPlayer3D player{&sk};
    player.play(&base_clip);
    AnimationPlayer3D::Layer& layer = player.layer(0);
    layer.clip = &wave;
    layer.mask = PoseMask3D::subtree(sk, 1);
    layer.weight = 0.5f;
    player.update(0.5f);
    assert(player.layer_count() == 1);
    assert(close(player.pose().joints[0].translation, {1, 0, 0}));  // the mask keeps the base's root
    assert(close(player.pose().joints[1].rotation, rz(20), 1e-4f)); // 40 degrees at half weight
    // Additive: the clip's change from its first frame.
    player.clear_layers();
    AnimationPlayer3D::Layer& add = player.layer(0);
    add.clip = &wave;
    add.additive = true;
    add.loop = false;
    player.update(1.0f);
    assert(close(player.pose().joints[1].rotation, rz(80), 1e-4f) &&
           close(player.pose().joints[0].translation, {1, 0, 0}));

    // Root motion in the player: the root joint is held, the motion reported.
    AnimClip3D walk;
    walk.channels.resize(3);
    walk.channels[0].translation.times = {0.0f, 1.0f};
    walk.channels[0].translation.values = {{0, 0, 0}, {3, 0, 0}};
    walk.duration = 1.0f;
    AnimationPlayer3D p2{&sk};
    p2.play(&walk);
    p2.set_root_motion(true);
    float total = 0.0f;
    for (int i = 0; i < 25; ++i) { // 2.5 s, across two wraps
        p2.update(0.1f);
        total += p2.root_motion().translation.x;
        assert(close(p2.pose().joints[0].translation, {0, 0, 0}));
    }
    assert(close(total, 7.5f, 1e-3f));
    // Off: the root joint moves, nothing is reported.
    AnimationPlayer3D p3{&sk};
    p3.play(&walk);
    p3.update(0.5f);
    assert(close(p3.pose().joints[0].translation, {1.5f, 0, 0}) && close(p3.root_motion().translation, {0, 0, 0}));
    // A negative speed on a loop: the step across the start is a small step back, not the clip's whole travel forwards.
    AnimationPlayer3D p4{&sk};
    p4.set_speed(-1.0f);
    p4.play(&walk);
    p4.set_root_motion(true);
    float back = 0.0f;
    for (int i = 0; i < 25; ++i) { // 2.5 s backwards, across two wraps
        p4.update(0.1f);
        assert(p4.root_motion().translation.x < 0.0f);
        back += p4.root_motion().translation.x;
    }
    assert(close(back, -7.5f, 1e-3f));
}

// --- bounds
// ----------------------------------------------------------------------------------------------------------------

void test_skin_bounds_hold_the_posed_vertices() {
    const auto tube = skin_fixtures::make_tube();
    const std::vector<SkinBound3D> bounds = compute_skin_bounds(tube.mesh, 3);
    assert(bounds.size() == 3 && bounds[0].radius >= 0.0f && bounds[1].radius >= 0.0f && bounds[2].radius >= 0.0f);
    std::mt19937 rng{1234};
    std::uniform_real_distribution<float> angle(-170.0f, 170.0f), move(-3.0f, 3.0f);
    std::vector<Mat4f> m;
    for (int trial = 0; trial < 500; ++trial) {
        Pose3D pose = skin_fixtures::bent(tube, angle(rng), angle(rng), {move(rng), move(rng), move(rng)});
        if (trial % 3 == 0) {
            pose.joints[1].scale = {1.0f + std::fabs(move(rng)) * 0.3f, 0.5f, 1.5f}; // non-uniform scale is allowed for
        }
        compute_joint_matrices(tube.skeleton, pose, m);
        Vec3f c{};
        float r = 0.0f;
        assert(skin_sphere(bounds, m, c, r));
        const MeshData3D posed = skin_mesh(tube.mesh, m, /*gpu_weights=*/false);
        for (const MeshVertex3D& v : posed.vertices) {
            assert(length(v.position - c) <= r + 1e-4f);
        }
        const MeshData3D posed_gpu = skin_mesh(tube.mesh, m, /*gpu_weights=*/true);
        for (const MeshVertex3D& v : posed_gpu.vertices) {
            assert(length(v.position - c) <= r + 1e-4f);
        }
    }
    // At rest the sphere is not much bigger than the mesh (it is a bound, but a useful one).
    compute_joint_matrices(tube.skeleton, tube.skeleton.rest_pose(), m);
    Vec3f c{};
    float r = 0.0f;
    assert(skin_sphere(bounds, m, c, r));
    const Bounds3D box = tube.mesh.bounds();
    assert(r < box.radius() * 1.5f);
    // An arm swung out of the rest bound is inside the posed one: the case a rest bound would cull.
    const Pose3D out = skin_fixtures::bent(tube, 90.0f, 0.0f, {20.0f, 0, 0});
    compute_joint_matrices(tube.skeleton, out, m);
    assert(skin_sphere(bounds, m, c, r) && c.x > 15.0f);
    // No bounds, no joints used: nothing to bound.
    assert(!skin_sphere({}, m, c, r));
    // stretch_bound: 1 for a rotation, the largest scale for a scale, never below the true stretch.
    assert(close(stretch_bound(Mat4f::rotation(rz(33))), 1.0f, 1e-5f) &&
           close(stretch_bound(Mat4f::scaling({2, 3, 1})), 3.0f, 1e-5f));
    assert(stretch_bound(Mat4f::rotation(rz(30)) * Mat4f::scaling({1, 4, 2})) >= 4.0f - 1e-5f);
}

// Every joint that pulls on a vertex bounds it, not only the strongest: a vertex held 40% by a joint that stays and 30%
// each by two that swing half a turn about the origin ends up inside the spheres only if the joints it is merely shared
// with count its place too.
void test_skin_bounds_count_every_influence() {
    MeshData3D m;
    m.vertices.assign(3, MeshVertex3D{});
    m.vertices[0].position = {0, 0, 0};
    m.vertices[1].position = {0, 0, 0};
    m.vertices[2].position = {10, 0, 0};
    m.indices = {0, 1, 2};
    m.skin.resize(3);
    m.skin[0] = {{0, 0, 0, 0}, {1, 0, 0, 0}};
    m.skin[1] = {{1, 0, 0, 0}, {1, 0, 0, 0}};
    m.skin[2] = {{0, 1, 2, 0}, {0.4f, 0.3f, 0.3f, 0.0f}};
    assert(m.validate());
    const std::vector<SkinBound3D> bounds = compute_skin_bounds(m, 3);
    assert(bounds[2].radius >= 0.0f &&
           close(bounds[2].center, {10, 0, 0})); // joint 2 pulls on one vertex only, and still has a bound
    assert(close(bounds[1].center, {5, 0, 0}) && bounds[1].radius >= 5.0f);
    const Mat4f half_turn = Mat4f::rotation(Quatf::axis_angle({0, 0, 1}, 180.0f));
    const std::vector<Mat4f> matrices = {Mat4f{}, half_turn, half_turn};
    const MeshData3D posed = skin_mesh(m, matrices, false);
    assert(close(posed.vertices[2].position, {-2.0f, 0.0f, 0.0f}, 1e-4f));
    Vec3f c{};
    float r = 0.0f;
    assert(skin_sphere(bounds, matrices, c, r));
    for (const MeshVertex3D& v : posed.vertices) {
        assert(length(v.position - c) <= r + 1e-4f);
    }
}

void test_cpu_skinning() {
    const auto tube = skin_fixtures::make_tube();
    std::vector<Mat4f> m;
    // At rest, skinning changes nothing.
    compute_joint_matrices(tube.skeleton, tube.skeleton.rest_pose(), m);
    const MeshData3D same = skin_mesh(tube.mesh, m);
    for (std::size_t i = 0; i < same.vertices.size(); ++i) {
        assert(close(same.vertices[i].position, tube.mesh.vertices[i].position, 1e-5f) &&
               close(same.vertices[i].normal, tube.mesh.vertices[i].normal, 1e-5f));
    }
    assert(!same.skinned() && same.indices == tube.mesh.indices);
    // Bent 90 degrees over the middle joint: the top ring is at (-2, 1, 0) + the radius.
    compute_joint_matrices(tube.skeleton, skin_fixtures::bent(tube, 90.0f, 0.0f), m);
    const MeshData3D bent = skin_mesh(tube.mesh, m);
    // The last ring is bound to the top joint alone: its vertices (x, 3, z) go to (-2, 1 + x, z) (worked by hand in
    // test_joint_matrices).
    for (std::size_t i = bent.vertices.size() - 13; i < bent.vertices.size(); ++i) {
        const Vec3f at = tube.mesh.vertices[i].position;
        assert(close(bent.vertices[i].position, {-2.0f, 1.0f + at.x, at.z}, 1e-4f));
    }
    for (const MeshVertex3D& v : bent.vertices) {
        assert(std::isfinite(v.position.x) && close(length(v.normal), 1.0f, 1e-4f));
    }
}

void test_mesh_skin_data() {
    MeshData3D mesh = meshes::box({1, 1, 1});
    assert(!mesh.skinned() && mesh.skin_joint_count() == 0);
    mesh.skin.assign(mesh.vertices.size(), {});
    std::string why;
    assert(kin::test::valid3d(mesh.validate(), why) && mesh.skinned() && mesh.skin_joint_count() == 1);
    mesh.skin[3].joints = {5, 0, 0, 0};
    assert(mesh.skin_joint_count() == 6);
    mesh.skin[3].joints = {5, 7, 0, 0};
    mesh.skin[3].weights = {0.5f, 0.0f, 0, 0}; // a joint with no weight does not count
    assert(mesh.skin_joint_count() == 6);
    mesh.skin.pop_back();
    assert(!kin::test::valid3d(mesh.validate(), why) && why.find("skin") != std::string::npos);
    mesh.skin.push_back({});
    mesh.skin[0].weights = {0, 0, 0, 0};
    assert(!kin::test::valid3d(mesh.validate(), why));
    mesh.skin[0].weights = {-1, 1, 0, 0};
    assert(!kin::test::valid3d(mesh.validate(), why));
    mesh.skin[0].weights = {std::nanf(""), 1, 0, 0};
    assert(!kin::test::valid3d(mesh.validate(), why));
    mesh.skin[0] = {};
    assert(kin::test::valid3d(mesh.validate(), why));
    // append keeps the skin parallel, binding what has none to joint 0.
    MeshData3D a = meshes::box({1, 1, 1});
    MeshData3D b = meshes::box({1, 1, 1});
    b.skin.assign(b.vertices.size(), MeshSkinVertex3D{{3, 0, 0, 0}, {1, 0, 0, 0}});
    const std::size_t av = a.vertices.size();
    a.append(b, Mat4f::translation({5, 0, 0}));
    assert(a.skin.size() == a.vertices.size() && a.skin[0].joints[0] == 0 && a.skin[av].joints[0] == 3 &&
           kin::test::valid3d(a.validate(), why));
    MeshData3D c = b;
    c.append(meshes::box({1, 1, 1}));
    assert(c.skin.size() == c.vertices.size() && c.skin.front().joints[0] == 3 && c.skin.back().joints[0] == 0 &&
           kin::test::valid3d(c.validate(), why));
    MeshData3D plain = meshes::box({1, 1, 1});
    plain.append(meshes::box({2, 2, 2}));
    assert(!plain.skinned());
    // Weights as the GPU holds them: bytes that sum to exactly 255.
    const std::array<float, 4> cases[] = {{1, 0, 0, 0},
                                          {0.5f, 0.5f, 0, 0},
                                          {1.0f / 3, 1.0f / 3, 1.0f / 3, 0},
                                          {0.25f, 0.25f, 0.25f, 0.25f},
                                          {0.7f, 0.2f, 0.07f, 0.03f},
                                          {2, 2, 0, 0},
                                          {0, 0, 0, 0},
                                          {0.001f, 0.999f, 0, 0}};
    for (const auto& w : cases) {
        const std::array<u8, 4> q = quantise_skin_weights(w);
        assert(int(q[0]) + q[1] + q[2] + q[3] == 255);
    }
    assert(quantise_skin_weights({1, 0, 0, 0}) == (std::array<u8, 4>{255, 0, 0, 0}));
    assert(quantise_skin_weights({0, 0, 0, 0}) == (std::array<u8, 4>{255, 0, 0, 0}));
    assert(quantise_skin_weights({0.5f, 0.5f, 0, 0})[0] + quantise_skin_weights({0.5f, 0.5f, 0, 0})[1] == 255);
}

// --- the loader
// ------------------------------------------------------------------------------------------------------------

ModelData3D read(const skin_fixtures::Gltf& gltf) {
    ModelData3D data;
    std::string error;
    const bool ok = kin::test::read_value3d(kin::read_model_data(gltf.json, "memory.gltf"), data, error);
    if (!ok) {
        std::cerr << "read_model_data: " << error << '\n' << gltf.json << '\n';
    }
    assert(ok);
    return data;
}

// The weights of joint 0 and joint 1 of each vertex of the first part, as the loader made them.
std::array<std::array<float, 2>, 6> weights_of(const ModelData3D& data) {
    std::array<std::array<float, 2>, 6> out{};
    const MeshData3D& mesh = data.parts[0].mesh;
    assert(mesh.skin.size() == 6);
    for (std::size_t v = 0; v < 6; ++v) {
        for (std::size_t k = 0; k < 4; ++k) {
            if (mesh.skin[v].weights[k] > 0.0f) {
                assert(mesh.skin[v].joints[k] < 2);
                out[v][mesh.skin[v].joints[k]] += mesh.skin[v].weights[k];
            }
        }
        float sum = 0.0f;
        for (float w : mesh.skin[v].weights)
            sum += w;
        assert(close(sum, 1.0f, 1e-5f));
    }
    return out;
}

void test_loader_skins_and_clips() {
    using Joints = GltfOptions::Joints;
    using Weights = GltfOptions::Weights;
    // The same strip in every accessor type the spec allows: joints u8 or u16, weights float or normalised u8 or u16.
    for (const Joints jt : {Joints::U8, Joints::U16}) {
        for (const Weights wt : {Weights::Float, Weights::U8, Weights::U16}) {
            for (const bool reversed : {false, true}) {
                GltfOptions o;
                o.joints = jt;
                o.weights = wt;
                o.reversed = reversed;
                const auto gltf = skin_fixtures::make_gltf(o);
                const ModelData3D data = read(gltf);
                assert(data.parts.size() == 1 && data.skeletons.size() == 1 && data.clips.size() == 1);
                const ModelPartData3D& part = data.parts[0];
                assert(part.skin == 0 && part.mesh.vertices.size() == 6 && part.mesh.skinned() && part.mesh.validate());
                // The skinned part's node placing is ignored.
                assert(part.transform.m[12] == 0.0f && part.transform.m[13] == 0.0f);
                const auto w = weights_of(data);
                for (std::size_t v = 0; v < 6; ++v) {
                    assert(close(w[v][0], gltf.weight[v][0], 0.004f) && close(w[v][1], gltf.weight[v][1], 0.004f));
                }
                // The skeleton: two joints, root first whatever order the file listed them in; the inverse binds
                // follow.
                const Skeleton3D& sk = data.skeletons[0];
                assert(sk.joint_count() == 2 && sk.validate() && sk.parents[0] == -1 && sk.parents[1] == 0);
                assert(sk.names[0] == "joint0" && sk.names[1] == "joint1");
                assert(close(sk.rest[1].translation, {0, 1, 0}) && close(sk.inverse_bind[1].m[13], -1.0f));
                assert(close(sk.root_transform.m[12], 2.0f)); // the Armature node above the root joint
                assert(sk.find("joint1") == 1);
            }
        }
    }
    // The clip.
    const auto gltf = skin_fixtures::make_gltf();
    const ModelData3D data = read(gltf);
    const Skeleton3D& sk = data.skeletons[0];
    const AnimClip3D& clip = data.clips[0];
    assert(clip.name == "bend" && close(clip.duration, 2.0f) && clip.skeleton == 0 && clip.channels.size() == 2);
    assert(clip.channels[1].rotation.times.size() == 3 &&
           clip.channels[1].rotation.interpolation == Interpolation3D::Linear);
    assert(clip.channels[0].translation.interpolation == Interpolation3D::CubicSpline &&
           clip.channels[0].translation.values.size() == 6);
    assert(clip.channels[1].translation.interpolation == Interpolation3D::Step);
    Pose3D pose;
    sample_clip(clip, sk, 0.5f, false, pose);
    assert(close(pose.joints[1].rotation, rz(45), 1e-5f));
    assert(
        close(pose.joints[0].translation, {1.125f, 0, 0}, 1e-5f)); // the spline by hand: see test_sampling_cubic_spline
    assert(close(pose.joints[1].translation, {0, 1, 0}));
    sample_clip(clip, sk, 1.0f, false, pose);
    assert(close(pose.joints[1].translation, {0, 2, 0}) && close(pose.joints[1].rotation, rz(90), 1e-5f) &&
           close(pose.joints[0].translation, {2, 0, 0}));
    sample_clip(clip, sk, 1.75f, false, pose);
    assert(close(pose.joints[1].translation, {0, 2, 0}) && close(pose.joints[1].rotation, rz(22.5f), 1e-4f));
    Model3D model;
    model.clips = data.clips;
    assert(model.find_clip("bend") == &model.clips[0] && model.find_clip("nope") == nullptr &&
           model.find_clip("bend", 1) == nullptr);

    // Weights that do not sum to 1 are made to; a vertex with none goes to joint 0; an index past the skin's joints is
    // dropped.
    GltfOptions o;
    o.unnormalised = true;
    auto w = weights_of(read(skin_fixtures::make_gltf(o)));
    assert(close(w[2][0], 0.5f, 1e-5f) && close(w[2][1], 0.5f, 1e-5f) && close(w[5][1], 1.0f, 1e-5f));
    o = {};
    o.no_weights = true;
    auto data2 = read(skin_fixtures::make_gltf(o));
    w = weights_of(data2);
    assert(close(w[0][0], 1.0f) && data2.parts[0].mesh.skin[0].joints[0] == 0);
    o = {};
    o.bad_index = true;
    w = weights_of(read(skin_fixtures::make_gltf(o)));
    assert(close(w[5][0], 1.0f) && close(w[5][1], 0.0f)); // the index 200 influence is dropped, the rest renormalised

    // Morph target weights channels are ignored (logged); the joints' clip is kept. A node that is no joint, moved by
    // the same animation, is the node skeleton's (after the skin), and its channel a second clip of the same name after
    // the skin's.
    o = {};
    o.morph_channel = true;
    o.node_animation = true;
    auto data3 = read(skin_fixtures::make_gltf(o));
    assert(data3.clips.size() == 2 && data3.clips[0].channels.size() == 2 &&
           data3.clips[0].channels[1].rotation.times.size() == 3);
    assert(data3.clips[0].skeleton == 0 && data3.skeletons.size() == 2 && data3.node_skeleton == 1);
    assert(data3.clips[1].name == "bend" && data3.clips[1].skeleton == 1 && data3.clips[1].channels.size() == 1);
    assert(data3.clips[1].channels[0].translation.times.size() == 3 && close(data3.clips[1].duration, 2.0f));
    assert(data3.skeletons[1].names == std::vector<std::string>{"loose"} &&
           data3.skeletons[1].parents == std::vector<i32>{-1});
    // The loose mesh (a node that is no joint) is a static part at its node's placing, hanging from it; the strip is
    // skinned.
    assert(data3.parts.size() == 2 && data3.parts[0].skin == 0 && data3.parts[1].skin == -1);
    assert(data3.parts[0].node == -1 && data3.parts[1].node == 0);

    // A mesh on a node under a joint becomes a part bound to that joint, in the space the inverse binds start from.
    o = {};
    o.attachment = true;
    o.joint_count = 3;
    auto data4 = read(skin_fixtures::make_gltf(o));
    assert(data4.parts.size() == 2);
    const ModelPartData3D* hat = nullptr;
    for (const auto& part : data4.parts)
        hat = part.name == "hat" ? &part : hat;
    assert(hat && hat->skin == 0 && hat->mesh.skinned() && hat->mesh.skin[0].joints[0] == 1 &&
           hat->mesh.skin[0].weights[0] == 1.0f);
    // The hat's vertices are in the world at rest: the strip's y 0..2 placed at the hat node (0, 0.5) under joint1 (0,
    // 1): 1.5 .. 3.5, x from the armature (2).
    assert(close(hat->mesh.bounds().min.y, 1.5f, 1e-5f) && close(hat->mesh.bounds().max.y, 3.5f, 1e-5f) &&
           close(hat->mesh.bounds().min.x, 2.0f - 0.5f, 1e-5f));
    assert(hat->transform.m[12] == 0.0f && hat->transform.m[13] == 0.0f);

    // A file with no skin reads as before: no skeleton, no clips, parts not skinned.
    ModelData3D plain;
    std::string error;
    {
        // The strip's mesh 1 alone (no JOINTS): drop the skin from the file.
        std::string json = skin_fixtures::make_gltf().json;
        const std::size_t at = json.find("\"skin\":0");
        assert(at != std::string::npos);
        json.erase(at - 1, std::string{",\"skin\":0"}.size());
        const std::size_t skins = json.find("\"skins\":");
        json.erase(skins, json.find("}],", skins) + 3 - skins);
        const std::size_t anims = json.find("\"animations\":");
        json.erase(anims, json.find("}],\"buffers\"", anims) + 3 - anims);
        assert(kin::test::read_value3d(kin::read_model_data(json, "plain.gltf"), plain, error));
        assert(plain.skeletons.empty() && plain.clips.empty() && plain.parts.size() == 1 && plain.parts[0].skin == -1 &&
               !plain.parts[0].mesh.skinned());
    }
    // Not glTF at all: refused, with why.
    ModelData3D none;
    assert(!kin::test::read_value3d(kin::read_model_data("this is not a model", "x.gltf"), none, error) &&
           !error.empty());
}

// Exporters may give two skins the same joints in a different order. Both skeletons must receive every shared
// channel; identical skins should collapse to a single skeleton instead.
void test_loader_shared_skin_channels() {
    for (const bool reversed : {false, true}) {
        auto fixture = skin_fixtures::make_gltf({});
        auto& json = fixture.json;
        const auto start = json.find("\"skins\":[") + 9;
        const auto stop = json.find("}],", start) + 1;
        std::string duplicate = json.substr(start, stop - start);
        if (reversed) {
            const auto joints = duplicate.find("[1,2]");
            assert(joints != std::string::npos);
            duplicate.replace(joints, 5, "[2,1]");
        }
        json.insert(stop, "," + duplicate);
        const auto data = read(fixture);
        assert(data.skeletons.size() == (reversed ? 2u : 1u));
        assert(data.clips.size() == data.skeletons.size());
        for (const auto& clip : data.clips) {
            const auto& sk = data.skeletons[clip.skeleton];
            const auto joint = sk.find("joint1");
            assert(joint >= 0 && !clip.channels[joint].rotation.empty() && !clip.channels[joint].translation.empty());
            Pose3D posed;
            sample_clip(clip, sk, 0.5f, false, posed);
            assert(!close(posed.joints[joint].rotation, Quatf{}));
        }
    }
}

void test_loader_texture_transform() {
    const std::array<float, 15> floats{0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1};
    std::vector<std::uint8_t> bytes(sizeof floats);
    std::memcpy(bytes.data(), floats.data(), sizeof floats);
    const std::string json = R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],
        "nodes":[{"mesh":0}],"meshes":[{"primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1},"material":0}]}],
        "materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0,"extensions":{
            "KHR_texture_transform":{"offset":[0.25,0.5],"scale":[2,3],"rotation":1.5707963267948966}}}}}],
        "textures":[{"source":0}],"images":[{"uri":"unused.png"}],
        "bufferViews":[{"buffer":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":24}],
        "accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},
            {"bufferView":1,"componentType":5126,"count":3,"type":"VEC2"}],
        "buffers":[{"byteLength":60,"uri":"data:application/octet-stream;base64,)" +
                             skin_fixtures::base64(bytes) + "\"}]}";
    ModelData3D data;
    std::string error;
    assert(kin::test::read_value3d(kin::read_model_data(json, "texture-transform.gltf"), data, error));
    assert(data.parts.size() == 1 && data.parts[0].mesh.vertices.size() == 3);
    const auto& vertices = data.parts[0].mesh.vertices;
    assert(close(vertices[0].uv.x, .25f) && close(vertices[0].uv.y, .5f));
    assert(close(vertices[1].uv.x, .25f) && close(vertices[1].uv.y, 2.5f));
    assert(close(vertices[2].uv.x, -2.75f) && close(vertices[2].uv.y, .5f));
}

void test_loader_skin_limits() {
    // 256 joints are the most: this one is kept; 257 is refused, logged, the model static.
    GltfOptions o;
    o.joint_count = 256;
    auto data = read(skin_fixtures::make_gltf(o));
    assert(data.skeletons.size() == 1 && data.skeletons[0].joint_count() == 256 && data.skeletons[0].validate() &&
           data.parts[0].mesh.skinned());
    assert(data.clips.size() == 1);
    o.joint_count = 257;
    data = read(skin_fixtures::make_gltf(o));
    assert(data.skeletons.empty() && data.clips.empty());
    assert(data.parts.size() == 1 && data.parts[0].skin == -1 && !data.parts[0].mesh.skinned());
    // Static, at its node's placing (as a model without a skin would be).
    assert(data.parts[0].mesh.vertices.size() == 6);
}

// `values` appended to `buffer` (from a multiple of four bytes): their buffer view, as {offset, length}.
template <class T>
std::pair<std::size_t, std::size_t> append_values(std::vector<std::uint8_t>& buffer, const std::vector<T>& values) {
    while (buffer.size() % 4 != 0)
        buffer.push_back(0);
    const std::size_t offset = buffer.size();
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(values.data());
    buffer.insert(buffer.end(), bytes, bytes + values.size() * sizeof(T));
    return {offset, values.size() * sizeof(T)};
}

// A .gltf of `rest` (its nodes, meshes, accessors and so on), with `buffer` embedded as buffer 0 and `views` its buffer
// views.
std::string test_gltf(const std::vector<std::uint8_t>& buffer,
                      const std::vector<std::pair<std::size_t, std::size_t>>& views, const std::string& rest) {
    std::ostringstream j;
    j << "{\"asset\":{\"version\":\"2.0\"}," << rest << ",\"buffers\":[{\"byteLength\":" << buffer.size()
      << ",\"uri\":\"data:application/octet-stream;base64," << skin_fixtures::base64(buffer)
      << "\"}],\"bufferViews\":[";
    for (std::size_t i = 0; i < views.size(); ++i) {
        j << (i ? "," : "") << "{\"buffer\":0,\"byteOffset\":" << views[i].first
          << ",\"byteLength\":" << views[i].second << "}";
    }
    j << "]}";
    return j.str();
}

// Whether a warning `message` with a detail holding `detail` was logged.
bool logged(const std::vector<LogEvent>& events, std::string_view message, std::string_view detail) {
    return std::any_of(events.begin(), events.end(), [&](const LogEvent& event) {
        return event.message == message &&
               std::any_of(event.fields.begin(), event.fields.end(), [&](const LogField& field) {
                   return field.name == "detail" && field.value.find(detail) != std::string::npos;
               });
    });
}

void test_loader_refuses_unreadable_geometry() {
    // One triangle three ways: plainly, with its POSITION sparse (a corner replaced), and with its POSITION read as
    // VEC2. cgltf reads neither of the last two element by element (it hands back nothing, or a z of 0): they are left
    // out and logged, not drawn as a heap of vertices at the origin.
    std::vector<std::uint8_t> buffer;
    const auto corners = append_values(buffer, std::vector<float>{0, 0, 0, 1, 0, 0, 0, 1, 0});
    const auto sparse_index = append_values(buffer, std::vector<std::uint16_t>{1});
    const auto sparse_value = append_values(buffer, std::vector<float>{5, 5, 5});
    const std::string json = test_gltf(
        buffer, {corners, sparse_index, sparse_value},
        "\"scene\":0,\"scenes\":[{\"nodes\":[0,1,2]}],"
        "\"nodes\":[{\"name\":\"plain\",\"mesh\":0},{\"name\":\"sparse\",\"mesh\":1},{\"name\":\"flat\",\"mesh\":2}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]},{\"primitives\":[{\"attributes\":{"
        "\"POSITION\":1}}]},"
        "{\"primitives\":[{\"attributes\":{\"POSITION\":2}}]}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"sparse\":{\"count\":1,"
        "\"indices\":{\"bufferView\":1,\"componentType\":5123},\"values\":{\"bufferView\":2}}},"
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"}]");
    std::vector<LogEvent> events;
    const LoggerConfig before = logger_config();
    set_logger_config({.min_level = LogLevel::Warn, .sdl_sink = false, .memory_events = &events});
    ModelData3D data;
    std::string error;
    const bool ok = kin::test::read_value3d(kin::read_model_data(json, "refusals.gltf"), data, error);
    set_logger_config(before);
    assert(ok);
    assert(data.parts.size() == 1 && data.parts[0].name == "plain" && data.parts[0].mesh.vertices.size() == 3);
    assert(close(data.parts[0].mesh.bounds().max, {1, 1, 0}));
    const char* left_out = "3D: a mesh primitive is left out (its geometry cannot be read)";
    assert(logged(events, left_out, "sparse: a sparse accessor is not read"));
    assert(logged(events, left_out, "flat: an attribute has the wrong number of components"));
}

void test_loader_keeps_nodes_between_joints() {
    // Hips (a joint) -> SpineOffset (a plain node, moved and turned) -> Spine (a joint), under an Armature moved 2 in
    // x. The inverse bind matrices are the joints' world matrices inverted, as an exporter makes them, so every joint
    // matrix is the identity at rest only when the offset is in the skeleton (without it Spine rests 0.5 too low and
    // unturned).
    const Quatf turn = Quatf{0.0f, 0.0f, 0.70710678f, 0.70710678f}.normalized(); // 90 degrees about Z
    const Mat4f hips_world = Mat4f::translation({2, 0, 0}) * Mat4f::translation({0, 1, 0});
    const Mat4f spine_world =
        hips_world * Mat4f::trs({0, 0.5f, 0}, turn, {1, 1, 1}) * Mat4f::translation({0, 0.25f, 0});
    std::vector<float> binds;
    for (const Mat4f& world : {hips_world, spine_world}) {
        const Mat4f inverse = world.inverse();
        binds.insert(binds.end(), inverse.m, inverse.m + 16);
    }
    const Quatf bow = Quatf::axis_angle({1, 0, 0}, 30);
    std::vector<std::uint8_t> buffer;
    const auto corners = append_values(buffer, std::vector<float>{-0.5f, 1.5f, 0, 0.5f, 1.5f, 0, 0, 2, 0});
    const auto joints = append_values(
        buffer, std::vector<std::uint8_t>{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}); // the file's joint 1: Spine
    const auto weights = append_values(buffer, std::vector<float>{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0});
    const auto inverse_binds = append_values(buffer, binds);
    const auto times = append_values(buffer, std::vector<float>{0, 1});
    const auto rotations = append_values(buffer, std::vector<float>{0, 0, 0, 1, bow.x, bow.y, bow.z, bow.w});
    const std::string json =
        test_gltf(buffer, {corners, joints, weights, inverse_binds, times, rotations},
                  "\"scene\":0,\"scenes\":[{\"nodes\":[0,4]}],"
                  "\"nodes\":[{\"name\":\"Armature\",\"translation\":[2,0,0],\"children\":[1]},"
                  "{\"name\":\"Hips\",\"translation\":[0,1,0],\"children\":[2]},"
                  "{\"name\":\"SpineOffset\",\"translation\":[0,0.5,0],\"rotation\":[0,0,0.70710678,0.70710678],"
                  "\"children\":[3]},"
                  "{\"name\":\"Spine\",\"translation\":[0,0.25,0]},"
                  "{\"name\":\"body\",\"mesh\":0,\"skin\":0}],"
                  "\"skins\":[{\"inverseBindMatrices\":3,\"joints\":[1,3]}],"
                  "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"JOINTS_0\":1,\"WEIGHTS_0\":2}}]}],"
                  "\"animations\":[{\"name\":\"bow\",\"samplers\":[{\"input\":4,\"output\":5}],"
                  "\"channels\":[{\"sampler\":0,\"target\":{\"node\":3,\"path\":\"rotation\"}}]}],"
                  "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
                  "{\"bufferView\":1,\"componentType\":5121,\"count\":3,\"type\":\"VEC4\"},"
                  "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC4\"},"
                  "{\"bufferView\":3,\"componentType\":5126,\"count\":2,\"type\":\"MAT4\"},"
                  "{\"bufferView\":4,\"componentType\":5126,\"count\":2,\"type\":\"SCALAR\",\"min\":[0],\"max\":[1]},"
                  "{\"bufferView\":5,\"componentType\":5126,\"count\":2,\"type\":\"VEC4\"}]");
    ModelData3D data;
    std::string error;
    assert(kin::test::read_value3d(kin::read_model_data(json, "between.gltf"), data, error));
    assert(data.skeletons.size() == 1 && data.parts.size() == 1 && data.parts[0].skin == 0);
    const Skeleton3D& sk = data.skeletons[0];
    const i32 hips = sk.find("Hips"), offset = sk.find("SpineOffset"), spine = sk.find("Spine");
    assert(sk.joint_count() == 3 && sk.validate() && hips >= 0 && offset >= 0 && spine >= 0);
    assert(sk.parents[static_cast<std::size_t>(offset)] == hips &&
           sk.parents[static_cast<std::size_t>(spine)] == offset);
    assert(close(sk.root_transform.m[12], 2.0f));
    std::vector<Mat4f> matrices;
    compute_joint_matrices(sk, sk.rest_pose(), matrices);
    for (const Mat4f& m : matrices) {
        const Mat4f identity{};
        for (int i = 0; i < 16; ++i)
            assert(close(m.m[i], identity.m[i], 1e-5f));
    }
    // The vertices, bound to the file's joint 1, are bound to Spine in the skeleton's order; the clip moves Spine, not
    // the offset.
    assert(data.parts[0].mesh.skin.size() == 3);
    for (const MeshSkinVertex3D& v : data.parts[0].mesh.skin)
        assert(v.joints[0] == static_cast<u8>(spine) && close(v.weights[0], 1.0f));
    assert(data.clips.size() == 1 && data.clips[0].channels.size() == 3);
    assert(data.clips[0].channels[static_cast<std::size_t>(spine)].rotation.times.size() == 2);
    assert(data.clips[0].channels[static_cast<std::size_t>(offset)].rotation.times.empty());
}

void test_loader_refuses_hostile_files() {
    std::vector<std::uint8_t> buffer;
    const auto corners = append_values(buffer, std::vector<float>{0, 0, 0, 1, 0, 0, 0, 1, 0});
    const std::string triangle =
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}]";
    // Nodes nested `depth` deep, the triangle on the deepest: 512 are read (a 256-joint chain must); more are refused,
    // for the reader recurses.
    const auto nested = [&](int depth) {
        std::string nodes = "\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[";
        for (int i = 0; i + 1 < depth; ++i)
            nodes += "{\"children\":[" + std::to_string(i + 1) + "]},";
        return test_gltf(buffer, {corners}, nodes + "{\"mesh\":0}]," + triangle);
    };
    ModelData3D data;
    std::string error;
    assert(kin::test::read_value3d(kin::read_model_data(nested(512), "nested.gltf"), data, error) &&
           data.parts.size() == 1);
    assert(!kin::test::read_value3d(kin::read_model_data(nested(520), "nested.gltf"), data, error) &&
           error.find("nest more than 512 deep") != std::string::npos);

    // A model under the content root may name a buffer up a folder ("../"), but not one that ".." takes out of the
    // root, nor an absolute path, even when the file is there: that model does not load. A loose model outside any root
    // keeps "..".
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() /
        ("kin-skeleton-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(folder / "content" / "models");
    std::filesystem::create_directories(
        folder / "loose"); // "loose/../triangle.bin" needs the folder to exist for the OS to resolve ".."
    for (const std::filesystem::path& file : {folder / "triangle.bin", folder / "content" / "triangle.bin"}) {
        std::ofstream out{file, std::ios::binary};
        out.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    }
    const auto naming = [&](const std::string& uri) {
        std::string json = test_gltf(buffer, {corners}, "\"nodes\":[{\"mesh\":0}]," + triangle);
        const std::size_t at = json.find("data:application/octet-stream");
        json.replace(at, json.find('"', at) - at, uri);
        return json;
    };
    const auto reads = [&](const std::string& uri, const std::filesystem::path& model) {
        return kin::test::read_value3d(kin::read_model_data(naming(uri), model), data, error) && data.parts.size() == 1;
    };
    char program[] = "skeleton-tests";
    std::string root_arg = "--content=" + (folder / "content").string(),
                none_arg = "--content=" + (folder / "nowhere").string();
    char* root_argv[] = {program, root_arg.data()};
    char* none_argv[] = {program, none_arg.data()};
    const bool rooted = !find_content_root({}, 2, root_argv).empty();
    const bool beside = reads("triangle.bin", folder / "content" / "beside.gltf");
    const bool up_inside = reads("../triangle.bin", folder / "content" / "models" / "climbs.gltf");
    const bool up_out = reads("../../triangle.bin", folder / "content" / "models" / "escapes.gltf");
    const std::string up_out_error = error;
    const bool absolute =
        reads((folder / "content" / "triangle.bin").generic_string(), folder / "content" / "absolute.gltf");
    const bool loose = reads("../triangle.bin", folder / "loose" / "loose.gltf");
    find_content_root({}, 2, none_argv); // no root again: content_root() is empty
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
    assert(rooted && content_root().empty());
    assert(beside && up_inside && loose);
    assert(!up_out && up_out_error.find("outside its content") != std::string::npos);
    assert(!absolute);
}

// The CPU cost of animating one character a frame, printed (no assertion: a machine's speed is not a test): a clip
// sampled and its joint matrices made, and a crossfade (two clips sampled, blended, then the matrices) through the
// player. The synthetic skeleton has 24 joints with three tracks of 30 keys each, as a game's character might; the Fox
// from the reference cache is timed too when it is there.
void bench_animation() {
    const auto per_call = [](int iterations, const auto& body) {
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < iterations; ++i) {
            body(i);
        }
        return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / iterations;
    };
    Skeleton3D sk;
    const int n = 24;
    sk.parents.resize(n);
    sk.names.resize(n);
    sk.rest.resize(n);
    sk.inverse_bind.resize(n);
    AnimClip3D clip;
    clip.channels.resize(n);
    for (int j = 0; j < n; ++j) {
        sk.parents[j] = j == 0 ? -1 : (j - 1) / 2;
        sk.rest[j].translation = {0.0f, 0.2f, 0.0f};
        JointChannels3D& c = clip.channels[j];
        for (int k = 0; k < 30; ++k) {
            const float t = static_cast<float>(k) / 29.0f;
            c.translation.times.push_back(t);
            c.translation.values.push_back({0.0f, 0.2f + 0.01f * std::sin(t * 6.0f), 0.0f});
            c.rotation.times.push_back(t);
            c.rotation.values.push_back(
                Quatf::axis_angle({0, 0, 1}, 30.0f * std::sin(t * 6.0f + static_cast<float>(j))));
            c.scale.times.push_back(t);
            c.scale.values.push_back({1, 1, 1});
        }
    }
    clip.duration = 1.0f;
    float sink = 0.0f;
    Pose3D pose;
    std::vector<Mat4f> matrices;
    const double sample = per_call(20000, [&](int i) {
        sample_clip(clip, sk, static_cast<float>(i % 100) * 0.0101f, true, pose);
        sink += pose.joints[3].translation.y;
    });
    const double matrices_only = per_call(20000, [&](int) {
        compute_joint_matrices(sk, pose, matrices);
        sink += matrices[5].m[13];
    });
    AnimationPlayer3D player{&sk};
    player.play(&clip);
    const double single = per_call(20000, [&](int) {
        player.update(1.0f / 60.0f);
        player.joint_matrices(matrices);
        sink += matrices[7].m[12];
    });
    AnimClip3D other = clip;
    AnimationPlayer3D fading{&sk};
    fading.play(&clip);
    fading.crossfade_to(&other, 1000.0f); // never finishes: two clips sampled and blended every update
    const double faded = per_call(20000, [&](int) {
        fading.update(1.0f / 60.0f);
        fading.joint_matrices(matrices);
        sink += matrices[7].m[12];
    });
    std::cerr << "animation, a 24-joint character (3 tracks of 30 keys a joint): sample_clip " << sample
              << " us, joint matrices " << matrices_only << " us, player update + matrices " << single
              << " us, with a crossfade " << faded << " us (a frame at 60 Hz is 16,667 us) [" << sink << "]\n";
    // The real Fox, when the reference cache has it.
    const char* cache = std::getenv("KIN_REFERENCE_CACHE");
    const std::filesystem::path fox =
        (cache ? std::filesystem::path{cache}
               : std::filesystem::path{std::getenv("HOME") ? std::getenv("HOME") : "."} / ".cache/kin/reference") /
        "fox/Fox.glb";
    std::ifstream in(fox, std::ios::binary);
    if (!in) {
        std::cerr
            << "animation: the Fox is not in the reference cache (tools/reference/fetch.py): its clips not timed\n";
        return;
    }
    const std::string bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    ModelData3D data;
    std::string error;
    if (!kin::test::read_value3d(kin::read_model_data(bytes, fox), data, error)) {
        std::cerr << "animation: " << error << "\n";
        return;
    }
    assert(data.skeletons.size() == 1 && data.clips.size() == 3);
    const Skeleton3D& fsk = data.skeletons[0];
    AnimationPlayer3D fp{&fsk};
    fp.play(&data.clips[1]);
    const double fox_us = per_call(20000, [&](int) {
        fp.update(1.0f / 60.0f);
        fp.joint_matrices(matrices);
        sink += matrices[3].m[12];
    });
    AnimationPlayer3D fx{&fsk};
    fx.play(&data.clips[1]);
    fx.crossfade_to(&data.clips[2], 1000.0f);
    const double fox_fade = per_call(20000, [&](int) {
        fx.update(1.0f / 60.0f);
        fx.joint_matrices(matrices);
        sink += matrices[3].m[12];
    });
    std::cerr << "animation, the real Fox (" << fsk.joint_count() << " joints, Walk clip " << data.clips[1].duration
              << " s): player update + matrices " << fox_us << " us, Walk to Run crossfade " << fox_fade << " us ["
              << sink << "]\n";
}

// --- P1a part 2: 1D blends in the player, a shared pose in the scene, the ECS animator
// ----------------------------------------

// A clip that swings `joint` about Z by `degrees` and back over `duration` seconds.
AnimClip3D swing_clip(const Skeleton3D& sk, const char* name, float duration, float degrees, u32 joint = 1) {
    AnimClip3D clip;
    clip.name = name;
    clip.duration = duration;
    clip.channels.resize(sk.joint_count());
    Track3D<Quatf>& r = clip.channels[joint].rotation;
    r.interpolation = Interpolation3D::Linear;
    r.times = {0.0f, duration * 0.5f, duration};
    r.values = {rz(0), rz(degrees), rz(0)};
    return clip;
}

float max_difference(std::span<const Mat4f> a, std::span<const Mat4f> b) {
    assert(a.size() == b.size());
    float worst = 0.0f;
    for (std::size_t i = 0; i < a.size(); ++i) {
        for (int k = 0; k < 16; ++k) {
            worst = std::max(worst, std::fabs(a[i].m[k] - b[i].m[k]));
        }
    }
    return worst;
}

float max_difference(const Pose3D& a, const Pose3D& b) {
    assert(a.joints.size() == b.joints.size());
    float worst = 0.0f;
    for (std::size_t i = 0; i < a.joints.size(); ++i) {
        const Vec3f t = a.joints[i].translation - b.joints[i].translation, s = a.joints[i].scale - b.joints[i].scale;
        worst = std::max(
            {worst, std::fabs(t.x), std::fabs(t.y), std::fabs(t.z), std::fabs(s.x), std::fabs(s.y), std::fabs(s.z)});
        const Quatf q = a.joints[i].rotation, p = b.joints[i].rotation;
        const float d = std::fabs(q.x * p.x + q.y * p.y + q.z * p.z + q.w * p.w);
        worst = std::max(worst, 1.0f - std::min(d, 1.0f));
    }
    return worst;
}

// --- animated nodes
// --------------------------------------------------------------------------------------------------------

// A small crane in a glTF: "base" (a triangle, at x 1) carries "arm" (2 up), which carries "tip" (a triangle 1 out in
// z, twice the size); "ground" (a triangle at x 5) hangs from nothing that moves, and "marker" carries no mesh. The
// animation "swing" moves the arm (2 to 3 up, and a turn of 90 degrees about Y, over a second), the marker, and
// "elsewhere", a mesh of another scene. Without `animated` there is no animation; with `meshless` only the marker
// moves.
std::string crane_gltf(bool animated = true, bool meshless = false) {
    std::vector<std::uint8_t> buffer;
    const auto corners = append_values(buffer, std::vector<float>{0, 0, 0, 1, 0, 0, 0, 1, 0});
    const auto times = append_values(buffer, std::vector<float>{0, 1});
    const auto moves = append_values(buffer, std::vector<float>{0, 2, 0, 0, 3, 0});
    const Quatf turn = Quatf::axis_angle({0, 1, 0}, 90);
    const auto turns = append_values(buffer, std::vector<float>{0, 0, 0, 1, turn.x, turn.y, turn.z, turn.w});
    std::string json = "\"scene\":0,\"scenes\":[{\"nodes\":[0,3,5]},{\"nodes\":[4]}],"
                       "\"nodes\":[{\"name\":\"base\",\"mesh\":0,\"translation\":[1,0,0],\"children\":[1]},"
                       "{\"name\":\"arm\",\"translation\":[0,2,0],\"children\":[2]},"
                       "{\"name\":\"tip\",\"mesh\":0,\"translation\":[0,0,1],\"scale\":[2,2,2]},"
                       "{\"name\":\"ground\",\"mesh\":0,\"translation\":[5,0,0]},"
                       "{\"name\":\"elsewhere\",\"mesh\":0},"
                       "{\"name\":\"marker\",\"translation\":[0,0,-3]}],"
                       "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}],";
    if (animated && meshless) {
        json += "\"animations\":[{\"name\":\"idle\",\"samplers\":[{\"input\":1,\"output\":2}],"
                "\"channels\":[{\"sampler\":0,\"target\":{\"node\":5,\"path\":\"translation\"}}]}],";
    } else if (animated) {
        json +=
            "\"animations\":[{\"name\":\"swing\",\"samplers\":[{\"input\":1,\"output\":2},{\"input\":1,\"output\":3}],"
            "\"channels\":[{\"sampler\":0,\"target\":{\"node\":1,\"path\":\"translation\"}},"
            "{\"sampler\":1,\"target\":{\"node\":1,\"path\":\"rotation\"}},"
            "{\"sampler\":0,\"target\":{\"node\":4,\"path\":\"translation\"}},"
            "{\"sampler\":0,\"target\":{\"node\":5,\"path\":\"translation\"}}]}],";
    }
    json += "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
            "{\"bufferView\":1,\"componentType\":5126,\"count\":2,\"type\":\"SCALAR\",\"min\":[0],\"max\":[1]},"
            "{\"bufferView\":2,\"componentType\":5126,\"count\":2,\"type\":\"VEC3\"},"
            "{\"bufferView\":3,\"componentType\":5126,\"count\":2,\"type\":\"VEC4\"}]";
    return test_gltf(buffer, {corners, times, moves, turns}, json);
}

// The crane read with a fake mesh for each part (no GPU): a Model3D to pose and draw on the CPU.
std::shared_ptr<Model3D> crane_model(const ModelData3D& data) {
    struct FakeMesh final : IMeshBackend3D {};
    auto model = std::make_shared<Model3D>();
    for (const ModelPartData3D& part : data.parts) {
        model->parts.push_back({part.name,
                                Mesh3D{std::make_shared<FakeMesh>(), 3, 3, part.mesh.bounds()},
                                {},
                                part.transform,
                                part.skin,
                                part.node,
                                part.node_offset});
    }
    model->skeletons = data.skeletons;
    model->clips = data.clips;
    model->node_skeleton = data.node_skeleton;
    return model;
}

bool close(const Mat4f& a, const Mat4f& b, float eps = 1e-5f) {
    for (int i = 0; i < 16; ++i) {
        if (!close(a.m[i], b.m[i], eps))
            return false;
    }
    return true;
}
bool same(const Mat4f& a, const Mat4f& b) {
    return std::memcmp(a.m, b.m, sizeof(a.m)) == 0;
}

const ModelPart3D& part_called(const Model3D& model, std::string_view name) {
    const auto found =
        std::find_if(model.parts.begin(), model.parts.end(), [&](const ModelPart3D& p) { return p.name == name; });
    assert(found != model.parts.end());
    return *found;
}
std::size_t index_of(const Model3D& model, std::string_view name) {
    return static_cast<std::size_t>(&part_called(model, name) - model.parts.data());
}

// Where the crane's tip is in the model at `t` seconds of "swing": base, the arm moved and turned, the tip below it.
Mat4f crane_tip_at(float t) {
    return Mat4f::translation({1, 0, 0}) * Mat4f::trs({0, 2 + t, 0}, Quatf::axis_angle({0, 1, 0}, 90 * t), {1, 1, 1}) *
           Mat4f::trs({0, 0, 1}, Quatf{}, {2, 2, 2});
}

// The plain nodes a glTF animation moves are a skeleton of the model, its node skeleton: the moved nodes and those
// above them, parents first, the nodes' names and rest placings, identity inverse binds; its clip has the nodes'
// channels; each part that is not skinned hangs from its nearest node in it, and a pose places it at that node's matrix
// times its placing below. A file without the animation reads exactly as before, and nodes the model draws nothing
// below, or of another scene, are not animated.
void test_loader_node_animation() {
    std::vector<LogEvent> events;
    const LoggerConfig before = logger_config();
    set_logger_config({.min_level = LogLevel::Warn, .sdl_sink = false, .memory_events = &events});
    ModelData3D data, plain, meshless;
    std::string error;
    const bool read_crane = kin::test::read_value3d(kin::read_model_data(crane_gltf(), "crane.gltf"), data, error);
    const bool read_plain =
        kin::test::read_value3d(kin::read_model_data(crane_gltf(false), "plain.gltf"), plain, error);
    const bool read_meshless =
        kin::test::read_value3d(kin::read_model_data(crane_gltf(true, true), "meshless.gltf"), meshless, error);
    set_logger_config(before);
    assert(read_crane && read_plain && read_meshless);

    // The node skeleton: base and arm (the arm's parent), then the marker, a root of its own; "elsewhere" is another
    // scene's.
    assert(data.skeletons.size() == 1 && data.node_skeleton == 0);
    const Skeleton3D& sk = data.skeletons[0];
    assert(sk.validate() && (sk.names == std::vector<std::string>{"base", "arm", "marker"}) &&
           (sk.parents == std::vector<i32>{-1, 0, -1}));
    assert(close(sk.rest[0].translation, {1, 0, 0}) && close(sk.rest[1].translation, {0, 2, 0}) &&
           close(sk.rest[2].translation, {0, 0, -3}));
    for (const Mat4f& m : sk.inverse_bind)
        assert(same(m, Mat4f{}));
    assert(same(sk.root_transform, Mat4f{}));
    assert(data.clips.size() == 1);
    const AnimClip3D& swing = data.clips[0];
    assert(swing.name == "swing" && swing.skeleton == 0 && swing.channels.size() == 3 && close(swing.duration, 1.0f));
    assert(swing.channels[0].translation.empty() && swing.channels[0].rotation.empty());
    assert(swing.channels[1].translation.times.size() == 2 && swing.channels[1].rotation.times.size() == 2 &&
           swing.channels[1].scale.empty());
    assert(swing.channels[2].translation.times.size() == 2);
    assert(logged(events, "3D: an animation moves nodes no part of the model hangs from: those channels are ignored",
                  "swing"));

    // The parts: the base hangs from its own node, the tip from the arm (its placing below it kept), the ground from
    // nothing.
    const std::shared_ptr<Model3D> crane = crane_model(data);
    assert(crane->parts.size() == 3);
    const ModelPart3D& base = part_called(*crane, "base");
    const ModelPart3D& tip = part_called(*crane, "tip");
    const ModelPart3D& ground = part_called(*crane, "ground");
    assert(base.node == 0 && same(base.node_offset, Mat4f{}) && tip.node == 1 && ground.node == -1 &&
           same(ground.node_offset, Mat4f{}));
    assert(close(tip.node_offset, Mat4f::trs({0, 0, 1}, Quatf{}, {2, 2, 2})) &&
           close(tip.transform, crane_tip_at(0.0f)));

    // Posed: the player's joint matrices are the nodes' world matrices (identity inverse binds), and part_transform
    // places each.
    AnimationPlayer3D player{&crane->skeletons[0]};
    player.play(crane->find_clip("swing"), false);
    std::vector<Mat4f> nodes, world;
    player.joint_matrices(nodes);
    for (std::size_t i = 0; i < crane->parts.size(); ++i)
        assert(close(crane->part_transform(i, nodes), crane->parts[i].transform, 1e-6f));
    player.update(0.5f);
    player.joint_matrices(nodes);
    compute_world_matrices(crane->skeletons[0], player.pose(), world);
    assert(nodes.size() == 3 && max_difference(nodes, world) == 0.0f);
    assert(close(crane->part_transform(index_of(*crane, "tip"), nodes), crane_tip_at(0.5f)));
    assert(close(crane->part_transform(index_of(*crane, "base"), nodes), base.transform));
    assert(same(crane->part_transform(index_of(*crane, "ground"), nodes), ground.transform));
    assert(close(nodes[2].apply({0, 0, 0}),
                 {0, 2.5f, 0.0f})); // the marker moves too (its track replaces its rest), carrying nothing
    // Too few matrices, or a part past the parts: at rest, and the identity.
    assert(same(crane->part_transform(index_of(*crane, "tip"), std::span<const Mat4f>{nodes}.first(1)), tip.transform));
    assert(same(crane->part_transform(99, nodes), Mat4f{}));

    // Without the animation: no node skeleton, no clip, every part as it was (its placing to the bit, hanging from
    // nothing).
    assert(plain.skeletons.empty() && plain.clips.empty() && plain.node_skeleton == -1 &&
           plain.parts.size() == data.parts.size());
    for (std::size_t i = 0; i < plain.parts.size(); ++i) {
        const ModelPartData3D& a = plain.parts[i];
        const ModelPartData3D& b = data.parts[i];
        assert(a.name == b.name && same(a.transform, b.transform) && a.skin == -1 && a.node == -1 &&
               same(a.node_offset, Mat4f{}));
        assert(a.mesh.vertices.size() == b.mesh.vertices.size() &&
               std::memcmp(a.mesh.vertices.data(), b.mesh.vertices.data(),
                           a.mesh.vertices.size() * sizeof(MeshVertex3D)) == 0);
    }
    // Only a node that carries no mesh moves: nothing to pose, so no node skeleton, and the animation is left out (said
    // so).
    assert(meshless.skeletons.empty() && meshless.clips.empty() && meshless.node_skeleton == -1);
    assert(logged(events, "3D: an animation moves no joint of a skin and no node a part hangs from: it is left out",
                  "idle"));
    for (const ModelPartData3D& part : meshless.parts)
        assert(part.node == -1);

    // More moved nodes (with those above them) than a skeleton may have: not animated, said so, the parts at rest.
    {
        std::vector<std::uint8_t> buffer;
        const auto corners = append_values(buffer, std::vector<float>{0, 0, 0, 1, 0, 0, 0, 1, 0});
        const auto times = append_values(buffer, std::vector<float>{0, 1});
        const auto moves = append_values(buffer, std::vector<float>{0, 0, 0, 1, 0, 0});
        std::string nodes_json = "\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[";
        for (int i = 0; i < 299; ++i)
            nodes_json += "{\"children\":[" + std::to_string(i + 1) + "]},";
        const std::string json = test_gltf(
            buffer, {corners, times, moves},
            nodes_json +
                "{\"mesh\":0}],"
                "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}],"
                "\"animations\":[{\"name\":\"slide\",\"samplers\":[{\"input\":1,\"output\":2}],"
                "\"channels\":[{\"sampler\":0,\"target\":{\"node\":299,\"path\":\"translation\"}}]}],"
                "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
                "{\"bufferView\":1,\"componentType\":5126,\"count\":2,\"type\":\"SCALAR\",\"min\":[0],\"max\":[1]},"
                "{\"bufferView\":2,\"componentType\":5126,\"count\":2,\"type\":\"VEC3\"}]");
        events.clear();
        set_logger_config({.min_level = LogLevel::Warn, .sdl_sink = false, .memory_events = &events});
        ModelData3D deep;
        const bool ok = kin::test::read_value3d(kin::read_model_data(json, "deep.gltf"), deep, error);
        set_logger_config(before);
        assert(ok && deep.parts.size() == 1 && deep.parts[0].node == -1 && deep.skeletons.empty() &&
               deep.clips.empty());
        assert(logged(events, "3D: animations move more nodes than a skeleton can have: those nodes are not animated",
                      "300 nodes"));
    }
}

// Drawing the crane's pose: Scene3D::draw(model, transform, node_matrices) places the parts that hang from a node by it
// and the others as draw(model) does (to the bit), at rest for a pose that lacks a part's matrix (said once);
// draw_skinned with node matrices poses a skinned model's animated props alongside its skin. An entity with a
// ModelRenderer3D and an Animator3D plays the clip on the nodes; one without an animator (or before its first animate)
// draws exactly what it drew before.
void test_node_animation_draws() {
    ModelData3D data;
    std::string error;
    assert(kin::test::read_value3d(kin::read_model_data(crane_gltf(), "crane.gltf"), data, error));
    const std::shared_ptr<Model3D> crane = crane_model(data);
    const Skeleton3D& sk = crane->skeletons[static_cast<std::size_t>(crane->node_skeleton)];
    Pose3D pose;
    sample_clip(*crane->find_clip("swing"), sk, 0.5f, false, pose);
    std::vector<Mat4f> nodes;
    compute_world_matrices(sk, pose, nodes);
    const Mat4f placed = Mat4f::translation({0, 0, -2});
    const auto transform_of = [](const Scene3D& scene, const Mesh3D& mesh) {
        for (const Scene3D::Draw& draw : scene.draws()) {
            if (draw.mesh == mesh)
                return scene.instances()[draw.first].transform;
        }
        assert(false);
        return Mat4f{};
    };
    {
        Scene3D scene, still;
        (void)scene.draw(*crane, placed, nodes, colors::white, 9);
        (void)still.draw(*crane, placed, colors::white, 9);
        assert(scene.draws().size() == 3 && scene.instances().size() == 3 && scene.instances()[0].id == 9);
        assert(close(transform_of(scene, part_called(*crane, "tip").mesh), placed * crane_tip_at(0.5f)));
        assert(same(transform_of(scene, part_called(*crane, "ground").mesh),
                    transform_of(still, part_called(*crane, "ground").mesh)));
        // A pose of one matrix: the tip has none, so it is at rest; said once.
        std::vector<LogEvent> events;
        const LoggerConfig before = logger_config();
        set_logger_config({.min_level = LogLevel::Warn, .sdl_sink = false, .memory_events = &events});
        Scene3D short_pose;
        (void)short_pose.draw(*crane, placed, std::span<const Mat4f>{nodes}.first(1));
        (void)short_pose.draw(*crane, placed, std::span<const Mat4f>{nodes}.first(1));
        set_logger_config(before);
        assert(same(short_pose.instances()[index_of(*crane, "tip")].transform,
                    placed * part_called(*crane, "tip").transform));
        assert(std::count_if(events.begin(), events.end(),
                             [](const LogEvent& e) { return e.message.find("node pose") != std::string::npos; }) == 1);
    }
    {
        // A skinned strip with an animated loose mesh: one call poses both; without node matrices the loose mesh is at
        // rest.
        GltfOptions o;
        o.node_animation = true;
        const ModelData3D both = read(skin_fixtures::make_gltf(o));
        struct FakeMesh final : IMeshBackend3D {};
        Model3D model;
        for (const ModelPartData3D& part : both.parts) {
            model.parts.push_back({part.name,
                                   part.skin >= 0 ? Mesh3D{std::make_shared<FakeMesh>(), 6, 12, {}, {}, {}, 2}
                                                  : Mesh3D{std::make_shared<FakeMesh>(), 6, 12, {}},
                                   {},
                                   part.transform,
                                   part.skin,
                                   part.node,
                                   part.node_offset});
        }
        model.skeletons = both.skeletons;
        model.clips = both.clips;
        model.node_skeleton = both.node_skeleton;
        assert(model.node_skeleton == 1 && model.parts[1].node == 0);
        std::vector<Mat4f> joints, loose;
        compute_joint_matrices(model.skeletons[0], model.skeletons[0].rest_pose(), joints);
        Pose3D moved;
        sample_clip(*model.find_clip("bend", 1), model.skeletons[1], 1.0f, false, moved);
        compute_world_matrices(model.skeletons[1], moved, loose);
        Scene3D scene, rest;
        (void)scene.draw_skinned(model, joints, Mat4f{}, colors::white, 3, {}, loose);
        (void)rest.draw_skinned(model, joints, Mat4f{}, colors::white, 3);
        assert(scene.draws().size() == 2 && scene.draws()[0].skinned && !scene.draws()[1].skinned);
        assert(close(scene.instances()[1].transform.apply({0, 0, 0}), {0, 2, 0})); // the stepped key of 1 s: 2 up
        assert(same(rest.instances()[1].transform, model.parts[1].transform));
        assert(max_difference(scene.joint_matrices(), rest.joint_matrices()) == 0.0f);
    }
    // The ECS: two cranes, one animated. The still one, and the animated one before its first animate(), draw what they
    // drew before.
    kin::EcsWorld world;
    WorldRenderState3D state{world};
    auto& w = world.raw();
    const Animator3D swinging = [] {
        Animator3D a;
        a.clip = "swing";
        a.loop = false;
        return a;
    }();
    const flecs::entity moving =
        w.entity().set(Transform3D{.position = {0, 0, -2}}).set(ModelRenderer3D{.model = crane}).set(swinging);
    const flecs::entity still =
        w.entity().set(Transform3D{.position = {0, 0, -2}}).set(ModelRenderer3D{.model = crane});
    const auto collected = [&](const flecs::entity& e, std::string_view part) {
        Scene3D scene;
        state.propagate_transforms();
        state.collect(scene);
        const Mesh3D& mesh = part_called(*crane, part).mesh;
        const u32 id = static_cast<u32>(e.id() & 0xffffffffu);
        for (const Scene3D::Draw& draw : scene.draws()) {
            if (!(draw.mesh == mesh))
                continue;
            for (u32 i = draw.first; i < draw.first + draw.count; ++i) {
                if (scene.instances()[i].id == id)
                    return scene.instances()[i].transform;
            }
        }
        assert(false);
        return Mat4f{};
    };
    const Mat4f entity = Mat4f::translation({0, 0, -2});
    for (const char* part : {"base", "tip", "ground"}) {
        assert(same(collected(moving, part), entity * part_called(*crane, part).transform));
        assert(same(collected(still, part), entity * part_called(*crane, part).transform));
    }
    state.animate(0.25f);
    state.animate(0.25f);
    assert(moving.get<Animator3D>()->player.skeleton() == &crane->skeletons[0] && still.get<Animator3D>() == nullptr);
    assert(close(collected(moving, "tip"), entity * crane_tip_at(0.5f)));
    assert(close(collected(moving, "base"), entity * part_called(*crane, "base").transform));
    for (const char* part : {"base", "tip", "ground"}) {
        assert(same(collected(still, part), entity * part_called(*crane, part).transform));
    }
    // Held at the end (no loop), and deterministic: a second world steps to the same matrices, to the bit.
    state.animate(1.0f);
    assert(close(collected(moving, "tip"), entity * crane_tip_at(1.0f)));
    kin::EcsWorld again;
    WorldRenderState3D again_state{again};
    const flecs::entity twin = again.raw()
                                   .entity()
                                   .set(Transform3D{.position = {0, 0, -2}})
                                   .set(ModelRenderer3D{.model = crane})
                                   .set(swinging);
    for (const float dt : {0.25f, 0.25f, 1.0f})
        again_state.animate(dt);
    Scene3D twin_scene;
    again_state.propagate_transforms();
    again_state.collect(twin_scene);
    const Mat4f twin_tip = transform_of(twin_scene, part_called(*crane, "tip").mesh);
    assert(same(twin_tip, collected(moving, "tip")) && twin.get<Animator3D>()->player.finished());
}

// A one-dimensional blend in the player: the clips of the two points either side of the parameter, each at the same
// phase of its own length, blended by how far the parameter is between them; the phase advances by the time over the
// duration blended at the parameter now (so a change of the parameter never jumps the phase); a parameter outside the
// points is held at the end; a fade to and from a blend.
void test_player_blend_1d() {
    const auto tube = skin_fixtures::make_tube();
    const Skeleton3D& sk = tube.skeleton;
    const AnimClip3D walk = swing_clip(sk, "walk", 1.0f, 40.0f, 1), run = swing_clip(sk, "run", 0.6f, -70.0f, 2),
                     jog = swing_clip(sk, "jog", 0.8f, 20.0f, 1);
    // Out of order on purpose (sorted by position), and a null clip, which is dropped.
    const BlendPoint3D points[] = {{&run, 4.5f}, {nullptr, 3.0f}, {&walk, 1.5f}, {&jog, 3.0f}};
    AnimationPlayer3D player{&sk};
    player.play_blend(points, 3.0f);
    assert(player.blending() && player.clip() == nullptr && close(player.time(), 0.0f));
    // At 3.0 exactly: the jog alone (weight 1 on the point), at phase 0.
    player.update(0.0f);
    Pose3D expected;
    sample_clip(jog, sk, 0.0f, true, expected);
    assert(max_difference(player.pose(), expected) < 1e-6f);
    // Between walk (1.5) and jog (3.0): parameter 2.1 is 0.4 of the way; durations 1.0 and 0.8 give 0.92 s.
    player.set_blend_parameter(2.1f);
    float phase = 0.0f;
    for (int i = 0; i < 25; ++i) {
        const float duration = 1.0f + (0.8f - 1.0f) * 0.4f;
        phase += 0.02f / duration;
        phase -= std::floor(phase);
        player.update(0.02f);
        Pose3D a, b;
        sample_clip(walk, sk, phase * 1.0f, true, a);
        sample_clip(jog, sk, phase * 0.8f, true, b);
        expected = blend_poses(a, b, 0.4f);
        assert(max_difference(player.pose(), expected) < 1e-4f);
        assert(close(player.time(), phase, 1e-4f));
    }
    // The parameter moves while it plays: the phase carries on from where it is (it advances at the speed of the new
    // duration).
    const float before = player.time();
    player.set_blend_parameter(4.0f); // between jog (3.0) and run (4.5): 2/3 of the way; durations 0.8 and 0.6
    player.update(0.05f);
    const float duration = 0.8f + (0.6f - 0.8f) * (1.0f / 1.5f);
    assert(close(player.time(), before + 0.05f / duration - std::floor(before + 0.05f / duration), 1e-4f));
    // Held at the ends: below the first point the first clip, above the last the last.
    player.set_blend_parameter(-5.0f);
    player.update(0.0f);
    sample_clip(walk, sk, player.time() * 1.0f, true, expected);
    assert(max_difference(player.pose(), expected) < 1e-5f);
    player.set_blend_parameter(99.0f);
    player.update(0.0f);
    sample_clip(run, sk, player.time() * 0.6f, true, expected);
    assert(max_difference(player.pose(), expected) < 1e-5f);
    // A fade from a plain clip to a blend, and from the blend to a clip, is the blend of the two pictures at the fade's
    // progress.
    AnimationPlayer3D fade{&sk};
    fade.play(&walk);
    fade.update(0.1f);
    const BlendPoint3D two[] = {{&walk, 0.0f}, {&run, 1.0f}};
    fade.crossfade_to_blend(two, 0.25f, 0.5f);
    fade.update(0.1f); // alpha 0.2
    {
        Pose3D from, a, b;
        sample_clip(walk, sk, 0.2f, true, from);
        const float d = 1.0f + (0.6f - 1.0f) * 0.25f;
        const float ph = 0.1f / d;
        sample_clip(walk, sk, ph, true, a);
        sample_clip(run, sk, ph * 0.6f, true, b);
        expected = blend_poses(from, blend_poses(a, b, 0.25f), 0.2f);
        assert(max_difference(fade.pose(), expected) < 1e-4f);
    }
    assert(fade.blending() && fade.fading_from() == &walk); // the plain clip fades out
}

// Scene3D::draw_skinned(model): one range of matrices for all the skinned parts, each copy's offset the same; plain
// parts are drawn as before.
void test_scene_shared_pose() {
    const auto tube = skin_fixtures::make_tube();
    struct FakeMesh final : IMeshBackend3D {};
    Model3D model;
    model.skeletons.push_back(tube.skeleton);
    for (int i = 0; i < 4; ++i) {
        ModelPart3D part;
        part.mesh =
            Mesh3D{std::make_shared<FakeMesh>(), 3, 3, {}, {}, {}, i == 3 ? 2u : 3u}; // the last uses two joints only
        part.skin = i == 2 ? -1 : 0; // and one is not skinned (a rigid prop)
        if (i == 2) {
            part.mesh = Mesh3D{std::make_shared<FakeMesh>(), 3, 3, {}};
        }
        model.parts.push_back(part);
    }
    std::vector<Mat4f> pose;
    compute_joint_matrices(tube.skeleton, tube.skeleton.rest_pose(), pose);
    Scene3D scene;
    assert(scene.draw_skinned(model, pose, Mat4f::identity(), colors::white, 17));
    // Three skinned copies (0, 1, 3), one plain (2); the pose held once, of the most any part uses (3 joints).
    assert(scene.draws().size() == 4 && scene.instances().size() == 4);
    assert(scene.joint_matrices().size() == 3 && scene.skin_offsets().size() == 3);
    assert(scene.skin_offsets()[0] == 0 && scene.skin_offsets()[1] == 0 && scene.skin_offsets()[2] == 0);
    assert(scene.instances()[0].id == 17 && scene.instances()[2].id == 17);
    // A second character after it: its own range.
    assert(scene.draw_skinned(model, pose, Mat4f::translation({1, 0, 0})));
    assert(scene.joint_matrices().size() == 6 && scene.skin_offsets().size() == 6 && scene.skin_offsets()[3] == 3 &&
           scene.skin_offsets()[5] == 3);
    // Invalid poses reject the complete submission, including its rigid parts,
    // and preserve already submitted commands until clear() resets the error.
    Scene3D few;
    assert(few.draw(model.parts[2].mesh, Mat4f::identity(), {}, colors::white, 91));
    assert(!few.draw_skinned(model, std::span<const Mat4f>{pose}.first(2), Mat4f::identity()));
    assert(few.draws().size() == 1 && few.instances().size() == 1 && few.instances()[0].id == 91);
    assert(few.joint_matrices().empty() && few.skin_offsets().empty() && few.diagnostics().size() == 1);
    assert(!few.draw_skinned(model, pose, Mat4f::identity()));
    few.clear();
    assert(few.diagnostics().empty() && few.draw_skinned(model, pose, Mat4f::identity()));
    assert(few.draws().size() == 4 && few.joint_matrices().size() == 3);
}

// --- the ECS animator ---

struct Character {
    std::shared_ptr<const Model3D> model;
    const Skeleton3D* skeleton = nullptr;
    const AnimClip3D* clip(const char* name) const { return model->find_clip(name); }
};

Character make_character() {
    const auto tube = skin_fixtures::make_tube();
    struct FakeMesh final : IMeshBackend3D {};
    auto model = std::make_shared<Model3D>();
    model->skeletons.push_back(tube.skeleton);
    const Skeleton3D& sk = model->skeletons[0];
    model->clips.push_back(swing_clip(sk, "idle", 1.0f, 10.0f, 1));
    model->clips.push_back(swing_clip(sk, "walk", 1.0f, 40.0f, 1));
    model->clips.push_back(swing_clip(sk, "run", 0.6f, -70.0f, 2));
    model->clips.push_back(swing_clip(sk, "jump", 0.5f, 80.0f, 2));
    model->clips.push_back(swing_clip(sk, "wave", 0.7f, 60.0f, 2));
    AnimClip3D root_walk;
    root_walk.name = "root_walk";
    root_walk.duration = 1.0f;
    root_walk.channels.resize(sk.joint_count());
    root_walk.channels[0].translation.times = {0.0f, 1.0f};
    root_walk.channels[0].translation.values = {{0, 0, 0}, {2, 0, 0}};
    model->clips.push_back(root_walk);
    ModelPart3D part;
    part.name = "tube";
    part.mesh = Mesh3D{std::make_shared<FakeMesh>(), 3, 3, {}, {}, {}, 3};
    part.skin = 0;
    model->parts.push_back(part);
    Character c;
    c.model = model;
    c.skeleton = &c.model->skeletons[0];
    return c;
}

std::vector<Mat4f> matrices_of(const Pose3D& pose, const Skeleton3D& sk) {
    std::vector<Mat4f> m;
    compute_joint_matrices(sk, pose, m);
    return m;
}

std::vector<Mat4f> collected_matrices(WorldRenderState3D& state) {
    Scene3D scene;
    state.propagate_transforms();
    state.collect(scene);
    return {scene.joint_matrices().begin(), scene.joint_matrices().end()};
}

u64 hash_bytes(u64 h, const void* data, std::size_t bytes) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < bytes; ++i) {
        h = (h ^ p[i]) * 1099511628211ull;
    }
    return h;
}

// An entity with Animator3D plays its clip by name: the joint matrices at every step are those of the CPU maths (the
// clip sampled at the accumulated time, at the speed given), the draw is the model in that pose with the entity's id,
// and two runs agree to the byte.
void test_ecs_animator_clip() {
    const Character hero = make_character();
    const float dt = 1.0f / 60.0f;
    const AnimClip3D& walk = *hero.clip("walk");
    const auto run = [&](std::vector<u64>* hashes, bool check) {
        kin::EcsWorld world;
        WorldRenderState3D state{world};
        auto& w = world.raw();
        const flecs::entity e = w.entity()
                                    .set(Transform3D{.position = {1, 2, 3}})
                                    .set(SkinnedMeshRenderer3D{.model = hero.model})
                                    .set(Animator3D{.clip = "walk", .speed = 1.5f});
        float t = 0.0f;
        u64 h = 1469598103934665603ull;
        for (int step = 0; step < 90; ++step) {
            state.animate(dt);
            t = clip_time(walk, t + dt * 1.5f, true);
            std::vector<Mat4f> got;
            {
                Scene3D scene;
                state.propagate_transforms();
                state.collect(scene);
                assert(scene.draws().size() == 1 && scene.draws()[0].skinned && scene.instances().size() == 1);
                assert(scene.instances()[0].id == static_cast<u32>(e.id() & 0xffffffffu) &&
                       scene.instances()[0].id != 0);
                const Vec3f at{scene.instances()[0].transform.m[12], scene.instances()[0].transform.m[13],
                               scene.instances()[0].transform.m[14]};
                assert(close(at, {1, 2, 3}));
                got.assign(scene.joint_matrices().begin(), scene.joint_matrices().end());
            }
            if (check) {
                Pose3D pose;
                sample_clip(walk, *hero.skeleton, t, true, pose);
                assert(max_difference(got, matrices_of(pose, *hero.skeleton)) < 1e-5f);
            }
            h = hash_bytes(h, got.data(), got.size() * sizeof(Mat4f));
            if (hashes) {
                hashes->push_back(h);
            }
        }
        // Nothing dangling: the entity gone, the model is held by the test alone, and a collect after it is fine.
        assert(hero.model.use_count() >= 2);
        e.destruct();
        Scene3D scene;
        state.propagate_transforms();
        state.collect(scene);
        assert(scene.draws().empty());
    };
    std::vector<u64> first, second;
    run(&first, true);
    run(&second, false);
    assert(first == second);
    assert(hero.model.use_count() == 1);
    // Not playing: held.
    kin::EcsWorld world;
    WorldRenderState3D state{world};
    world.raw()
        .entity()
        .set(Transform3D{})
        .set(SkinnedMeshRenderer3D{.model = hero.model})
        .set(Animator3D{.clip = "walk"});
    state.animate(0.25f);
    const std::vector<Mat4f> at_quarter = collected_matrices(state);
    world.raw().query<Animator3D>().each([](Animator3D& a) { a.playing = false; });
    state.animate(0.25f);
    assert(max_difference(collected_matrices(state), at_quarter) == 0.0f);
    // Before its first animate(): the rest pose, drawn.
    kin::EcsWorld fresh;
    WorldRenderState3D fresh_state{fresh};
    fresh.raw()
        .entity()
        .set(Transform3D{})
        .set(SkinnedMeshRenderer3D{.model = hero.model})
        .set(Animator3D{.clip = "walk"});
    assert(max_difference(collected_matrices(fresh_state), matrices_of(hero.skeleton->rest_pose(), *hero.skeleton)) <
           1e-6f);
    // A renderer with no animator is drawn at rest, and an invisible one is not drawn.
    kin::EcsWorld bare;
    WorldRenderState3D bare_state{bare};
    bare.raw().entity().set(Transform3D{}).set(SkinnedMeshRenderer3D{.model = hero.model});
    bare.raw().entity().set(Transform3D{}).set(SkinnedMeshRenderer3D{.model = hero.model, .visible = false});
    assert(collected_matrices(bare_state).size() == 3);
}

// A state machine (kin/anim's) drives the animator: parameters choose the state, a transition crossfades over the
// length the animator has for it, a state with a blend space plays the 1D blend by its parameter, a trigger is consumed
// by the transition that takes it. The pose in the middle of a fade is the blend of the two pictures at its progress,
// to float rounding, as the CPU maths gives it.
void test_ecs_animator_state_machine() {
    const Character hero = make_character();
    const Skeleton3D& sk = *hero.skeleton;
    const float dt = 1.0f / 60.0f;
    auto machine = std::make_shared<AnimationStateMachine>();
    machine->initial = "idle";
    machine->states = {{"idle", "idle"}, {"locomotion", "walk"}, {"jump", "jump"}};
    machine->transitions = {
        {"", "jump", {{.param = "jump", .op = CondOp::TriggerSet}}},
        {"idle", "locomotion", {{.param = "speed", .op = CondOp::Greater, .threshold = 0.1f}}},
        {"locomotion", "idle", {{.param = "speed", .op = CondOp::Less, .threshold = 0.05f}}},
        {"jump", "idle", {{.param = "grounded", .op = CondOp::IsTrue}}},
    };
    Animator3D animator;
    animator.machine = machine;
    animator.fade_seconds = 0.3f;
    animator.fades = {{"idle", "locomotion", 0.25f}, {"", "jump", 0.1f}};
    animator.blend_spaces["locomotion"] = {"speed", {{"walk", 1.5f}, {"run", 4.5f}}};
    animator.params.set_bool("grounded", true);
    kin::EcsWorld world;
    WorldRenderState3D state{world};
    world.raw()
        .entity()
        .set(Transform3D{})
        .set(SkinnedMeshRenderer3D{.model = hero.model})
        .set(static_cast<const Animator3D&>(animator));
    const auto animator_of = [&]() -> Animator3D& {
        static thread_local Animator3D* found = nullptr;
        world.raw().query<Animator3D>().each([&](Animator3D& a) { found = &a; });
        return *found;
    };
    const AnimClip3D& idle = *hero.clip("idle");
    const AnimClip3D& walk = *hero.clip("walk");
    const AnimClip3D& run = *hero.clip("run");
    const AnimClip3D& jump = *hero.clip("jump");
    // Idle for 10 steps: the idle clip, no fade.
    float idle_t = 0.0f;
    for (int i = 0; i < 10; ++i) {
        state.animate(dt);
        idle_t = clip_time(idle, idle_t + dt, true);
    }
    {
        Pose3D p;
        sample_clip(idle, sk, idle_t, true, p);
        assert(max_difference(collected_matrices(state), matrices_of(p, sk)) < 1e-5f);
        assert(animator_of().state == "idle" && animator_of().player.fading_from() == nullptr);
    }
    // Speed 3.0: to locomotion at this step (the fade is 0.25 s); the blend is walk at 1.5 and run at 4.5, w = 0.5,
    // duration 0.8.
    animator_of().params.set_float("speed", 3.0f);
    float phase = 0.0f;
    for (int k = 1; k <= 8; ++k) {
        state.animate(dt);
        idle_t = clip_time(idle, idle_t + dt, true);
        phase += dt / 0.8f;
        phase -= std::floor(phase);
        assert(animator_of().state == "locomotion");
        const float alpha = std::min(1.0f, static_cast<float>(k) * dt / 0.25f);
        Pose3D from, a, b;
        sample_clip(idle, sk, idle_t, true, from);
        sample_clip(walk, sk, phase * 1.0f, true, a);
        sample_clip(run, sk, phase * 0.6f, true, b);
        const Pose3D expected = blend_poses(from, blend_poses(a, b, 0.5f), alpha);
        assert(max_difference(collected_matrices(state), matrices_of(expected, sk)) < 2e-4f);
    }
    // The fade is over at 0.25 s (15 steps): from then on it is the blend alone, following the parameter.
    for (int k = 9; k <= 20; ++k) {
        state.animate(dt);
        phase += dt / 0.8f;
        phase -= std::floor(phase);
    }
    assert(animator_of().player.fading_from() == nullptr);
    {
        Pose3D a, b;
        sample_clip(walk, sk, phase * 1.0f, true, a);
        sample_clip(run, sk, phase * 0.6f, true, b);
        assert(max_difference(collected_matrices(state), matrices_of(blend_poses(a, b, 0.5f), sk)) < 3e-4f);
    }
    // The speed sweeps: the pose follows the weight (a at 1.5 is walk alone, at 4.5 run alone).
    animator_of().params.set_float("speed", 1.5f);
    state.animate(0.0f);
    {
        Pose3D p;
        sample_clip(walk, sk, animator_of().player.time() * 1.0f, true, p);
        assert(max_difference(collected_matrices(state), matrices_of(p, sk)) < 1e-5f);
    }
    animator_of().params.set_float("speed", 4.5f);
    state.animate(0.0f);
    {
        Pose3D p;
        sample_clip(run, sk, animator_of().player.time() * 0.6f, true, p);
        assert(max_difference(collected_matrices(state), matrices_of(p, sk)) < 1e-5f);
    }
    // A trigger: to jump from anywhere at once (0.1 s fade), consumed; another transition does not fire in the same
    // step.
    animator_of().params.set_bool("grounded", false);
    animator_of().params.set_trigger("jump");
    state.animate(dt);
    assert(animator_of().state == "jump" && !animator_of().params.triggers.contains("jump"));
    assert(animator_of().player.fading_from() ==
           nullptr); // the fade is from a blend (fading_from names a plain clip, or none)
    assert(close(animator_of().player.fade_progress(), dt / 0.1f, 1e-4f));
    for (int i = 0; i < 8; ++i) {
        state.animate(dt); // 9 steps of 1/60 s: past the fade
    }
    assert(close(animator_of().player.fade_progress(), 1.0f));
    {
        Pose3D p;
        sample_clip(jump, sk, animator_of().player.time(), true, p);
        assert(max_difference(collected_matrices(state), matrices_of(p, sk)) < 1e-5f);
    }
    // Landing: grounded again, back to idle at the default fade (0.3 s).
    animator_of().params.set_bool("grounded", true);
    state.animate(dt);
    assert(animator_of().state == "idle" && close(animator_of().player.fade_progress(), dt / 0.3f, 1e-4f));
    // Two runs of the same inputs agree to the byte.
    const auto replay = [&] {
        kin::EcsWorld w;
        WorldRenderState3D s{w};
        w.raw()
            .entity()
            .set(Transform3D{})
            .set(SkinnedMeshRenderer3D{.model = hero.model})
            .set(static_cast<const Animator3D&>(animator));
        u64 h = 1469598103934665603ull;
        for (int step = 0; step < 200; ++step) {
            w.raw().query<Animator3D>().each([&](Animator3D& a) {
                a.params.set_float("speed", 0.2f + 4.5f * std::fabs(std::sin(static_cast<float>(step) * 0.05f)));
                a.params.set_bool("grounded", step % 70 > 20);
                if (step % 97 == 40) {
                    a.params.set_trigger("jump");
                }
            });
            s.animate(dt);
            const std::vector<Mat4f> m = collected_matrices(s);
            h = hash_bytes(h, m.data(), m.size() * sizeof(Mat4f));
        }
        return h;
    };
    assert(replay() == replay());
}

// Layers as data (a clip on a joint's subtree over the base, by weight), additive layers and root motion moved onto the
// entity.
void test_ecs_animator_layers_and_root_motion() {
    const Character hero = make_character();
    const Skeleton3D& sk = *hero.skeleton;
    const float dt = 1.0f / 60.0f;
    {
        Animator3D a;
        a.clip = "walk";
        a.layers = {{.clip = "wave", .mask_joint = "middle", .include_root = true, .weight = 0.75f}};
        kin::EcsWorld world;
        WorldRenderState3D state{world};
        world.raw().entity().set(Transform3D{}).set(SkinnedMeshRenderer3D{.model = hero.model}).set(a);
        float t_base = 0.0f, t_layer = 0.0f;
        for (int i = 0; i < 20; ++i) {
            state.animate(dt);
            t_base = clip_time(*hero.clip("walk"), t_base + dt, true);
            t_layer = clip_time(*hero.clip("wave"), t_layer + dt, true);
        }
        Pose3D base, layer;
        sample_clip(*hero.clip("walk"), sk, t_base, true, base);
        sample_clip(*hero.clip("wave"), sk, t_layer, true, layer);
        blend_masked(base, layer, PoseMask3D::subtree(sk, sk.find("middle"), 1.0f, true), 0.75f);
        assert(max_difference(collected_matrices(state), matrices_of(base, sk)) < 1e-5f);
    }
    {
        // Root motion: 2 m a second along the entity's own x; the entity turned a quarter about Y goes along -z. The
        // root joint stays at rest.
        Animator3D a;
        a.clip = "root_walk";
        a.apply_root_motion = true;
        kin::EcsWorld world;
        WorldRenderState3D state{world};
        const flecs::entity e =
            world.raw()
                .entity()
                .set(Transform3D{.position = {5, 0, 0}, .rotation = Quatf::axis_angle({0, 1, 0}, 90.0f)})
                .set(SkinnedMeshRenderer3D{.model = hero.model})
                .set(a);
        for (int i = 0; i < 30; ++i) {
            state.animate(dt);
        }
        const Transform3D* t = e.get<Transform3D>();
        assert(close(t->position, {5.0f, 0.0f, -1.0f}, 1e-3f));
        Pose3D p;
        p = e.get<Animator3D>()->player.pose();
        assert(close(p.joints[0].translation.x, 0.0f, 1e-6f)); // held at rest in x
    }
}

// --- inverse kinematics (kin/render3d/ik3d.hpp), CPU only
// --------------------------------------------------------------------

namespace biped = skin_fixtures::biped;

// A skeleton, its local pose and the world matrices the solvers keep in step.
struct Rig {
    Skeleton3D sk;
    Pose3D pose;
    std::vector<Mat4f> world;

    explicit Rig(Skeleton3D s = skin_fixtures::make_biped()) : sk(std::move(s)) {
        pose = sk.rest_pose();
        refresh();
    }
    void refresh() { compute_world_matrices(sk, pose, world); }
    Vec3f at(int j) const {
        return {world[static_cast<std::size_t>(j)].m[12], world[static_cast<std::size_t>(j)].m[13],
                world[static_cast<std::size_t>(j)].m[14]};
    }
    float bone(int a, int b) const { return length(at(a) - at(b)); }
    Quatf rotation(int j) const { return rotation_of(world[static_cast<std::size_t>(j)]); }
};

float angle_degrees(Quatf a, Quatf b) {
    const float d = std::min(1.0f, std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w));
    return 2.0f * std::acos(d) * 180.0f / 3.14159265f;
}
float angle_degrees(Vec3f a, Vec3f b) {
    return std::atan2(length(cross(a, b)), dot(a, b)) * 180.0f / 3.14159265f;
}

const TwoBoneChain3D left_arm{biped::shoulder_l, biped::elbow_l, biped::hand_l};
const TwoBoneChain3D left_leg{biped::hip_l, biped::knee_l, biped::ankle_l};

// The arm bent a little and turned, so that it has a bend plane and nothing is aligned with an axis.
Rig bent_arm_rig() {
    Rig rig;
    rig.pose.joints[biped::shoulder_l].rotation =
        Quatf::axis_angle({0, 0, 1}, 12.0f) * Quatf::axis_angle({0, 1, 0}, 20.0f);
    rig.pose.joints[biped::elbow_l].rotation = Quatf::axis_angle({0.2f, 1, 0.1f}, -55.0f);
    rig.refresh();
    return rig;
}

// A deterministic stream of numbers in [-1, 1].
struct Lcg {
    unsigned s = 12345u;
    float next() {
        s = s * 1664525u + 1013904223u;
        return static_cast<float>((s >> 8) & 0xffff) / 32767.5f - 1.0f;
    }
    Vec3f direction() {
        for (;;) {
            const Vec3f v{next(), next(), next()};
            const float l = length(v);
            if (l > 0.2f && l <= 1.0f) {
                return v / l;
            }
        }
    }
};

void check_only_chain_changed(const Pose3D& before, const Pose3D& after, std::initializer_list<int> rotated) {
    for (std::size_t i = 0; i < before.joints.size(); ++i) {
        assert(before.joints[i].translation == after.joints[i].translation);
        assert(before.joints[i].scale == after.joints[i].scale);
        bool may = false;
        for (int r : rotated) {
            may = may || static_cast<std::size_t>(r) == i;
        }
        if (!may) {
            assert(before.joints[i].rotation == after.joints[i].rotation);
        }
    }
}

// Reachable targets in every direction: the hand arrives, the bones keep their lengths, and nothing but the two
// rotations changed.
void test_ik_two_bone_reach() {
    Lcg rng;
    for (const TwoBoneChain3D chain : {left_arm, left_leg}) {
        const Rig base = bent_arm_rig();
        const float l1 = base.bone(chain.root, chain.mid), l2 = base.bone(chain.mid, chain.end);
        for (int i = 0; i < 400; ++i) {
            Rig rig = base;
            const Pose3D before = rig.pose;
            const Vec3f root = rig.at(chain.root);
            const float d = (0.12f + 0.86f * (rng.next() * 0.5f + 0.5f)) * (l1 + l2);
            const Vec3f target = root + rng.direction() * d;
            TwoBoneOptions3D o;
            o.pole_mode = PoleMode3D::Direction;
            o.pole = rng.direction();
            const TwoBoneResult3D r = solve_two_bone(rig.sk, rig.pose, rig.world, chain, target, o);
            assert(r.solved && r.reached);
            assert(close(rig.at(chain.end), target, 2e-4f));
            assert(close(rig.bone(chain.root, chain.mid), l1, 1e-5f) &&
                   close(rig.bone(chain.mid, chain.end), l2, 1e-5f));
            check_only_chain_changed(before, rig.pose, {chain.root, chain.mid});
            // The matrices kept in step are the pose's own.
            std::vector<Mat4f> again;
            compute_world_matrices(rig.sk, rig.pose, again);
            assert(max_difference(again, rig.world) < 1e-5f);
            assert(close(length_of(rig.pose.joints[static_cast<std::size_t>(chain.mid)].rotation), 1.0f, 1e-5f));
        }
    }
}

// Out of reach: the arm is straight at the target; with a stretch it lengthens softly toward the limit and never
// beyond; a minimum bend keeps the elbow from locking.
void test_ik_two_bone_out_of_reach() {
    const Rig base = bent_arm_rig();
    const float reach = base.bone(biped::shoulder_l, biped::elbow_l) + base.bone(biped::elbow_l, biped::hand_l);
    const Vec3f root = base.at(biped::shoulder_l);
    const Vec3f dir = normalize({0.3f, 0.5f, -0.7f});
    {
        Rig rig = base;
        TwoBoneOptions3D o;
        o.pole_mode = PoleMode3D::Direction;
        o.pole = {0, 1, 0};
        const TwoBoneResult3D r = solve_two_bone(rig.sk, rig.pose, rig.world, left_arm, root + dir * 2.0f, o);
        assert(r.solved && !r.reached);
        assert(close(rig.at(biped::hand_l), root + dir * reach, 1e-4f));
        const Vec3f e = rig.at(biped::elbow_l) - root;
        assert(length(cross(e, dir)) < 1e-4f);
        assert(r.bend_degrees < 0.1f);
    }
    {
        // A stretch of 1.3: monotonic and continuous in the target's distance, tends to 1.3 and stays under it.
        TwoBoneOptions3D o;
        o.max_stretch = 1.3f;
        o.pole_mode = PoleMode3D::Direction;
        o.pole = {0, 1, 0};
        float last = 0.5f * reach;
        for (int i = 0; i <= 600; ++i) {
            const float d = 0.5f * reach + 0.004f * static_cast<float>(i) * reach;
            Rig rig = base;
            const TwoBoneResult3D r = solve_two_bone(rig.sk, rig.pose, rig.world, left_arm, root + dir * d, o);
            const float got = length(rig.at(biped::hand_l) - root);
            assert(got <= 1.3f * reach + 1e-4f && got >= last - 1e-5f && got - last < 0.004f * reach * 1.02f + 1e-5f);
            if (d <= reach) {
                assert(close(got, d, 2e-4f) && r.stretch < 1.0001f);
            } else {
                assert(r.stretch > 1.0f - 1e-6f && r.stretch <= 1.3f);
                assert(close(rig.bone(biped::shoulder_l, biped::elbow_l) + rig.bone(biped::elbow_l, biped::hand_l),
                             reach * r.stretch, 1e-4f));
                // Out of reach, the hand is at the end of the stretched chain, on the line to the target.
                assert(close(got, reach * r.stretch, 2e-4f) &&
                       length(cross(rig.at(biped::hand_l) - root, dir)) < 2e-4f);
            }
            last = got;
        }
        assert(last > 1.25f * reach);
    }
    {
        // A minimum bend of 10 degrees: the hand stops short of full reach, on the line, with that much bend.
        Rig rig = base;
        TwoBoneOptions3D o;
        o.min_bend_degrees = 10.0f;
        o.pole_mode = PoleMode3D::Direction;
        o.pole = {0, 1, 0};
        const TwoBoneResult3D r = solve_two_bone(rig.sk, rig.pose, rig.world, left_arm, root + dir * 2.0f, o);
        assert(close(r.bend_degrees, 10.0f, 1e-2f));
        const Vec3f h = rig.at(biped::hand_l) - root;
        assert(length(cross(h, dir)) < 1e-4f && length(h) < reach && length(h) > 0.98f * reach);
    }
}

// The elbow goes to the pole's side; opposite poles mirror the bend across the line from the shoulder to the hand; a
// pole given as a point is the same as a direction to it.
void test_ik_two_bone_pole() {
    const Rig base = bent_arm_rig();
    const Vec3f root = base.at(biped::shoulder_l);
    const Vec3f target = root + Vec3f{0.35f, -0.2f, 0.4f};
    const auto solve = [&](Vec3f pole, PoleMode3D mode) {
        Rig rig = base;
        TwoBoneOptions3D o;
        o.pole_mode = mode;
        o.pole = pole;
        solve_two_bone(rig.sk, rig.pose, rig.world, left_arm, target, o);
        return rig;
    };
    const Rig up = solve({0, 1, 0}, PoleMode3D::Direction);
    const Rig down = solve({0, -1, 0}, PoleMode3D::Direction);
    assert(up.at(biped::elbow_l).y > root.y && down.at(biped::elbow_l).y < root.y);
    // Mirrored about the axis: the midpoint of the two elbows lies on the line root-target.
    const Vec3f axis = normalize(target - root);
    const Vec3f mid = (up.at(biped::elbow_l) + down.at(biped::elbow_l)) * 0.5f - root;
    assert(length(cross(mid, axis)) < 1e-4f);
    assert(close(length(up.at(biped::elbow_l) - root), length(down.at(biped::elbow_l) - root), 1e-5f));
    // Toward the front (+Z) and the back.
    assert(solve({0, 0, 1}, PoleMode3D::Direction).at(biped::elbow_l).z > root.z);
    assert(solve({0, 0, -1}, PoleMode3D::Direction).at(biped::elbow_l).z < root.z);
    // A point: seen from the shoulder.
    const Rig by_point = solve(root + Vec3f{0, 3, 0}, PoleMode3D::Position);
    assert(close(by_point.at(biped::elbow_l), up.at(biped::elbow_l), 1e-5f));
}

// Weight 0 leaves the pose bit for bit; 0.5 lies between; the end reaches at 1.
void test_ik_two_bone_weight() {
    const Rig base = bent_arm_rig();
    const Vec3f target = base.at(biped::shoulder_l) + Vec3f{0.1f, -0.5f, 0.45f};
    TwoBoneOptions3D o;
    o.pole_mode = PoleMode3D::Direction;
    o.pole = {0, 1, 0};
    Rig none = base;
    o.weight = 0.0f;
    assert(!solve_two_bone(none.sk, none.pose, none.world, left_arm, target, o).solved);
    assert(std::memcmp(none.pose.joints.data(), base.pose.joints.data(),
                       base.pose.joints.size() * sizeof(JointPose3D)) == 0);
    assert(std::memcmp(none.world.data(), base.world.data(), base.world.size() * sizeof(Mat4f)) == 0);
    Rig full = base, half = base;
    o.weight = 1.0f;
    solve_two_bone(full.sk, full.pose, full.world, left_arm, target, o);
    o.weight = 0.5f;
    solve_two_bone(half.sk, half.pose, half.world, left_arm, target, o);
    const float start = length(base.at(biped::hand_l) - target), h = length(half.at(biped::hand_l) - target);
    assert(h < start && h > 0.01f);
    // Each rotation is half way (as an angle) from the pose's to the solved one.
    for (int j : {biped::shoulder_l, biped::elbow_l}) {
        const float whole = angle_degrees(base.pose.joints[static_cast<std::size_t>(j)].rotation,
                                          full.pose.joints[static_cast<std::size_t>(j)].rotation);
        const float part = angle_degrees(base.pose.joints[static_cast<std::size_t>(j)].rotation,
                                         half.pose.joints[static_cast<std::size_t>(j)].rotation);
        assert(whole > 5.0f && close(part, whole * 0.5f, 0.05f));
    }
    // A weight over 1 is 1; tiny weights are tiny.
    Rig tiny = base;
    o.weight = 1e-4f;
    solve_two_bone(tiny.sk, tiny.pose, tiny.world, left_arm, target, o);
    assert(length(tiny.at(biped::hand_l) - base.at(biped::hand_l)) < 1e-3f);
}

// Solving a solved pose again changes nothing (to rounding), with a pole and without one.
void test_ik_two_bone_idempotent() {
    for (const bool with_pole : {true, false}) {
        Rig rig = bent_arm_rig();
        const Vec3f target = rig.at(biped::shoulder_l) + Vec3f{0.3f, 0.1f, 0.5f};
        TwoBoneOptions3D o;
        o.max_stretch = 1.2f;
        o.min_bend_degrees = 3.0f;
        if (with_pole) {
            o.pole_mode = PoleMode3D::Direction;
            o.pole = {0.2f, 1, -0.3f};
        }
        solve_two_bone(rig.sk, rig.pose, rig.world, left_arm, target, o);
        const Pose3D once = rig.pose;
        const std::vector<Mat4f> once_world = rig.world;
        solve_two_bone(rig.sk, rig.pose, rig.world, left_arm, target, o);
        assert(max_difference(once_world, rig.world) < 2e-5f);
        for (std::size_t i = 0; i < once.joints.size(); ++i) {
            assert(close(once.joints[i].rotation, rig.pose.joints[i].rotation, 1e-5f));
        }
        // Out of reach too.
        Rig far = bent_arm_rig();
        const Vec3f faraway = far.at(biped::shoulder_l) + Vec3f{1.5f, 0.5f, 0.2f};
        solve_two_bone(far.sk, far.pose, far.world, left_arm, faraway, o);
        const std::vector<Mat4f> far_once = far.world;
        solve_two_bone(far.sk, far.pose, far.world, left_arm, faraway, o);
        assert(max_difference(far_once, far.world) < 2e-5f);
    }
}

// The largest change of the chain's pose (the angle of either rotation, and the elbow's direction from the shoulder)
// between two consecutive solves.
struct PoseStep {
    float root_degrees = 0.0f, mid_degrees = 0.0f, elbow_degrees = 0.0f;
    float worst() const { return std::max({root_degrees, mid_degrees, elbow_degrees}); }
};
PoseStep pose_step(const Rig& a, const Rig& b, const TwoBoneChain3D& c) {
    PoseStep s;
    s.root_degrees = angle_degrees(a.pose.joints[static_cast<std::size_t>(c.root)].rotation,
                                   b.pose.joints[static_cast<std::size_t>(c.root)].rotation);
    s.mid_degrees = angle_degrees(a.pose.joints[static_cast<std::size_t>(c.mid)].rotation,
                                  b.pose.joints[static_cast<std::size_t>(c.mid)].rotation);
    s.elbow_degrees = angle_degrees(a.at(c.mid) - a.at(c.root), b.at(c.mid) - b.at(c.root));
    return s;
}

// A target sweeping through the cases where a solver flips: through the root, along the chain's axis, round and through
// the pole. The change from one step to the next stays small (proportional to the step), the pose never jumps.
void test_ik_two_bone_continuity() {
    // Steps of `steps` over u in [0, 1]; `jumps_allowed` is how many steps may turn a joint more than
    // `max_step_degrees` (the twist at a fold's very centre: 180 degrees with the joints in the same places); the elbow
    // and hand never move more than `max_move` in one step.
    const auto sweep = [&](const char* what, Rig start, const TwoBoneChain3D& chain, TwoBoneOptions3D o, int steps,
                           auto target_at, float max_step_degrees, int jumps_allowed = 0, float max_move = 0.02f) {
        Rig previous;
        float worst = 0.0f, worst_move = 0.0f;
        int jumps = 0;
        for (int i = 0; i <= steps; ++i) {
            Rig rig = start;
            const Vec3f target = target_at(static_cast<float>(i) / static_cast<float>(steps));
            const TwoBoneResult3D r = solve_two_bone(rig.sk, rig.pose, rig.world, chain, target, o);
            assert(r.solved);
            for (const Mat4f& m : rig.world) {
                for (float v : m.m) {
                    assert(std::isfinite(v));
                }
            }
            if (i > 0) {
                const PoseStep st = pose_step(previous, rig, chain);
                if (st.worst() > max_step_degrees) {
                    ++jumps;
                } else {
                    worst = std::max(worst, st.worst());
                }
                worst_move = std::max({worst_move, length(rig.at(chain.mid) - previous.at(chain.mid)),
                                       length(rig.at(chain.end) - previous.at(chain.end))});
            }
            previous = std::move(rig);
        }
        std::cout << "  ik continuity (" << what << "): worst step " << worst << " degrees, " << jumps
                  << " jump(s) over " << max_step_degrees << ", elbow/hand move at most " << worst_move << " a step"
                  << std::endl;
        assert(jumps <= jumps_allowed && worst_move <= max_move);
    };
    const Rig base = bent_arm_rig();
    const Vec3f root = base.at(biped::shoulder_l);
    TwoBoneOptions3D pole;
    pole.pole_mode = PoleMode3D::Direction;
    pole.pole = {0, 1, 0};
    // Through the root, equal bones (a fold flat on itself is the only pose there): continuous. (Not sampled at the
    // root's very point, where every direction is as good as another and the solver keeps the chain's own.)
    Rig equal = base;
    equal.pose.joints[biped::hand_l].translation = {0.5f, 0, 0};
    equal.refresh();
    const Vec3f line = normalize({0.4f, 0.1f, 0.5f});
    sweep(
        "through the root, equal bones", equal, left_arm, pole, 1999,
        [&](float u) { return root + line * (u * 1.6f - 0.8f); }, 6.0f, 1);
    // The same with no pole at all (the pose's own bend plane).
    sweep(
        "through the root, no pole", equal, left_arm, {}, 1999,
        [&](float u) { return root + line * (u * 1.6f - 0.8f); }, 6.0f, 1);
    // Along the arm's own axis, to well out of reach, straight through the full extension (the elbow's angle goes with
    // the square root of the distance short of it, so a step of 0.1% of the reach turns it a few degrees: that is the
    // bound).
    const Vec3f axis = normalize(base.at(biped::hand_l) - root);
    sweep(
        "along the axis, past full reach", base, left_arm, pole, 2000,
        [&](float u) { return root + axis * (u * 2.1f + 0.1f); }, 6.0f);
    // Through the root of an arm whose bones differ (a hole of 5 cm round the shoulder where the hand cannot be): the
    // chain turns end over end within those 5 cm, steeply (a step of 0.8 mm there is about 1 degree per 0.01 of the
    // hole's radius) but without a jump.
    sweep(
        "through the root, unequal bones", base, left_arm, pole, 1999,
        [&](float u) { return root + line * (u * 1.6f - 0.8f); }, 15.0f, 0, 0.1f);
    sweep(
        "along the axis, past full reach, stretch", base, left_arm,
        [&] {
            TwoBoneOptions3D o = pole;
            o.max_stretch = 1.25f;
            return o;
        }(),
        2000, [&](float u) { return root + axis * (u * 2.1f + 0.1f); }, 6.0f);
    // A straight chain (the rest pose: no bend plane of its own) with no pole: still finite and continuous.
    Rig straight;
    sweep(
        "straight chain, no pole", straight, left_arm, {}, 2000,
        [&](float u) {
            return root + Vec3f{0.3f + 0.4f * std::cos(u * 6.2831853f), 0.4f * std::sin(u * 6.2831853f), 0.2f};
        },
        4.0f);
    // Round the pole: the target on a cone about the pole's direction, wide and narrow.
    for (const float half_angle : {40.0f, 8.0f, 2.0f}) {
        const float c = std::cos(half_angle * 3.14159265f / 180.0f), s = std::sin(half_angle * 3.14159265f / 180.0f);
        sweep(
            "round the pole", base, left_arm, pole, 3000,
            [&](float u) { return root + Vec3f{s * std::cos(u * 6.2831853f), c, s * std::sin(u * 6.2831853f)} * 0.7f; },
            3.0f);
    }
    // Through the pole's axis, straight across it. The elbow has to swing to the other side; the pull toward the pose's
    // own bend makes it a turn over a few degrees of the target's travel, not a jump.
    for (const Vec3f across : {Vec3f{1, 0, 0}, Vec3f{0.6f, 0, 0.8f}, Vec3f{0, 0, 1}}) {
        sweep(
            "through the pole's axis", base, left_arm, pole, 4000,
            [&](float u) { return root + Vec3f{0, 0.7f, 0} + across * (u * 0.6f - 0.3f); }, 12.0f);
    }
}

// A scaled, turned model and a scaled joint above the chain: the maths is done in the space of the root's parent, so
// the hand still arrives and the bones keep the lengths they are drawn at there.
void test_ik_two_bone_scaled_and_turned() {
    Rig rig = bent_arm_rig();
    rig.sk.root_transform =
        Mat4f::trs({1.0f, 2.0f, -3.0f}, Quatf::axis_angle({0.3f, 1, 0.2f}, 70.0f), {1.5f, 0.6f, 2.0f});
    rig.pose.joints[biped::chest].scale = {1.3f, 0.8f, 1.1f};
    rig.pose.joints[biped::spine].rotation = Quatf::axis_angle({0, 0, 1}, 15.0f);
    rig.refresh();
    const Mat4f parent = rig.world[biped::chest];
    const Mat4f inverse = parent.inverse();
    const auto lengths_in_parent = [&](const Rig& r) {
        const Vec3f a = inverse.apply(r.at(biped::shoulder_l)), b = inverse.apply(r.at(biped::elbow_l)),
                    c = inverse.apply(r.at(biped::hand_l));
        return std::array<float, 2>{length(b - a), length(c - b)};
    };
    const auto before = lengths_in_parent(rig);
    Lcg rng;
    for (int i = 0; i < 200; ++i) {
        Rig r = rig;
        // A target inside the reach, as the parent's space sees it.
        const Vec3f local = inverse.apply(r.at(biped::shoulder_l)) +
                            rng.direction() * (0.1f + 0.8f * (rng.next() * 0.5f + 0.5f)) * (before[0] + before[1]);
        const Vec3f target = parent.apply(local);
        TwoBoneOptions3D o;
        o.pole_mode = PoleMode3D::Direction;
        o.pole = rng.direction();
        const TwoBoneResult3D res = solve_two_bone(r.sk, r.pose, r.world, left_arm, target, o);
        assert(res.solved && res.reached);
        assert(close(r.at(biped::hand_l), target, 1e-3f));
        const auto after = lengths_in_parent(r);
        assert(close(after[0], before[0], 2e-5f) && close(after[1], before[1], 2e-5f));
        std::vector<Mat4f> again;
        compute_world_matrices(r.sk, r.pose, again);
        assert(max_difference(again, r.world) < 1e-4f);
    }
}

// The end joint is turned to a given orientation (a hand to a hold's surface), by `rotation_weight`; without it the
// hand only rides on the forearm.
void test_ik_two_bone_orientation() {
    const Rig base = bent_arm_rig();
    const Vec3f target = base.at(biped::shoulder_l) + Vec3f{0.3f, -0.2f, 0.5f};
    const Quatf want = Quatf::axis_angle({0.4f, 1, -0.3f}, 110.0f);
    TwoBoneOptions3D o;
    o.pole_mode = PoleMode3D::Direction;
    o.pole = {0, 1, 0};
    Rig plain = base;
    solve_two_bone(plain.sk, plain.pose, plain.world, left_arm, target, o);
    // Unrotated: the hand's local rotation is what the pose had.
    assert(plain.pose.joints[biped::hand_l].rotation == base.pose.joints[biped::hand_l].rotation);
    o.match_rotation = true;
    o.end_rotation = want;
    Rig matched = base;
    const TwoBoneResult3D r = solve_two_bone(matched.sk, matched.pose, matched.world, left_arm, target, o);
    assert(r.reached && close(matched.at(biped::hand_l), target, 1e-4f));
    assert(angle_degrees(matched.rotation(biped::hand_l), want) < 0.05f);
    // Half the rotation weight: half way (as an angle) from the plain hand's orientation.
    o.rotation_weight = 0.5f;
    Rig half = base;
    solve_two_bone(half.sk, half.pose, half.world, left_arm, target, o);
    const float whole = angle_degrees(plain.rotation(biped::hand_l), want);
    assert(close(angle_degrees(half.rotation(biped::hand_l), plain.rotation(biped::hand_l)), whole * 0.5f, 0.1f));
    // The position is the same whatever the orientation does.
    assert(close(half.at(biped::hand_l), target, 1e-4f));
}

// A look-at turns the head's aim axis to the target; the weights share the turn, limits hold, the weight scales it, a
// target going round behind never flips the head, and smoothing approaches.
void test_ik_large_skeleton() {
    Skeleton3D sk;
    sk.parents.assign(300, 0);
    sk.parents[0] = -1;
    sk.rest.resize(300);
    sk.inverse_bind.resize(300);
    sk.rest.back().translation = {0, 0, 1};
    Pose3D pose = sk.rest_pose();
    std::vector<Mat4f> world(300);
    compute_world_matrices(sk, pose, world);
    const std::array<LookAtJoint3D, 1> chain{{{.joint = 0, .weight = 1}}};
    assert(solve_look_at(sk, pose, world, chain, {1, 0, 1}).solved);
    const Vec3f want = normalize(Vec3f{1, 0, 1});
    assert(close(world.back().apply({0, 0, 0}), want, 1e-4f));
}

void test_ik_look_at() {
    const std::vector<LookAtJoint3D> chain = {{.joint = biped::spine, .weight = 0.2f},
                                              {.joint = biped::neck, .weight = 0.3f},
                                              {.joint = biped::head, .weight = 0.5f}};
    const Rig base;
    const Vec3f head = base.at(biped::head);
    const auto aim_of = [](const Rig& r) { return normalize(r.world[biped::head].apply_vector({0, 0, 1})); };
    const auto yaw_of = [](Vec3f d) { return std::atan2(d.x, d.z) * 180.0f / 3.14159265f; };
    const auto pitch_of = [](Vec3f d) { return std::asin(std::clamp(d.y, -1.0f, 1.0f)) * 180.0f / 3.14159265f; };

    // Unlimited: the aim arrives, and the shares are the weights'.
    {
        Rig r = base;
        const Vec3f target = head + normalize(Vec3f{3, 0.8f, 2}) * 4.0f;
        const LookAtResult3D res = solve_look_at(r.sk, r.pose, r.world, chain, target);
        assert(res.solved);
        assert(angle_degrees(aim_of(r), normalize(target - head)) < 0.05f);
        // A pure yaw (target level with the eyes): the three joints turned 20%, 30%, 50% of it.
        Rig level = base;
        const Vec3f level_target = head + normalize(Vec3f{1, 0, 1}) * 4.0f;
        solve_look_at(level.sk, level.pose, level.world, chain, level_target);
        const float total = angle_degrees(Quatf{}, level.pose.joints[biped::spine].rotation) +
                            angle_degrees(Quatf{}, level.pose.joints[biped::neck].rotation) +
                            angle_degrees(Quatf{}, level.pose.joints[biped::head].rotation);
        assert(close(total, 45.0f, 0.05f));
        assert(close(angle_degrees(Quatf{}, level.pose.joints[biped::spine].rotation), 9.0f, 0.05f));
        assert(close(angle_degrees(Quatf{}, level.pose.joints[biped::neck].rotation), 13.5f, 0.05f));
        assert(close(angle_degrees(Quatf{}, level.pose.joints[biped::head].rotation), 22.5f, 0.05f));
        // Only the three rotated; nothing moved in place.
        for (std::size_t i = 0; i < base.pose.joints.size(); ++i) {
            assert(level.pose.joints[i].translation == base.pose.joints[i].translation);
        }
    }
    // Limits: a head with 25 degrees of yaw, a neck 15, a spine 5 turns 45 at most, and each stays inside its own.
    {
        const std::vector<LookAtJoint3D> limited = {{.joint = biped::spine,
                                                     .weight = 0.2f,
                                                     .max_yaw_degrees = 5.0f,
                                                     .max_pitch_up_degrees = 5.0f,
                                                     .max_pitch_down_degrees = 5.0f},
                                                    {.joint = biped::neck,
                                                     .weight = 0.3f,
                                                     .max_yaw_degrees = 15.0f,
                                                     .max_pitch_up_degrees = 15.0f,
                                                     .max_pitch_down_degrees = 15.0f},
                                                    {.joint = biped::head,
                                                     .weight = 0.5f,
                                                     .max_yaw_degrees = 25.0f,
                                                     .max_pitch_up_degrees = 25.0f,
                                                     .max_pitch_down_degrees = 25.0f}};
        Rig r = base;
        LookAtOptions3D o;
        o.release_degrees = 0.0f;
        solve_look_at(r.sk, r.pose, r.world, limited, head + Vec3f{4, 0, 0}, o); // 90 degrees to the side
        assert(close(angle_degrees(Quatf{}, r.pose.joints[biped::spine].rotation), 5.0f, 0.05f));
        assert(close(angle_degrees(Quatf{}, r.pose.joints[biped::neck].rotation), 15.0f, 0.05f));
        assert(close(angle_degrees(Quatf{}, r.pose.joints[biped::head].rotation), 25.0f, 0.05f));
        assert(close(yaw_of(aim_of(r)), 45.0f, 0.1f));
        // Pitch: 10 degrees up and 10 down of a chain whose joints take 5, 15 and 25 are all inside the limits:
        // reached.
        Rig up = base;
        solve_look_at(up.sk, up.pose, up.world, limited,
                      head + Vec3f{0, std::tan(10.0f * 3.14159265f / 180.0f) * 4, 4});
        assert(close(pitch_of(aim_of(up)), 10.0f, 0.05f));
        // A joint with different limits up and down.
        const std::vector<LookAtJoint3D> asym = {{.joint = biped::head,
                                                  .weight = 1.0f,
                                                  .max_yaw_degrees = 60.0f,
                                                  .max_pitch_up_degrees = 10.0f,
                                                  .max_pitch_down_degrees = 40.0f}};
        Rig u = base, d = base;
        solve_look_at(u.sk, u.pose, u.world, asym, head + Vec3f{0, 4, 1}, o);
        solve_look_at(d.sk, d.pose, d.world, asym, head + Vec3f{0, -4, 1}, o);
        assert(close(pitch_of(aim_of(u)), 10.0f, 0.1f) && close(pitch_of(aim_of(d)), -40.0f, 0.1f));
    }
    // Weight: 0 is bit for bit, 0.5 half the turn; the eye offset moves the point looking.
    {
        Rig none = base, half = base, full = base;
        LookAtOptions3D o;
        o.weight = 0.0f;
        const Vec3f target = head + Vec3f{3, 1, 3};
        assert(!solve_look_at(none.sk, none.pose, none.world, chain, target, o).solved);
        assert(std::memcmp(none.pose.joints.data(), base.pose.joints.data(),
                           base.pose.joints.size() * sizeof(JointPose3D)) == 0);
        solve_look_at(full.sk, full.pose, full.world, chain, target);
        o.weight = 0.5f;
        solve_look_at(half.sk, half.pose, half.world, chain, target, o);
        assert(
            close(angle_degrees(aim_of(base), aim_of(half)), angle_degrees(aim_of(base), aim_of(full)) * 0.5f, 0.1f));
    }
    // Round behind: a target circling the head at eye height. The aim follows to the limit, is eased back to forward as
    // the target goes behind, and comes round the other side without a jump: no step of the aim bigger than a little
    // over the target's own.
    {
        const std::vector<LookAtJoint3D> neck = {{.joint = biped::neck,
                                                  .weight = 0.4f,
                                                  .max_yaw_degrees = 50.0f,
                                                  .max_pitch_up_degrees = 30.0f,
                                                  .max_pitch_down_degrees = 30.0f},
                                                 {.joint = biped::head,
                                                  .weight = 0.6f,
                                                  .max_yaw_degrees = 70.0f,
                                                  .max_pitch_up_degrees = 40.0f,
                                                  .max_pitch_down_degrees = 40.0f}};
        Vec3f previous{};
        float worst = 0.0f, behind = 360.0f;
        const int steps = 720;
        for (int i = 0; i <= steps; ++i) {
            const float a = 6.2831853f * static_cast<float>(i) / static_cast<float>(steps);
            Rig r = base;
            solve_look_at(r.sk, r.pose, r.world, neck, head + Vec3f{std::sin(a), 0.1f, std::cos(a)} * 3.0f);
            const Vec3f aim = aim_of(r);
            for (float v : r.world[biped::head].m) {
                assert(std::isfinite(v));
            }
            if (i > 0) {
                worst = std::max(worst, angle_degrees(aim, previous));
            }
            if (std::fabs(static_cast<float>(i) - steps * 0.5f) < 0.5f) {
                behind = angle_degrees(aim, aim_of(base));
            }
            previous = aim;
        }
        std::cout << "  ik look-at round behind: worst step of the aim " << worst
                  << " degrees (the target moves 0.5), at the back " << behind << " from forward" << std::endl;
        assert(worst < 3.0f && behind < 1.0f);
    }
    // Smoothing: toward a fixed target the aim approaches monotonically and arrives; the speed cap holds; dt 0 holds.
    {
        LookAtOptions3D o;
        o.smooth_seconds = 0.2f;
        LookAtState3D state;
        const Vec3f target = head + Vec3f{3, 0, 3};
        float last = 360.0f;
        for (int i = 0; i < 180; ++i) {
            Rig r = base;
            solve_look_at(r.sk, r.pose, r.world, chain, target, o, &state, 1.0f / 60.0f);
            const float err = angle_degrees(aim_of(r), normalize(target - head));
            assert(err <= last + 1e-3f);
            last = err;
        }
        assert(last < 0.5f);
        // The time constant: after one step the aim has hardly moved, after 0.2 s (the constant) a fraction 1 / e of
        // the way is left.
        LookAtState3D timed;
        float start = 0.0f, after_one = 0.0f, after_constant = 0.0f;
        for (int i = 0; i <= 12; ++i) {
            Rig r = base;
            solve_look_at(r.sk, r.pose, r.world, chain, target, o, &timed, i == 0 ? 0.0f : 1.0f / 60.0f);
            const float err = angle_degrees(aim_of(r), normalize(target - head));
            start = i == 0 ? err : start;
            after_one = i == 1 ? err : after_one;
            after_constant = err;
        }
        assert(after_one > 0.9f * start && after_one < start);
        assert(close(after_constant / start, std::exp(-1.0f), 0.02f));
        LookAtOptions3D capped;
        capped.max_speed_degrees = 90.0f;
        LookAtState3D s2;
        Rig first = base;
        solve_look_at(first.sk, first.pose, first.world, chain, target, capped, &s2, 0.0f);
        assert(angle_degrees(aim_of(first), aim_of(base)) < 1e-3f); // the first call starts from the head's own aim
        Rig second = base;
        solve_look_at(second.sk, second.pose, second.world, chain, target, capped, &s2, 0.1f);
        assert(close(angle_degrees(aim_of(second), aim_of(base)), 9.0f, 0.05f));
        Rig held = base;
        solve_look_at(held.sk, held.pose, held.world, chain, target, capped, &s2, 0.0f);
        assert(angle_degrees(aim_of(held), aim_of(second)) < 1e-3f);
    }
}

// A fake ground for the foot placer: a height by x (and a slope in z), as a query along a segment.
struct FakeGround {
    std::function<float(float x, float z)> height;
    Vec3f normal{0, 1, 0};
    int casts = 0;

    GroundQuery3D query() {
        return [this](Vec3f from, Vec3f to) -> std::optional<GroundHit3D> {
            ++casts;
            // March down the segment to the first sample under the surface, then bisect.
            const auto below = [&](float t) {
                const Vec3f p = from + (to - from) * t;
                return p.y <= height(p.x, p.z);
            };
            if (below(0.0f)) {
                return GroundHit3D{from, normal};
            }
            if (!below(1.0f)) {
                return std::nullopt;
            }
            float lo = 0.0f, hi = 1.0f;
            for (int i = 0; i < 40; ++i) {
                const float mid = 0.5f * (lo + hi);
                (below(mid) ? hi : lo) = mid;
            }
            const Vec3f p = from + (to - from) * hi;
            return GroundHit3D{{p.x, height(p.x, p.z), p.z}, normal};
        };
    }
};

// Foot placement on a fake ground: flat ground moves nothing; a step down lowers the pelvis by the drop and puts the
// foot on it; a slope tilts the foot to the normal up to the clamp; the pelvis offset eases and a foot rising onto a
// step is not eased.
void test_ik_foot_placement() {
    const Rig base;
    const std::array<FootLeg3D, 2> legs = {FootLeg3D{{biped::hip_l, biped::knee_l, biped::ankle_l}, 0.05f},
                                           FootLeg3D{{biped::hip_r, biped::knee_r, biped::ankle_r}, 0.05f}};
    TwoBoneOptions3D knee;
    knee.pole_mode = PoleMode3D::Direction;
    knee.pole = {0, 0, 1};
    FootPlacementOptions3D options;
    options.smooth_seconds = 0.0f;

    // Flat: nothing moves, to rounding, and the pelvis stays.
    {
        FakeGround ground{[](float, float) { return 0.0f; }};
        Rig r = base;
        const FootPlan3D plan = plan_foot_placement(r.sk, r.pose, r.world, legs, ground.query(), options);
        assert(ground.casts == 2 && plan.pelvis_offset == 0.0f);
        for (std::size_t i = 0; i < 2; ++i) {
            assert(plan.feet[i].grounded && close(plan.feet[i].target, r.at(legs[i].leg.end), 1e-5f));
            assert(angle_degrees(plan.feet[i].rotation, r.rotation(legs[i].leg.end)) < 0.01f);
        }
        apply_foot_placement(r.sk, r.pose, r.world, biped::pelvis, legs, plan, knee);
        assert(max_difference(r.world, base.world) < 1e-4f);
    }
    // A step down under the left foot (x > 0): the pelvis goes down by the drop, the left foot lands on it, the right
    // stays.
    {
        FakeGround ground{[](float x, float) { return x > 0.0f ? -0.2f : 0.0f; }};
        Rig r = base;
        const FootPlan3D plan = plan_foot_placement(r.sk, r.pose, r.world, legs, ground.query(), options);
        assert(close(plan.pelvis_offset, -0.2f, 1e-5f));
        assert(close(plan.feet[0].target.y, 0.05f - 0.2f, 1e-5f) && close(plan.feet[1].target.y, 0.05f, 1e-5f));
        apply_foot_placement(r.sk, r.pose, r.world, biped::pelvis, legs, plan, knee);
        assert(close(r.at(biped::pelvis).y, 0.8f, 1e-5f));
        assert(close(r.at(biped::ankle_l), plan.feet[0].target, 2e-4f) &&
               close(r.at(biped::ankle_r), plan.feet[1].target, 2e-4f));
        // The leg that was long enough stays straight; the other bends, its knee forward (+Z, the pole).
        assert(r.at(biped::knee_r).z > base.at(biped::knee_r).z + 0.01f);
        // Bones kept their lengths.
        assert(close(r.bone(biped::hip_l, biped::knee_l), 0.5f, 1e-5f) &&
               close(r.bone(biped::knee_r, biped::ankle_r), 0.45f, 1e-5f));
    }
    // A step up under the left foot: the pelvis stays (a raise is off by default), the foot is lifted onto it; with a
    // raise allowed the pelvis takes the lowest rise.
    {
        FakeGround ground{[](float x, float) { return x > 0.0f ? 0.15f : 0.0f; }};
        Rig r = base;
        FootPlan3D plan = plan_foot_placement(r.sk, r.pose, r.world, legs, ground.query(), options);
        assert(plan.pelvis_offset == 0.0f && close(plan.feet[0].target.y, 0.2f, 1e-5f));
        FakeGround both{[](float, float) { return 0.1f; }};
        FootPlacementOptions3D raise = options;
        raise.max_pelvis_raise = 0.3f;
        plan = plan_foot_placement(r.sk, r.pose, r.world, legs, both.query(), raise);
        assert(close(plan.pelvis_offset, 0.1f, 1e-5f));
    }
    // A slope of 20 degrees (normal tilted toward +X): the foot's up follows the normal; a slope of 50 is clamped
    // to 30.
    for (const float degrees : {20.0f, 50.0f}) {
        const float rad = degrees * 3.14159265f / 180.0f;
        const Vec3f normal{std::sin(rad), std::cos(rad), 0};
        FakeGround ground{[&](float x, float) { return -std::tan(rad) * x; }, normal};
        Rig r = base;
        const FootPlan3D plan = plan_foot_placement(r.sk, r.pose, r.world, legs, ground.query(), options);
        // (The tilted foot's ankle sits a little off the straight-down place: a leg at its full length wants a little
        // stretch.)
        TwoBoneOptions3D stretchy = knee;
        stretchy.max_stretch = 1.5f;
        apply_foot_placement(r.sk, r.pose, r.world, biped::pelvis, legs, plan, stretchy);
        const float expect = std::min(degrees, options.max_tilt_degrees);
        const Vec3f foot_up = normalize(r.world[biped::ankle_l].apply_vector({0, 1, 0}));
        assert(close(angle_degrees(foot_up, {0, 1, 0}), expect, 0.1f));
        assert(close(angle_degrees(foot_up, normal), degrees - expect, 0.1f));
        assert(close(r.at(biped::ankle_l), plan.feet[0].target, 2e-4f));
    }
    // The pelvis eases to its offset: a drop is snapped to on the first call, then the ground comes back and the offset
    // rises by the time constant; a foot that falls away eases down, one that rises is lifted at once.
    {
        FootPlacementOptions3D eased = options;
        eased.smooth_seconds = 0.1f;
        FootPlacementState3D state;
        float x_drop = 1.0f;
        FakeGround ground{[&](float x, float) { return x > 0.0f ? -0.2f * x_drop : 0.0f; }};
        Rig r = base;
        FootPlan3D plan = plan_foot_placement(r.sk, r.pose, r.world, legs, ground.query(), eased, &state, 1.0f / 60.0f);
        assert(close(plan.pelvis_offset, -0.2f, 1e-5f));
        x_drop = 0.0f;
        float last = plan.pelvis_offset;
        for (int i = 0; i < 20; ++i) {
            plan = plan_foot_placement(r.sk, r.pose, r.world, legs, ground.query(), eased, &state, 1.0f / 60.0f);
            assert(plan.pelvis_offset > last - 1e-6f && plan.pelvis_offset <= 0.0f);
            last = plan.pelvis_offset;
        }
        assert(last > -0.2f * std::exp(-20.0f / 60.0f / 0.1f) - 1e-3f &&
               last < -0.2f * std::exp(-20.0f / 60.0f / 0.1f) + 1e-3f);
        // The foot: ground at 0, then it falls away to -0.2 (eases), then returns (at once).
        FootPlacementState3D fs;
        float drop = 0.0f;
        FakeGround g2{[&](float, float) { return drop; }};
        plan_foot_placement(r.sk, r.pose, r.world, legs, g2.query(), eased, &fs, 1.0f / 60.0f);
        drop = -0.2f;
        plan = plan_foot_placement(r.sk, r.pose, r.world, legs, g2.query(), eased, &fs, 1.0f / 60.0f);
        assert(plan.feet[0].target.y > 0.05f - 0.2f + 0.05f && plan.feet[0].target.y < 0.05f); // part of the way
        drop = 0.1f;
        plan = plan_foot_placement(r.sk, r.pose, r.world, legs, g2.query(), eased, &fs, 1.0f / 60.0f);
        assert(close(plan.feet[0].target.y, 0.15f, 1e-5f));
    }
    // No ground under a foot: it is left as animated, not grounded, and does not count for the pelvis.
    {
        FakeGround ground{[](float x, float) { return x > 0.0f ? -5.0f : 0.0f; }};
        Rig r = base;
        const FootPlan3D plan = plan_foot_placement(r.sk, r.pose, r.world, legs, ground.query(), options);
        assert(!plan.feet[0].grounded && plan.feet[1].grounded && plan.pelvis_offset == 0.0f);
        assert(close(plan.feet[0].target, r.at(biped::ankle_l), 1e-6f));
    }
}

// FABRIK: the tip reaches, bones keep their lengths, out of reach the chain is straight at the target, a free root
// follows.
void test_ik_fabrik() {
    const std::array<i32, 5> spine = {biped::pelvis, biped::spine, biped::chest, biped::neck, biped::head};
    Rig base;
    base.pose.joints[biped::spine].rotation = Quatf::axis_angle({1, 0, 0}, 20.0f);
    base.pose.joints[biped::chest].rotation = Quatf::axis_angle({0, 0, 1}, 25.0f);
    base.refresh();
    float reach = 0.0f;
    for (std::size_t i = 0; i + 1 < spine.size(); ++i) {
        reach += base.bone(spine[i], spine[i + 1]);
    }
    const Vec3f root = base.at(biped::pelvis);
    Lcg rng;
    for (int i = 0; i < 100; ++i) {
        Rig r = base;
        const Vec3f target = root + rng.direction() * (0.2f + 0.75f * (rng.next() * 0.5f + 0.5f)) * reach;
        const FabrikResult3D res = solve_chain_fabrik(r.sk, r.pose, r.world, spine, target);
        assert(res.solved && res.reached);
        assert(close(r.at(biped::head), target, 1e-3f));
        assert(close(r.at(biped::pelvis), root, 1e-5f));
        for (std::size_t k = 0; k + 1 < spine.size(); ++k) {
            assert(close(r.bone(spine[k], spine[k + 1]), base.bone(spine[k], spine[k + 1]), 1e-4f));
        }
        std::vector<Mat4f> again;
        compute_world_matrices(r.sk, r.pose, again);
        assert(max_difference(again, r.world) < 1e-4f);
    }
    {
        Rig r = base;
        const Vec3f dir = normalize({1, 0.5f, -0.4f});
        const FabrikResult3D res = solve_chain_fabrik(r.sk, r.pose, r.world, spine, root + dir * 3.0f);
        assert(res.solved && !res.reached);
        assert(close(r.at(biped::head), root + dir * reach, 1e-3f));
        assert(length(cross(r.at(biped::neck) - root, dir)) < 1e-3f);
    }
    {
        Rig r = base;
        FabrikOptions3D o;
        o.fix_root = false;
        const Vec3f target = root + Vec3f{2.0f, 1.0f, 0.0f};
        const FabrikResult3D res = solve_chain_fabrik(r.sk, r.pose, r.world, spine, target, o);
        assert(res.reached && close(r.at(biped::head), target, 1e-3f));
        assert(length(r.at(biped::pelvis) - root) > 0.5f);
        for (std::size_t k = 0; k + 1 < spine.size(); ++k) {
            assert(close(r.bone(spine[k], spine[k + 1]), base.bone(spine[k], spine[k + 1]), 1e-4f));
        }
    }
    {
        Rig r = base;
        FabrikOptions3D o;
        o.weight = 0.0f;
        assert(!solve_chain_fabrik(r.sk, r.pose, r.world, spine, root + Vec3f{0.3f, 0.3f, 0.3f}, o).solved);
        assert(std::memcmp(r.pose.joints.data(), base.pose.joints.data(),
                           base.pose.joints.size() * sizeof(JointPose3D)) == 0);
    }
}

// Solvers in order, each on what the one before made: a second solve of the same chain wins, a foot placer and an arm
// see the pelvis where the first put it; and bad input does nothing and says so.
void test_ik_order_and_bad_input() {
    const Rig base = bent_arm_rig();
    const Vec3f a = base.at(biped::shoulder_l) + Vec3f{0.4f, 0.1f, 0.3f},
                b = base.at(biped::shoulder_l) + Vec3f{0.2f, -0.5f, 0.4f};
    for (const bool a_first : {true, false}) {
        Rig r = base;
        solve_two_bone(r.sk, r.pose, r.world, left_arm, a_first ? a : b);
        solve_two_bone(r.sk, r.pose, r.world, left_arm, a_first ? b : a);
        assert(close(r.at(biped::hand_l), a_first ? b : a, 2e-4f));
    }
    // Pelvis lowered, then an arm reaching a fixed point: the arm's shoulder has moved with the pelvis and the hand
    // still arrives.
    {
        Rig r = base;
        const std::array<FootLeg3D, 1> legs = {FootLeg3D{{biped::hip_l, biped::knee_l, biped::ankle_l}, 0.05f}};
        FakeGround ground{[](float, float) { return -0.15f; }};
        const FootPlan3D plan = plan_foot_placement(r.sk, r.pose, r.world, legs, ground.query(), {});
        apply_foot_placement(r.sk, r.pose, r.world, biped::pelvis, legs, plan);
        assert(close(r.at(biped::shoulder_l).y, base.at(biped::shoulder_l).y - 0.15f, 1e-5f));
        const Vec3f hold = base.at(biped::shoulder_l) + Vec3f{0.3f, 0.4f, 0.3f};
        solve_two_bone(r.sk, r.pose, r.world, left_arm, hold);
        assert(close(r.at(biped::hand_l), hold, 2e-4f));
    }
    // Bad chains: nothing happens.
    Rig r = base;
    assert(
        !solve_two_bone(r.sk, r.pose, r.world, {biped::hand_l, biped::elbow_l, biped::shoulder_l}, {1, 1, 1}).solved);
    assert(!solve_two_bone(r.sk, r.pose, r.world, {biped::shoulder_l, biped::shoulder_l, biped::hand_l}, {1, 1, 1})
                .solved);
    assert(!solve_two_bone(r.sk, r.pose, r.world, {-1, 3, 4}, {1, 1, 1}).solved);
    assert(!solve_two_bone(r.sk, r.pose, r.world, {0, 1, 99}, {1, 1, 1}).solved);
    // A zero-length bone.
    r.pose.joints[biped::hand_l].translation = {};
    r.refresh();
    assert(!solve_two_bone(r.sk, r.pose, r.world, left_arm, {1, 1, 1}).solved);
}

// --- IK in the ECS animator ---

struct BipedCharacter {
    std::shared_ptr<const Model3D> model;
    const Skeleton3D* skeleton = nullptr;
    std::vector<Mat4f>
        rest_world; // the joints' world matrices at rest (a joint matrix times these is the world matrix)
};

BipedCharacter make_biped_character() {
    struct FakeMesh final : IMeshBackend3D {};
    auto model = std::make_shared<Model3D>();
    model->skeletons.push_back(skin_fixtures::make_biped());
    const Skeleton3D& sk = model->skeletons[0];
    // A clip that sways the spine, so that the pose the IK sees is not the rest pose.
    AnimClip3D sway;
    sway.name = "sway";
    sway.duration = 1.0f;
    sway.channels.resize(sk.joint_count());
    sway.channels[biped::spine].rotation.times = {0.0f, 0.5f, 1.0f};
    sway.channels[biped::spine].rotation.values = {Quatf{}, Quatf::axis_angle({0, 0, 1}, 15.0f), Quatf{}};
    model->clips.push_back(sway);
    ModelPart3D part;
    part.name = "body";
    part.mesh = Mesh3D{std::make_shared<FakeMesh>(), 3, 3, {}, {}, {}, sk.joint_count()};
    part.skin = 0;
    model->parts.push_back(part);
    BipedCharacter c;
    c.model = model;
    c.skeleton = &c.model->skeletons[0];
    compute_world_matrices(*c.skeleton, c.skeleton->rest_pose(), c.rest_world);
    return c;
}

// One frame as a game does it: animate, propagate, collect; returns the matrices the scene got.
std::vector<Mat4f> ik_frame(WorldRenderState3D& state, float dt) {
    state.animate(dt);
    return collected_matrices(state);
}

Vec3f world_position_of(const BipedCharacter& c, std::span<const Mat4f> joint_matrices, int joint,
                        const Mat4f& entity) {
    return entity.apply(
        (joint_matrices[static_cast<std::size_t>(joint)] * c.rest_world[static_cast<std::size_t>(joint)])
            .apply({0, 0, 0}));
}

IKConstraint3D hand_constraint(float fade = 0.0f) {
    IKConstraint3D k;
    k.name = "hand";
    k.kind = IKKind3D::TwoBone;
    k.joints = {"shoulder_L", "elbow_L", "hand_L"};
    k.fade_seconds = fade;
    return k;
}

// A hand reaches a target given in the world, for an entity that is moved, turned and scaled; the matrices drawn are
// the IK pose's.
void test_ecs_ik_reaches_world_target() {
    const BipedCharacter hero = make_biped_character();
    for (const float scale : {1.0f, 1.7f}) {
        kin::EcsWorld world;
        WorldRenderState3D state{world};
        auto& w = world.raw();
        const flecs::entity e = w.entity()
                                    .set(Transform3D{.position = {2, 0, 5},
                                                     .rotation = Quatf::axis_angle({0, 1, 0}, 70.0f),
                                                     .scale = {scale, scale, scale}})
                                    .set(SkinnedMeshRenderer3D{.model = hero.model})
                                    .set(Animator3D{.clip = "sway"});
        Animator3D& a = *e.get_mut<Animator3D>();
        a.add_ik(hand_constraint());
        // A point in front of the character's left shoulder, in the world.
        const Mat4f entity_matrix = e.get<Transform3D>()->matrix();
        const Vec3f shoulder =
            entity_matrix.apply(hero.skeleton->rest[biped::shoulder_l].translation + Vec3f{0, 1.45f, 0});
        const Vec3f target = shoulder + Vec3f{0.0f, 0.1f, 0.5f * scale};
        a.ik("hand").set_target(target).set_pole_direction({0, 0, -1});
        std::vector<Mat4f> got;
        for (int i = 0; i < 5; ++i) {
            got = ik_frame(state, 1.0f / 60.0f);
        }
        assert(close(world_position_of(hero, got, biped::hand_l, entity_matrix), target, 2e-4f * scale));
        // The same pose is what the animator keeps for the game.
        const Animator3D& after = *e.get<Animator3D>();
        assert(after.ik_world.size() == hero.skeleton->joint_count());
        assert(close(entity_matrix.apply(Vec3f{after.ik_world[biped::hand_l].m[12], after.ik_world[biped::hand_l].m[13],
                                               after.ik_world[biped::hand_l].m[14]}),
                     target, 2e-4f * scale));
        // The entity moved and turned: the hand stays on the target in the world (the target is a world point).
        e.set(Transform3D{.position = {2.2f, 0, 5.1f},
                          .rotation = Quatf::axis_angle({0, 1, 0}, 85.0f),
                          .scale = {scale, scale, scale}});
        const Mat4f moved = e.get<Transform3D>()->matrix();
        got = ik_frame(state, 1.0f / 60.0f);
        const Vec3f shoulder_now = world_position_of(hero, got, biped::shoulder_l, moved);
        if (length(target - shoulder_now) < 0.9f * scale) {
            assert(close(world_position_of(hero, got, biped::hand_l, moved), target, 2e-4f * scale));
        }
        // The elbow is on the pole's side: the pole was model -Z, so the elbow is behind the line shoulder-hand in the
        // model.
        const Mat4f inv = moved.inverse();
        const Vec3f s = inv.apply(world_position_of(hero, got, biped::shoulder_l, moved)),
                    el = inv.apply(world_position_of(hero, got, biped::elbow_l, moved)),
                    h = inv.apply(world_position_of(hero, got, biped::hand_l, moved));
        const Vec3f axis = normalize(h - s);
        const Vec3f off = (el - s) - axis * dot(el - s, axis);
        assert(off.z < -0.01f);
    }
}

// An entity as the target follows it; the entity going away is safe and lets go over the fade (from the last place it
// was); a fade in takes its time and a set_weight(0) lets go.
void test_ecs_ik_entity_target_and_fades() {
    const BipedCharacter hero = make_biped_character();
    kin::EcsWorld world;
    WorldRenderState3D state{world};
    auto& w = world.raw();
    const flecs::entity hold = w.entity().set(Transform3D{.position = {0.5f, 1.45f, 0.5f}});
    const flecs::entity e =
        w.entity().set(Transform3D{}).set(SkinnedMeshRenderer3D{.model = hero.model}).set(Animator3D{.clip = "sway"});
    Animator3D& a = *e.get_mut<Animator3D>();
    a.add_ik(hand_constraint(0.5f)).set_target(hold, {0, 0.05f, 0}).set_pole_direction({0, 0, -1});
    const float dt = 0.1f;
    const Vec3f rest_hand =
        world_position_of(hero, matrices_of(hero.skeleton->rest_pose(), *hero.skeleton), biped::hand_l, Mat4f{});
    // Fade in over half a second: the hand is between at 0.25 s and on the hold at 0.5 s and later.
    std::vector<Mat4f> got;
    Vec3f target = hold.get<Transform3D>()->position + Vec3f{0, 0.05f, 0};
    float last = 1e9f;
    for (int i = 0; i < 8; ++i) {
        got = ik_frame(state, dt);
        const float err = length(world_position_of(hero, got, biped::hand_l, Mat4f{}) - target);
        if (i >= 1) {
            assert(err <= last + 1e-4f); // gets nearer
        }
        if (i == 2) { // 0.3 s: partly there
            assert(err > 0.02f && err < length(rest_hand - target));
        }
        if (i >= 5) {
            assert(err < 2e-4f);
        }
        last = err;
    }
    // The hold moves: the hand follows it.
    hold.set(Transform3D{.position = {0.4f, 1.2f, 0.6f}});
    target = Vec3f{0.4f, 1.25f, 0.6f};
    got = ik_frame(state, dt);
    assert(close(world_position_of(hero, got, biped::hand_l, Mat4f{}), target, 3e-4f));
    // Destroyed: safe, and the hand lets go from where the hold was, over the fade (the weight falls steadily, the hand
    // is still on the old place at first and then on the clips' pose).
    hold.destruct();
    float weight = 1.0f;
    for (int i = 0; i < 8; ++i) {
        got = ik_frame(state, dt);
        const float c = e.get<Animator3D>()->ik_constraints[0].current;
        assert(c <= weight + 1e-6f && c >= 0.0f);
        if (i == 0) {
            assert(c < 1.0f && c > 0.5f);
            assert(length(world_position_of(hero, got, biped::hand_l, Mat4f{}) - target) < 0.15f);
        }
        weight = c;
    }
    // Nothing left of the weight: the animated pose (the hand where the clips have it).
    const Animator3D& after = *e.get<Animator3D>();
    assert(after.ik_constraints[0].current == 0.0f);
    Pose3D clip_pose = after.player.pose();
    assert(close(world_position_of(hero, got, biped::hand_l, Mat4f{}),
                 world_position_of(hero, matrices_of(clip_pose, *hero.skeleton), biped::hand_l, Mat4f{}), 1e-4f));
    // set_weight: up and down with a fade again, with a world point.
    Animator3D& b = *e.get_mut<Animator3D>();
    b.ik("hand").set_target(Vec3f{0.3f, 1.4f, 0.5f}).set_weight(1.0f);
    for (int i = 0; i < 7; ++i) {
        got = ik_frame(state, dt);
    }
    assert(close(world_position_of(hero, got, biped::hand_l, Mat4f{}), {0.3f, 1.4f, 0.5f}, 3e-4f));
    e.get_mut<Animator3D>()->ik("hand").set_weight(0.0f);
    for (int i = 0; i < 7; ++i) {
        got = ik_frame(state, dt);
    }
    assert(close(world_position_of(hero, got, biped::hand_l, Mat4f{}),
                 world_position_of(hero, matrices_of(e.get<Animator3D>()->player.pose(), *hero.skeleton), biped::hand_l,
                                   Mat4f{}),
                 1e-4f));
}

// Constraints run in list order, each on the pose the one before left; an animator with none is exactly what it was
// (the matrices are those of the player's pose, to the bit, and no IK pose is kept).
void test_ecs_ik_order_and_off_path() {
    const BipedCharacter hero = make_biped_character();
    const auto run = [&](bool with_ik, bool swapped) {
        kin::EcsWorld world;
        WorldRenderState3D state{world};
        auto& w = world.raw();
        const flecs::entity e = w.entity()
                                    .set(Transform3D{.position = {1, 0, 0}})
                                    .set(SkinnedMeshRenderer3D{.model = hero.model})
                                    .set(Animator3D{.clip = "sway"});
        if (with_ik) {
            Animator3D& a = *e.get_mut<Animator3D>();
            IKConstraint3D first = hand_constraint();
            first.name = "first";
            first.target = {IKTarget3D::Kind::World, {1.4f, 1.5f, 0.4f}, 0};
            IKConstraint3D second = hand_constraint();
            second.name = "second";
            second.target = {IKTarget3D::Kind::World, {1.2f, 1.1f, 0.5f}, 0};
            if (swapped) {
                a.add_ik(second);
                a.add_ik(first);
            } else {
                a.add_ik(first);
                a.add_ik(second);
            }
        }
        std::vector<Mat4f> got;
        for (int i = 0; i < 4; ++i) {
            got = ik_frame(state, 1.0f / 60.0f);
        }
        return std::pair{got, e.get<Animator3D>()->ik_world.size()};
    };
    const auto [plain, plain_world] = run(false, false);
    assert(plain_world == 0);
    // Against the player's own matrices.
    {
        kin::EcsWorld world;
        WorldRenderState3D state{world};
        const flecs::entity e = world.raw()
                                    .entity()
                                    .set(Transform3D{})
                                    .set(SkinnedMeshRenderer3D{.model = hero.model})
                                    .set(Animator3D{.clip = "sway"});
        const auto got = ik_frame(state, 1.0f / 60.0f);
        std::vector<Mat4f> direct;
        e.get<Animator3D>()->player.joint_matrices(direct);
        assert(got.size() == direct.size() && std::memcmp(got.data(), direct.data(), got.size() * sizeof(Mat4f)) == 0);
    }
    const Mat4f entity = Mat4f::translation({1, 0, 0});
    const auto [ab, ab_world] = run(true, false);
    const auto [ba, ba_world] = run(true, true);
    assert(ab_world == hero.skeleton->joint_count() && ba_world == ab_world);
    // The later constraint wins on the same chain.
    assert(close(world_position_of(hero, ab, biped::hand_l, entity), {1.2f, 1.1f, 0.5f}, 3e-4f));
    assert(close(world_position_of(hero, ba, biped::hand_l, entity), {1.4f, 1.5f, 0.4f}, 3e-4f));
}

// The same frames twice give the same bytes, and a constraint whose joints are not in the skeleton does nothing.
void test_ecs_ik_deterministic_and_unresolved() {
    const BipedCharacter hero = make_biped_character();
    const auto run = [&]() {
        kin::EcsWorld world;
        WorldRenderState3D state{world};
        auto& w = world.raw();
        u64 h = 1469598103934665603ull;
        for (int n = 0; n < 6; ++n) {
            const flecs::entity e =
                w.entity()
                    .set(Transform3D{.position = {static_cast<float>(n), 0, 0},
                                     .rotation = Quatf::axis_angle({0, 1, 0}, 20.0f * static_cast<float>(n))})
                    .set(SkinnedMeshRenderer3D{.model = hero.model})
                    .set(Animator3D{.clip = "sway"});
            Animator3D& a = *e.get_mut<Animator3D>();
            a.add_ik(hand_constraint(0.2f))
                .set_target(Vec3f{static_cast<float>(n) + 0.3f, 1.4f, 0.5f})
                .set_pole_direction({0, 0, -1});
            IKConstraint3D look;
            look.name = "look";
            look.kind = IKKind3D::LookAt;
            look.joints = {"neck", "head"};
            look.look_links = {{.weight = 0.4f}, {.weight = 0.6f, .max_yaw_degrees = 70.0f}};
            look.look_at.smooth_seconds = 0.15f;
            look.target = {IKTarget3D::Kind::World, {static_cast<float>(n) + 2.0f, 1.7f, 1.0f}, 0};
            a.add_ik(look);
        }
        for (int i = 0; i < 40; ++i) {
            const auto m = ik_frame(state, 1.0f / 60.0f);
            h = (h ^ std::hash<std::string_view>{}(
                         std::string_view{reinterpret_cast<const char*>(m.data()), m.size() * sizeof(Mat4f)})) *
                1099511628211ull;
        }
        return h;
    };
    assert(run() == run());
    // Joints that do not exist: no effect (and no crash), the plain pose.
    kin::EcsWorld world;
    WorldRenderState3D state{world};
    const flecs::entity e = world.raw()
                                .entity()
                                .set(Transform3D{})
                                .set(SkinnedMeshRenderer3D{.model = hero.model})
                                .set(Animator3D{.clip = "sway"});
    IKConstraint3D bad = hand_constraint();
    bad.joints = {"shoulder_L", "no_such_joint", "hand_L"};
    e.get_mut<Animator3D>()->add_ik(bad).set_target(Vec3f{1, 1, 1});
    const auto with = ik_frame(state, 0.1f);
    std::vector<Mat4f> direct;
    e.get<Animator3D>()->player.joint_matrices(direct);
    assert(max_difference(with, direct) < 1e-6f);
}

// --- the fixes to P1b's known limits: joint limits, rest-bend fallback, scaled chains, the bend's swing, FABRIK in the
// ECS, foot locking ---

// The twist of a rotation about `axis` (its swing-twist decomposition), in degrees, signed.
float twist_degrees(Quatf q, Vec3f axis) {
    if (q.w < 0.0f) {
        q = {-q.x, -q.y, -q.z, -q.w};
    }
    return 2.0f * std::atan2(q.x * axis.x + q.y * axis.y + q.z * axis.z, q.w) * 180.0f / 3.14159265f;
}

// The across-the-axis part of (p - from), for where a joint sits to the side of the line from `from` to `to`.
Vec3f side_of(Vec3f p, Vec3f from, Vec3f to) {
    const Vec3f axis = normalize(to - from);
    const Vec3f v = p - from;
    return v - axis * dot(v, axis);
}

// A rig whose rest pose has `joint` turned (so that the rest chain is bent), pose at rest.
Rig rig_with_rest_turn(int joint, Quatf turn) {
    Skeleton3D sk = skin_fixtures::make_biped();
    sk.rest[static_cast<std::size_t>(joint)].rotation = turn;
    std::vector<Mat4f> w;
    compute_world_matrices(sk, sk.rest_pose(), w);
    sk.inverse_bind.clear();
    for (const Mat4f& m : w) {
        sk.inverse_bind.push_back(m.inverse());
    }
    return Rig{std::move(sk)};
}

// Item 1: the mid joint's maximum bend; the hinge (it only bends one way, about its own axis); the root's twist limit.
void test_ik_joint_limits() {
    const Rig base;
    const Vec3f hip = base.at(biped::hip_l);
    const float l1 = base.bone(biped::hip_l, biped::knee_l), l2 = base.bone(biped::knee_l, biped::ankle_l);

    // A maximum bend: a target near the hip makes the knee fold as far as the limit lets it and no further; the foot
    // then stands at the chord of that fold, toward the target (as near it as a hole round the root, as with unequal
    // bones, lets).
    for (const float most : {150.0f, 100.0f}) {
        Rig r = base;
        TwoBoneOptions3D o;
        o.pole_mode = PoleMode3D::Direction;
        o.pole = {0, 0, 1};
        o.max_bend_degrees = most;
        const Vec3f target = hip + normalize(Vec3f{0.2f, -0.3f, 0.5f}) * 0.12f;
        const TwoBoneResult3D res = solve_two_bone(r.sk, r.pose, r.world, left_leg, target, o);
        assert(res.solved && !res.reached && close(res.bend_degrees, most, 0.05f));
        const float chord = std::sqrt(l1 * l1 + l2 * l2 + 2.0f * l1 * l2 * std::cos(most * 3.14159265f / 180.0f));
        assert(close(length(r.at(biped::ankle_l) - hip), chord, 2e-4f));
        assert(dot(normalize(r.at(biped::ankle_l) - hip), normalize(target - hip)) >
               0.3f); // toward it (not on the line: the hole's aim)
        assert(close(r.bone(biped::hip_l, biped::knee_l), l1, 1e-5f) &&
               close(r.bone(biped::knee_l, biped::ankle_l), l2, 1e-5f));
        // A target further out than the fold's chord is not changed by the limit: the pose is the unlimited one's, bit
        // for bit.
        Rig far_limited = base, far_free = base;
        const Vec3f far_target = hip + Vec3f{0.1f, -0.7f, 0.3f};
        solve_two_bone(far_limited.sk, far_limited.pose, far_limited.world, left_leg, far_target, o);
        o.max_bend_degrees = 180.0f;
        solve_two_bone(far_free.sk, far_free.pose, far_free.world, left_leg, far_target, o);
        assert(std::memcmp(far_limited.pose.joints.data(), far_free.pose.joints.data(),
                           far_free.pose.joints.size() * sizeof(JointPose3D)) == 0);
    }

    // The hinge: with the knee's X as the axis, every reachable target and every pole leave the knee's local rotation a
    // pure turn about X by the bend, the positive way. (A straight leg has no plane of its own: it is the hinge's.)
    {
        Lcg rng;
        int pure = 0, free_pure = 0;
        for (int i = 0; i < 300; ++i) {
            Rig r = base, f = base;
            const Vec3f target = hip + rng.direction() * (0.2f + 0.7f * (rng.next() * 0.5f + 0.5f)) * (l1 + l2);
            TwoBoneOptions3D o;
            o.pole_mode = PoleMode3D::Direction;
            o.pole = rng.direction();
            const TwoBoneResult3D free_res = solve_two_bone(f.sk, f.pose, f.world, left_leg, target, o);
            Quatf fq = f.pose.joints[biped::knee_l].rotation;
            free_pure += std::fabs(fq.y) < 1e-3f && std::fabs(fq.z) < 1e-3f ? 1 : 0;
            o.hinge = true;
            o.hinge_axis = {1, 0, 0};
            const TwoBoneResult3D res = solve_two_bone(r.sk, r.pose, r.world, left_leg, target, o);
            assert(res.solved && res.reached && close(r.at(biped::ankle_l), target, 2e-4f));
            // The same place for the knee and foot (the hinge changes the twist of the bones, not where the joints
            // are). (A pole near the target's axis leans on the pose's own side a little, which the hinge gives
            // differently: not compared.)
            const Vec3f axis = normalize(target - hip);
            if (length(o.pole - axis * dot(o.pole, axis)) > 0.5f) {
                assert(close(r.at(biped::knee_l), f.at(biped::knee_l), 1e-4f) &&
                       close(res.bend_degrees, free_res.bend_degrees, 1e-3f));
            }
            Quatf q = r.pose.joints[biped::knee_l].rotation;
            if (q.w < 0.0f) {
                q = {-q.x, -q.y, -q.z, -q.w};
            }
            assert(std::fabs(q.y) < 1e-3f && std::fabs(q.z) < 1e-3f);
            assert(q.x > -1e-4f && close(twist_degrees(q, {1, 0, 0}), res.bend_degrees, 0.1f));
            ++pure;
        }
        std::cout << "  ik hinge: " << pure << " of 300 knees are pure hinges with it, " << free_pure << " without"
                  << std::endl;
        assert(free_pure < 150);
    }

    // A knee that a clip hyperextended (bent backward, about X by -25 degrees) is brought to bend the right way by the
    // hinge, to the same angle and with the foot where it was; without the hinge the solver keeps the clip's side.
    {
        Rig bent = base;
        bent.pose.joints[biped::knee_l].rotation = Quatf::axis_angle({1, 0, 0}, -25.0f);
        bent.refresh();
        const Vec3f target = bent.at(biped::ankle_l);
        const auto knee_forward = [&](bool hinge) {
            Rig r = bent;
            TwoBoneOptions3D o;
            o.hinge = hinge;
            o.hinge_axis = {1, 0, 0};
            const TwoBoneResult3D res = solve_two_bone(r.sk, r.pose, r.world, left_leg, target, o);
            assert(res.reached && close(r.at(biped::ankle_l), target, 2e-4f) && close(res.bend_degrees, 25.0f, 0.2f));
            return side_of(r.at(biped::knee_l), hip, target).z;
        };
        assert(knee_forward(false) < -0.01f);
        assert(knee_forward(true) > 0.01f);
    }

    // The twist limit: a pole behind a leg that has to bend forward. Without a limit the hip turns half way round to
    // put the knee behind; with 30 degrees it turns 30 and the knee is mostly forward, the foot still on the target.
    {
        const Vec3f target = hip + Vec3f{0.05f, -0.8f, 0.25f};
        const auto solve = [&](float limit, Vec3f pole) {
            Rig r = base;
            TwoBoneOptions3D o;
            o.hinge = true;
            o.hinge_axis = {1, 0, 0};
            o.pole_mode = PoleMode3D::Direction;
            o.pole = pole;
            o.max_twist_degrees = limit;
            const TwoBoneResult3D res = solve_two_bone(r.sk, r.pose, r.world, left_leg, target, o);
            assert(res.reached && close(r.at(biped::ankle_l), target, 2e-4f));
            const Quatf delta =
                (r.pose.joints[biped::hip_l].rotation * base.pose.joints[biped::hip_l].rotation.conjugate())
                    .normalized();
            return std::pair{r, std::fabs(twist_degrees(delta, {0, -1, 0}))};
        };
        const auto [free_rig, free_twist] = solve(180.0f, {0, 0, -1});
        assert(free_twist > 120.0f && side_of(free_rig.at(biped::knee_l), hip, target).z < -0.01f);
        const auto [held, held_twist] = solve(30.0f, {0, 0, -1});
        assert(held_twist < 30.05f);
        // Still bent the way the hinge allows: the knee is on the forward side (the clips' side), though the pole says
        // back.
        assert(side_of(held.at(biped::knee_l), hip, target).z > 0.0f);
        // Pole to the side (out): 30 degrees of twist is not enough to reach it from the front, so it is partway.
        const auto [part, part_twist] = solve(30.0f, {1, 0, 0});
        assert(part_twist < 30.05f && part_twist > 5.0f);
        // A limit that is not reached changes nothing: the pole is a few degrees from the clips' side.
        const auto [near, near_twist] = solve(30.0f, normalize(Vec3f{0.1f, 0, 1}));
        const auto [near_free, nf_twist] = solve(180.0f, normalize(Vec3f{0.1f, 0, 1}));
        assert(close(near.at(biped::knee_l), near_free.at(biped::knee_l), 1e-5f) && close(near_twist, nf_twist, 1e-3f));
    }

    // Continuity: the pole goes all the way round the leg (720 steps, 0.5 degrees each) with a limit of 30: the knee
    // moves smoothly, by about as much as the pole does (its elbow-to-pole gain is at most a few), never a jump, and
    // the foot is always on target.
    for (const float limit : {30.0f, 90.0f, 180.0f}) {
        const Vec3f target = hip + Vec3f{0.05f, -0.8f, 0.25f};
        Rig previous;
        float worst = 0.0f;
        for (int i = 0; i <= 720; ++i) {
            Rig r = base;
            const float a = static_cast<float>(i) * 0.5f * 3.14159265f / 180.0f;
            TwoBoneOptions3D o;
            o.hinge = true;
            o.hinge_axis = {1, 0, 0};
            o.pole_mode = PoleMode3D::Direction;
            o.pole = {std::sin(a), 0.2f, std::cos(a)};
            o.max_twist_degrees = limit;
            const TwoBoneResult3D res = solve_two_bone(r.sk, r.pose, r.world, left_leg, target, o);
            assert(res.reached && close(r.at(biped::ankle_l), target, 2e-4f));
            if (i > 0) {
                worst = std::max(worst, angle_degrees(side_of(r.at(biped::knee_l), hip, target),
                                                      side_of(previous.at(biped::knee_l), hip, target)));
            }
            previous = std::move(r);
        }
        std::cout << "  ik twist limit " << limit << ": pole round the leg in 0.5 degree steps, worst step of the knee "
                  << worst << " degrees" << std::endl;
        assert(worst < 4.0f);
    }
}

// Item 5: a straight chain with no pole bends the way the rest pose is bent (what the rig's author built), not toward
// -Y.
void test_ik_rest_bend_fallback() {
    // An arm (along +X) whose elbow is built bent (a turn of 30 degrees about Y in its rest pose: the forearm toward
    // -Z, the elbow pointing forward, +Z); a pose that has the forearm straight on the upper arm (the elbow's own turn
    // undone).
    const Quatf built = Quatf::axis_angle({0, 1, 0}, 30.0f);
    Rig rig = rig_with_rest_turn(biped::elbow_l, built);
    const Vec3f shoulder = rig.at(biped::shoulder_l);
    // Straight pose: the elbow's own turn cancelled.
    rig.pose.joints[biped::elbow_l].rotation = Quatf{};
    rig.refresh();
    assert(angle_degrees(rig.at(biped::hand_l) - shoulder, rig.at(biped::elbow_l) - shoulder) < 0.01f);
    const Vec3f target = shoulder + Vec3f{0.25f, -0.2f, 0.1f};
    const auto elbow_side = [&](const Rig& base_rig) {
        Rig r = base_rig;
        const TwoBoneResult3D res = solve_two_bone(r.sk, r.pose, r.world, left_arm, target);
        assert(res.reached && close(r.at(biped::hand_l), target, 2e-4f));
        return side_of(r.at(biped::elbow_l), shoulder, target);
    };
    const Vec3f side = elbow_side(rig);
    // The rest elbow's side (from the line shoulder-hand): the solver's elbow lies on that side, whichever way it is
    // (not down).
    Rig at_rest = rig_with_rest_turn(biped::elbow_l, built);
    const Vec3f rest_side = side_of(at_rest.at(biped::elbow_l), shoulder, at_rest.at(biped::hand_l));
    assert(length(rest_side) > 0.02f);
    assert(dot(normalize(side), normalize(rest_side)) > 0.4f);
    // The straight rest (the biped as it is): the old behaviour, an elbow down.
    const Rig straight_rig;
    const Vec3f old_side = elbow_side(straight_rig);
    assert(old_side.y < -0.01f);
    // The same rig with the hinge named: the side comes from the hinge when the rest is straight (a knee's X: forward).
    {
        Rig leg = straight_rig;
        const Vec3f hip = leg.at(biped::hip_l);
        const Vec3f t = hip + Vec3f{0.05f, -0.75f, 0.1f};
        TwoBoneOptions3D o;
        o.hinge = true;
        o.hinge_axis = {1, 0, 0};
        solve_two_bone(leg.sk, leg.pose, leg.world, left_leg, t, o);
        assert(side_of(leg.at(biped::knee_l), hip, t).z > 0.01f);
        Rig no_hinge = straight_rig;
        solve_two_bone(no_hinge.sk, no_hinge.pose, no_hinge.world, left_leg, t);
        assert(side_of(no_hinge.at(biped::knee_l), hip, t).y < side_of(leg.at(biped::knee_l), hip, t).y);
    }
    // Continuity: a sweep of the straight pose's target round the root (no pole) keeps small steps with the rest bend
    // as the default.
    {
        Rig previous;
        float worst = 0.0f;
        for (int i = 0; i <= 2000; ++i) {
            Rig r = rig;
            const float u = static_cast<float>(i) / 2000.0f * 6.2831853f;
            const Vec3f t = shoulder + Vec3f{0.3f + 0.4f * std::cos(u), 0.4f * std::sin(u), 0.2f};
            const TwoBoneResult3D res = solve_two_bone(r.sk, r.pose, r.world, left_arm, t);
            assert(res.solved);
            if (i > 0) {
                worst = std::max(worst, pose_step(previous, r, left_arm).worst());
            }
            previous = std::move(r);
        }
        std::cout << "  ik rest-bend fallback, straight pose, no pole: worst step " << worst << " degrees" << std::endl;
        assert(worst < 4.0f);
    }
}

// Item 4: a non-uniform scale on the root joint itself, on the mid joint, and on both: the hand lands on targets that
// are reachable (made by turning the joints of a scaled arm), the pose matches a full recompute, a second solve changes
// nothing.
void test_ik_two_bone_nonuniform_scale() {
    struct Case {
        const char* what;
        Vec3f root_scale, mid_scale;
    };
    const Case cases[] = {
        {"root joint scaled (1.5, 0.6, 2)", {1.5f, 0.6f, 2.0f}, {1, 1, 1}},
        {"mid joint scaled (0.7, 1.4, 1.1)", {1, 1, 1}, {0.7f, 1.4f, 1.1f}},
        {"both", {1.3f, 0.8f, 1.6f}, {0.8f, 1.2f, 0.9f}},
    };
    for (const Case& c : cases) {
        Lcg rng;
        float worst_hand = 0.0f, worst_length = 0.0f;
        int solved = 0;
        for (int i = 0; i < 120; ++i) {
            Rig truth = bent_arm_rig();
            truth.pose.joints[biped::shoulder_l].scale = c.root_scale;
            truth.pose.joints[biped::elbow_l].scale = c.mid_scale;
            truth.refresh();
            // A reachable target: the hand of this arm turned at random.
            Rig made = truth;
            made.pose.joints[biped::shoulder_l].rotation =
                Quatf::axis_angle(rng.direction(), 40.0f * (rng.next() * 0.5f + 0.5f)) *
                truth.pose.joints[biped::shoulder_l].rotation;
            made.pose.joints[biped::elbow_l].rotation =
                Quatf::axis_angle({0.1f, 1, 0.2f}, -10.0f - 90.0f * (rng.next() * 0.5f + 0.5f));
            made.refresh();
            const Vec3f target = made.at(biped::hand_l);
            Rig r = truth;
            TwoBoneOptions3D o;
            o.pole_mode = PoleMode3D::Direction;
            o.pole = normalize(side_of(made.at(biped::elbow_l), made.at(biped::shoulder_l),
                                       made.at(biped::hand_l))); // the side it was made with
            const TwoBoneResult3D res = solve_two_bone(r.sk, r.pose, r.world, left_arm, target, o);
            assert(res.solved);
            worst_hand = std::max(worst_hand, length(r.at(biped::hand_l) - target));
            // The matrices kept in step are the pose's own.
            std::vector<Mat4f> again;
            compute_world_matrices(r.sk, r.pose, again);
            assert(max_difference(again, r.world) < 1e-4f);
            // Solving again moves nothing.
            const std::vector<Mat4f> once = r.world;
            solve_two_bone(r.sk, r.pose, r.world, left_arm, target, o);
            assert(max_difference(once, r.world) < 5e-5f);
            // Bones: the upper bone is what it was; the forearm is the length it is drawn at, which depends on how the
            // scaled joints turn it, so it is not asserted equal (no rotation can keep it).
            worst_length = std::max(worst_length, std::fabs(r.bone(biped::shoulder_l, biped::elbow_l) -
                                                            truth.bone(biped::shoulder_l, biped::elbow_l)));
            ++solved;
        }
        std::cout << "  ik scaled chain, " << c.what << ": hand within " << worst_hand
                  << " of 120 reachable targets, upper bone off by at most " << worst_length << "" << std::endl;
        assert(solved == 120 && worst_hand < 2e-4f && worst_length < 1e-4f);
    }
    // The end orientation on a chain whose mid joint is scaled: the matrices' own rotation of the hand reaches the
    // goal.
    {
        Rig r = bent_arm_rig();
        r.pose.joints[biped::elbow_l].scale = {0.7f, 1.4f, 1.1f};
        r.pose.joints[biped::shoulder_l].scale = {1.4f, 0.7f, 1.2f};
        r.refresh();
        const Vec3f target = r.at(biped::shoulder_l) + Vec3f{0.3f, -0.15f, 0.35f};
        const Quatf want = Quatf::axis_angle({0.4f, 1, -0.3f}, 110.0f);
        TwoBoneOptions3D o;
        o.pole_mode = PoleMode3D::Direction;
        o.pole = {0, 0, -1};
        o.match_rotation = true;
        o.end_rotation = want;
        const TwoBoneResult3D res = solve_two_bone(r.sk, r.pose, r.world, left_arm, target, o);
        assert(res.solved);
        const float angle = angle_degrees(r.rotation(biped::hand_l), want);
        std::cout << "  ik scaled chain, orientation: hand " << length(r.at(biped::hand_l) - target)
                  << " from the target, " << angle << " degrees from the goal" << std::endl;
        assert(angle < 0.05f && length(r.at(biped::hand_l) - target) < 2e-4f);
    }
    // A weight of 0.5 on a scaled chain is half way (as an angle) between the pose and the full solve, for both
    // rotations.
    {
        Rig base = bent_arm_rig();
        base.pose.joints[biped::shoulder_l].scale = {1.5f, 0.6f, 2.0f};
        base.refresh();
        const Vec3f target = base.at(biped::shoulder_l) + Vec3f{0.2f, -0.3f, 0.4f};
        TwoBoneOptions3D o;
        o.pole_mode = PoleMode3D::Direction;
        o.pole = {0, 1, 0};
        Rig full = base, half = base;
        solve_two_bone(full.sk, full.pose, full.world, left_arm, target, o);
        o.weight = 0.5f;
        solve_two_bone(half.sk, half.pose, half.world, left_arm, target, o);
        for (int j : {biped::shoulder_l, biped::elbow_l}) {
            const float whole = angle_degrees(base.pose.joints[static_cast<std::size_t>(j)].rotation,
                                              full.pose.joints[static_cast<std::size_t>(j)].rotation);
            const float part = angle_degrees(base.pose.joints[static_cast<std::size_t>(j)].rotation,
                                             half.pose.joints[static_cast<std::size_t>(j)].rotation);
            assert(whole > 3.0f && close(part, whole * 0.5f, 0.1f));
        }
    }
}

// Item 6: the elbow's side through the pole's axis, and the fold's centre. Statelessly the side has to turn over a few
// degrees of the target's travel (the hairy-ball theorem: no tangent field on the sphere of target directions is
// without a zero), over time with a TwoBoneState3D it turns at a bounded speed.
void test_ik_bend_swing_and_singularities() {
    const Rig base = bent_arm_rig();
    const Vec3f root = base.at(biped::shoulder_l);
    TwoBoneOptions3D pole;
    pole.pole_mode = PoleMode3D::Direction;
    pole.pole = {0, 1, 0};
    // A target crossing the pole's axis at the rate of a hand across the top of the head, in 120 frames at 60 Hz (2 s):
    // the stateless side turns over a handful of frames; with a swing speed it is limited.
    const Vec3f across_dir = normalize(Vec3f{1.0f, 0.0f, 0.6f});
    const auto run = [&](float speed_degrees, int steps) {
        TwoBoneState3D state;
        Rig previous;
        float worst_side = 0.0f, worst_hand_err = 0.0f;
        for (int i = 0; i <= steps; ++i) {
            Rig r = base;
            const float u = static_cast<float>(i) / static_cast<float>(steps);
            const Vec3f target = root + Vec3f{0, 0.7f, 0} + across_dir * (u * 0.6f - 0.3f);
            TwoBoneOptions3D o = pole;
            o.bend_swing_degrees_per_second = speed_degrees;
            const TwoBoneResult3D res =
                solve_two_bone(r.sk, r.pose, r.world, left_arm, target, o, &state, 1.0f / 60.0f);
            assert(res.solved && res.reached);
            worst_hand_err = std::max(worst_hand_err, length(r.at(biped::hand_l) - target));
            if (i > 0) {
                worst_side = std::max(
                    worst_side, angle_degrees(side_of(r.at(biped::elbow_l), root, r.at(biped::hand_l)),
                                              side_of(previous.at(biped::elbow_l), root, previous.at(biped::hand_l))));
            }
            previous = std::move(r);
        }
        assert(worst_hand_err < 2e-4f);
        return worst_side;
    };
    for (const int frames : {120, 40}) {
        const float free_worst = run(0.0f, frames);
        const float limited = run(360.0f, frames);
        std::cout << "  ik pole-axis crossing in " << frames << " frames: worst step of the elbow's side " << free_worst
                  << " degrees stateless, " << limited << " with a swing of 360 degrees a second (6 a frame)"
                  << std::endl;
        assert(limited < 6.0f + 0.5f);
        if (frames == 40) {
            assert(free_worst > 15.0f);
        }
    }
    // And it gets where the stateless side is, given time: hold the last target for a second.
    {
        TwoBoneState3D state;
        const Vec3f end_target = root + Vec3f{0.3f, 0.7f, 0.18f};
        Rig r = base, free = base;
        TwoBoneOptions3D o = pole;
        o.bend_swing_degrees_per_second = 360.0f;
        solve_two_bone(r.sk, r.pose, r.world, left_arm, root + Vec3f{-0.3f, 0.7f, -0.18f}, o, &state, 1.0f / 60.0f);
        for (int i = 0; i < 60; ++i) {
            r = base;
            solve_two_bone(r.sk, r.pose, r.world, left_arm, end_target, o, &state, 1.0f / 60.0f);
        }
        solve_two_bone(free.sk, free.pose, free.world, left_arm, end_target, pole);
        assert(close(r.at(biped::elbow_l), free.at(biped::elbow_l), 1e-3f));
    }
    // dt of 0 holds the side it was on (a second solve in the frame is the first's).
    {
        TwoBoneState3D state;
        TwoBoneOptions3D o = pole;
        o.bend_swing_degrees_per_second = 90.0f;
        Rig a = base, b = base;
        const Vec3f t1 = root + Vec3f{0.2f, 0.6f, 0.3f}, t2 = root + Vec3f{-0.2f, 0.6f, -0.3f};
        solve_two_bone(a.sk, a.pose, a.world, left_arm, t1, o, &state, 1.0f / 60.0f);
        Rig c = base;
        solve_two_bone(c.sk, c.pose, c.world, left_arm, t2, o, &state, 1.0f / 60.0f);
        Rig d = base;
        solve_two_bone(d.sk, d.pose, d.world, left_arm, t2, o, &state, 0.0f);
        assert(close(c.at(biped::elbow_l), d.at(biped::elbow_l), 1e-4f));
        (void)b;
    }

    // The fold's centre: equal bones, a target passing through the root. The joints are in the same places on both
    // sides (the pose is continuous); the upper bone's twist about its own axis goes 180 degrees in one step AT the
    // centre, and for a target that passes at a distance d from it, over the steps within about d of it. Worst step of
    // the root's rotation for passes at 0, 2 mm and 20 mm, 2,000 steps over 1.6 m.
    {
        Rig equal = base;
        equal.pose.joints[biped::hand_l].translation = {0.5f, 0, 0};
        equal.refresh();
        const Vec3f line = normalize({0.4f, 0.1f, 0.5f});
        const Vec3f offset = normalize(cross(line, Vec3f{0, 1, 0}));
        float worst_for[3] = {};
        int k = 0;
        for (const float distance : {0.0f, 0.002f, 0.02f}) {
            Rig previous;
            float worst = 0.0f, move = 0.0f;
            for (int i = 0; i <= 2000; ++i) {
                Rig r = equal;
                const Vec3f target =
                    root + line * ((static_cast<float>(i) / 2000.0f) * 1.6f - 0.8f) + offset * distance;
                solve_two_bone(r.sk, r.pose, r.world, left_arm, target, pole);
                if (i > 0) {
                    worst = std::max(worst, angle_degrees(previous.pose.joints[biped::shoulder_l].rotation,
                                                          r.pose.joints[biped::shoulder_l].rotation));
                    move = std::max(move, length(r.at(biped::elbow_l) - previous.at(biped::elbow_l)));
                }
                previous = std::move(r);
            }
            std::cout << "  ik fold centre, pass at " << distance * 1000.0f << " mm: worst step of the shoulder "
                      << worst << " degrees, elbow moves at most " << move << " a step" << std::endl;
            worst_for[k++] = worst;
        }
        // Through it exactly: the half turn in one step. A pass 2 cm off turns the shoulder over some steps, each of
        // them well under that.
        assert(worst_for[0] > 90.0f);
        assert(worst_for[2] < 20.0f);
    }
}

// Item 2: FABRIK with a pole (CPU) and as an ECS constraint kind.
void test_ik_fabrik_pole() {
    const std::array<i32, 5> spine = {biped::pelvis, biped::spine, biped::chest, biped::neck, biped::head};
    Rig base;
    base.pose.joints[biped::spine].rotation = Quatf::axis_angle({1, 0, 0}, 20.0f);
    base.pose.joints[biped::chest].rotation = Quatf::axis_angle({0, 0, 1}, 25.0f);
    base.refresh();
    const Vec3f root = base.at(biped::pelvis);
    float reach = 0.0f;
    for (std::size_t i = 0; i + 1 < spine.size(); ++i) {
        reach += base.bone(spine[i], spine[i + 1]);
    }
    const Vec3f target = root + Vec3f{0.35f, 0.5f, 0.1f};
    const auto bulge_of = [&](const Rig& r) {
        Vec3f sum{};
        for (std::size_t i = 1; i + 1 < spine.size(); ++i) {
            sum += side_of(r.at(spine[i]), root, r.at(spine.back()));
        }
        return sum;
    };
    // Without a pole: the old solve, bit for bit (the options' new fields are off).
    Rig plain = base;
    solve_chain_fabrik(plain.sk, plain.pose, plain.world, spine, target);
    for (const Vec3f pole : {Vec3f{0, 0, 1}, Vec3f{0, 0, -1}, Vec3f{-1, 0, 0}}) {
        Rig r = base;
        FabrikOptions3D o;
        o.pole_mode = PoleMode3D::Direction;
        o.pole = pole;
        const FabrikResult3D res = solve_chain_fabrik(r.sk, r.pose, r.world, spine, target, o);
        assert(res.solved && res.reached && close(r.at(biped::head), target, 1e-3f) &&
               close(r.at(biped::pelvis), root, 1e-5f));
        for (std::size_t k = 0; k + 1 < spine.size(); ++k) {
            assert(close(r.bone(spine[k], spine[k + 1]), base.bone(spine[k], spine[k + 1]), 1e-4f));
        }
        // The bulge faces the pole: its across-the-axis part has a positive component along the pole's across part.
        const Vec3f axis = normalize(r.at(biped::head) - root);
        const Vec3f pole_across = pole - axis * dot(pole, axis);
        assert(dot(normalize(bulge_of(r)), normalize(pole_across)) > 0.99f);
        std::vector<Mat4f> again;
        compute_world_matrices(r.sk, r.pose, again);
        assert(max_difference(again, r.world) < 1e-4f);
    }
    // A pole as a point equals the direction to it from the first joint.
    {
        Rig a = base, b = base;
        FabrikOptions3D o;
        o.pole_mode = PoleMode3D::Direction;
        o.pole = normalize(Vec3f{-1, 0.2f, 0.5f});
        solve_chain_fabrik(a.sk, a.pose, a.world, spine, target, o);
        o.pole_mode = PoleMode3D::Position;
        o.pole = root + Vec3f{-1, 0.2f, 0.5f} * 3.0f;
        solve_chain_fabrik(b.sk, b.pose, b.world, spine, target, o);
        assert(close(a.at(biped::chest), b.at(biped::chest), 1e-4f));
    }
    // Continuity: the pole turning all the way round the axis (360 steps): the chain turns with it, a step at most the
    // pole's own plus a little; a pole along the axis fades out (no jump) rather than flips.
    {
        Rig previous;
        float worst = 0.0f;
        for (int i = 0; i <= 720; ++i) {
            Rig r = base;
            const float a = static_cast<float>(i) * 0.5f * 3.14159265f / 180.0f;
            FabrikOptions3D o;
            o.pole_mode = PoleMode3D::Direction;
            o.pole = {std::sin(a), 0.1f, std::cos(a)};
            solve_chain_fabrik(r.sk, r.pose, r.world, spine, target, o);
            if (i > 0) {
                worst =
                    std::max(worst, angle_degrees(side_of(r.at(biped::chest), root, r.at(biped::head)),
                                                  side_of(previous.at(biped::chest), root, previous.at(biped::head))));
            }
            previous = std::move(r);
        }
        std::cout << "  ik fabrik pole round the axis in 0.5 degree steps: worst step of the bulge " << worst
                  << " degrees" << std::endl;
        assert(worst < 3.0f);
    }
    // Weight 0 leaves it, bit for bit.
    {
        Rig r = base;
        FabrikOptions3D o;
        o.weight = 0.0f;
        o.pole_mode = PoleMode3D::Direction;
        o.pole = {0, 0, 1};
        assert(!solve_chain_fabrik(r.sk, r.pose, r.world, spine, target, o).solved);
        assert(std::memcmp(r.pose.joints.data(), base.pose.joints.data(),
                           base.pose.joints.size() * sizeof(JointPose3D)) == 0);
    }
}

// Item 3: a foot planted in the world stays there while the body moves over it, lets go at the threshold and eases
// back; the toe turns to its own ground; the ground query is called once for a leg (and once for a toe).
void test_ik_foot_locking_and_toes() {
    const Skeleton3D toes = skin_fixtures::make_biped_toes();
    const Rig base{toes};
    const std::array<FootLeg3D, 2> legs = {
        FootLeg3D{{biped::hip_l, biped::knee_l, biped::ankle_l}, 0.05f, biped::toe_l, 0.0f},
        FootLeg3D{{biped::hip_r, biped::knee_r, biped::ankle_r}, 0.05f, biped::toe_r, 0.0f}};
    TwoBoneOptions3D knee;
    knee.pole_mode = PoleMode3D::Direction;
    knee.pole = {0, 0, 1};
    knee.max_stretch = 1.3f;
    const float dt = 1.0f / 60.0f;

    // Locking: the body (model_to_world) moves 1 cm a frame along +Z over feet that stand on the ground in the
    // animation.
    const auto run = [&](bool lock, std::vector<Vec3f>* ankle_world, std::vector<bool>* locked) {
        FootPlacementOptions3D o;
        o.smooth_seconds = 0.0f;
        o.lock_feet = lock;
        FootPlacementState3D state;
        FakeGround ground{[](float, float) { return 0.0f; }};
        for (int i = 0; i < 60; ++i) {
            Rig r = base;
            const Mat4f to_world = Mat4f::translation({0.0f, 0.0f, 0.01f * static_cast<float>(i)});
            const FootPlan3D plan =
                plan_foot_placement(r.sk, r.pose, r.world, legs, ground.query(), o, &state, dt, &to_world);
            apply_foot_placement(r.sk, r.pose, r.world, biped::pelvis, legs, plan, knee);
            ankle_world->push_back(to_world.apply(r.at(biped::ankle_l)));
            locked->push_back(plan.feet[0].locked);
        }
    };
    std::vector<Vec3f> held, free_run;
    std::vector<bool> held_flags, free_flags;
    run(true, &held, &held_flags);
    run(false, &free_run, &free_flags);
    // Unlocked: the ankle goes with the body, 1 cm a frame.
    assert(close(free_run[59].z - free_run[0].z, 0.59f, 1e-4f));
    // Locked: the ankle stays at its place in the world until the animated foot is 15 cm from it (frame 15), then eases
    // back over 0.15 s to the animated ankle (which has gone on), by steps no bigger than the smoothstep's (1.5 x 15 cm
    // / 9 frames = 2.5 cm) and the body's own 1 cm.
    for (int i = 1; i < 15; ++i) {
        assert(held_flags[static_cast<std::size_t>(i)] && close(held[static_cast<std::size_t>(i)], held[0], 2e-4f));
    }
    float worst = 0.0f;
    for (int i = 1; i < 60; ++i) {
        worst = std::max(worst, length(held[static_cast<std::size_t>(i)] - held[static_cast<std::size_t>(i - 1)]));
    }
    std::cout << "  ik foot lock: held to " << length(held[14] - held[0])
              << " m over 14 frames, worst step of the ankle in the world " << worst
              << " m (the body moves 0.01 a frame)" << std::endl;
    assert(worst < 0.04f);
    // After the release the foot catches up and plants again: later frames hold still again.
    int still = 0;
    for (int i = 26; i < 39; ++i) {
        still += length(held[static_cast<std::size_t>(i + 1)] - held[static_cast<std::size_t>(i)]) < 1e-4f ? 1 : 0;
    }
    assert(still >= 12);
    // The held foot is not a different foot: bones keep their lengths and the matrices are the pose's own.
    {
        FootPlacementOptions3D o;
        o.smooth_seconds = 0.0f;
        o.lock_feet = true;
        FootPlacementState3D state;
        FakeGround ground{[](float, float) { return 0.0f; }};
        Rig r = base;
        const Mat4f first = Mat4f::translation({0, 0, 0});
        FootPlan3D plan = plan_foot_placement(r.sk, r.pose, r.world, legs, ground.query(), o, &state, dt, &first);
        Rig r2 = base;
        const Mat4f later = Mat4f::translation({0, 0, 0.08f});
        plan = plan_foot_placement(r2.sk, r2.pose, r2.world, legs, ground.query(), o, &state, dt, &later);
        apply_foot_placement(r2.sk, r2.pose, r2.world, biped::pelvis, legs, plan, knee);
        assert(close(later.apply(r2.at(biped::ankle_l)), {0.1f, 0.05f, 0.0f}, 3e-4f));
        std::vector<Mat4f> again;
        compute_world_matrices(r2.sk, r2.pose, again);
        assert(max_difference(again, r2.world) < 1e-4f);
    }
    // A foot that lifts in the animation lets go at once (the hip swings the foot forward and up); weight of the lock
    // is easing.
    {
        FootPlacementOptions3D o;
        o.smooth_seconds = 0.0f;
        o.lock_feet = true;
        FootPlacementState3D state;
        FakeGround ground{[](float, float) { return 0.0f; }};
        const Mat4f id{};
        Rig r = base;
        FootPlan3D plan = plan_foot_placement(r.sk, r.pose, r.world, legs, ground.query(), o, &state, dt, &id);
        assert(plan.feet[0].locked && state.locks[0].planted);
        Rig lifted = base;
        lifted.pose.joints[biped::hip_l].rotation = Quatf::axis_angle({1, 0, 0}, -40.0f);
        lifted.refresh();
        plan = plan_foot_placement(lifted.sk, lifted.pose, lifted.world, legs, ground.query(), o, &state, dt, &id);
        assert(!state.locks[0].planted); // let go...
        const FootTarget3D& first = plan.feet[0];
        assert(first.locked); // ...and easing, not snapped
        for (int i = 0; i < 12; ++i) {
            plan = plan_foot_placement(lifted.sk, lifted.pose, lifted.world, legs, ground.query(), o, &state, dt, &id);
        }
        assert(!plan.feet[0].locked && close(plan.feet[0].target, lifted.at(biped::ankle_l), 1e-4f));
    }

    // Ground query counts: once a leg; a toe is one more.
    {
        FakeGround ground{[](float, float) { return 0.0f; }};
        Rig r = base;
        FootPlacementOptions3D o;
        plan_foot_placement(r.sk, r.pose, r.world, legs, ground.query(), o);
        assert(ground.casts == 4); // two legs, each with a toe
        ground.casts = 0;
        const std::array<FootLeg3D, 2> no_toes = {FootLeg3D{{biped::hip_l, biped::knee_l, biped::ankle_l}, 0.05f},
                                                  FootLeg3D{{biped::hip_r, biped::knee_r, biped::ankle_r}, 0.05f}};
        plan_foot_placement(r.sk, r.pose, r.world, no_toes, ground.query(), o);
        assert(ground.casts == 2);
    }

    // Toes: ground rising at 20 degrees from z = 0.1 (under the toe, 0.15 forward of the ankle; the ankle's own ground
    // is flat). The toe turns to the slope's normal; clamped by toe_max_tilt_degrees and by the foot's slope clamp;
    // none while the foot is high.
    {
        const auto ground_with_slope = [](float degrees, int* casts) {
            return [degrees, casts](Vec3f from, Vec3f to) -> std::optional<GroundHit3D> {
                ++*casts;
                const float rad = degrees * 3.14159265f / 180.0f;
                const auto height = [&](float z) { return z > 0.1f ? (z - 0.1f) * std::tan(rad) : 0.0f; };
                const Vec3f n = from.z > 0.1f ? Vec3f{0, std::cos(rad), -std::sin(rad)} : Vec3f{0, 1, 0};
                const float h = height(from.z);
                if (from.y < h || to.y > h) {
                    return std::nullopt;
                }
                return GroundHit3D{{from.x, h, from.z}, n};
            };
        };
        for (const auto& [slope, toe_max, expect] :
             {std::tuple{20.0f, 40.0f, 20.0f}, std::tuple{50.0f, 40.0f, 30.0f}, std::tuple{20.0f, 8.0f, 8.0f}}) {
            int casts = 0;
            Rig r = base;
            FootPlacementOptions3D o;
            o.smooth_seconds = 0.0f;
            o.toe_max_tilt_degrees = toe_max;
            const FootPlan3D plan =
                plan_foot_placement(r.sk, r.pose, r.world, legs, ground_with_slope(slope, &casts), o);
            assert(casts == 4 && plan.feet[0].toe_grounded && plan.feet[0].grounded);
            apply_foot_placement(r.sk, r.pose, r.world, biped::pelvis, legs, plan, knee);
            const Vec3f toe_up = normalize(r.world[biped::toe_l].apply_vector({0, 1, 0}));
            const float tilt = angle_degrees(toe_up, {0, 1, 0});
            assert(close(tilt, expect, 0.2f));
            // The ankle was on flat ground: the foot itself is level.
            assert(angle_degrees(normalize(r.world[biped::ankle_l].apply_vector({0, 1, 0})), {0, 1, 0}) < 0.1f);
            // Which way: the toe's up leans back (toward -Z) on a slope that rises ahead.
            assert(toe_up.z < 0.0f);
        }
        // A foot in the air (the hip swung forward and up) does not follow the ground under its toe: the toe is turned
        // with the foot only.
        {
            Rig lifted = base;
            lifted.pose.joints[biped::hip_l].rotation = Quatf::axis_angle({1, 0, 0}, -40.0f);
            lifted.refresh();
            FootPlacementOptions3D o;
            o.smooth_seconds = 0.0f;
            // A ramp that is under the lifted toe too (the ground rises everywhere ahead of z = -1): the toe would tilt
            // on it if it were on the ground.
            const auto tilted_everywhere = [&](Vec3f from, Vec3f to) -> std::optional<GroundHit3D> {
                const float t = std::tan(25.0f * 3.14159265f / 180.0f);
                const float h = (from.z + 1.0f) * t;
                if (from.y < h || to.y > h) {
                    return std::nullopt;
                }
                return GroundHit3D{{from.x, h, from.z}, normalize(Vec3f{0, 1, -t})};
            };
            const FootPlan3D plan =
                plan_foot_placement(lifted.sk, lifted.pose, lifted.world, legs, tilted_everywhere, o);
            Rig placed = lifted;
            apply_foot_placement(placed.sk, placed.pose, placed.world, biped::pelvis, legs, plan, knee);
            // The toe's up against the ankle's up: the same (the toe's own animated turn was none), whatever the ground
            // there says.
            const Vec3f toe_up = normalize(placed.world[biped::toe_l].apply_vector({0, 1, 0}));
            const Vec3f ankle_up = normalize(placed.world[biped::ankle_l].apply_vector({0, 1, 0}));
            assert(plan.feet[0].grounded && plan.feet[0].toe_grounded);
            assert(angle_degrees(toe_up, ankle_up) < 0.5f);
        }
        // No toe joint named: the foot is as it was.
        int casts = 0;
        Rig r = base;
        FootPlacementOptions3D o;
        o.smooth_seconds = 0.0f;
        const std::array<FootLeg3D, 2> no_toes = {FootLeg3D{{biped::hip_l, biped::knee_l, biped::ankle_l}, 0.05f},
                                                  FootLeg3D{{biped::hip_r, biped::knee_r, biped::ankle_r}, 0.05f}};
        const FootPlan3D plan =
            plan_foot_placement(r.sk, r.pose, r.world, no_toes, ground_with_slope(20.0f, &casts), o);
        assert(casts == 2 && !plan.feet[0].toe_grounded);
        // Weight 0 of the leg options leaves the toe too.
        TwoBoneOptions3D zero = knee;
        zero.weight = 0.5f;
        Rig half = base, full = base;
        int c2 = 0;
        const FootPlan3D plan2 =
            plan_foot_placement(half.sk, half.pose, half.world, legs, ground_with_slope(20.0f, &c2), o);
        apply_foot_placement(half.sk, half.pose, half.world, biped::pelvis, legs, plan2, zero);
        apply_foot_placement(full.sk, full.pose, full.world, biped::pelvis, legs, plan2, knee);
        const float whole = angle_degrees(normalize(full.world[biped::toe_l].apply_vector({0, 1, 0})), {0, 1, 0});
        const float part = angle_degrees(normalize(half.world[biped::toe_l].apply_vector({0, 1, 0})), {0, 1, 0});
        assert(part > 1.0f && part < whole - 1.0f);
    }
}

// The ECS: a FABRIK constraint (reach, pole, fade), the ground query once a frame per leg however many solves, foot
// locking in the world, the limits through an IKConstraint3D, bend-swing state.
void test_ecs_ik_fabrik_locking_and_limits() {
    // A model with toes.
    struct FakeMesh final : IMeshBackend3D {};
    auto model = std::make_shared<Model3D>();
    model->skeletons.push_back(skin_fixtures::make_biped_toes());
    const Skeleton3D& sk = model->skeletons[0];
    AnimClip3D idle;
    idle.name = "idle";
    idle.duration = 1.0f;
    idle.channels.resize(sk.joint_count());
    model->clips.push_back(idle);
    ModelPart3D part;
    part.name = "body";
    part.mesh = Mesh3D{std::make_shared<FakeMesh>(), 3, 3, {}, {}, {}, sk.joint_count()};
    part.skin = 0;
    model->parts.push_back(part);
    std::vector<Mat4f> rest_world;
    compute_world_matrices(sk, sk.rest_pose(), rest_world);
    const auto joint_at = [&](const Animator3D& a, int j, const Mat4f& entity) {
        const Mat4f& m = a.ik_world[static_cast<std::size_t>(j)];
        return entity.apply({m.m[12], m.m[13], m.m[14]});
    };
    const float dt = 1.0f / 60.0f;

    // FABRIK: the head of the spine chain reaches a world target; the pole turns the bulge; the fade takes its time; 0
    // lets go.
    {
        kin::EcsWorld world;
        WorldRenderState3D state{world};
        const Transform3D placed{.position = {1, 0, 2}, .rotation = Quatf::axis_angle({0, 1, 0}, 40.0f)};
        const flecs::entity e =
            world.raw().entity().set(placed).set(SkinnedMeshRenderer3D{.model = model}).set(Animator3D{.clip = "idle"});
        IKConstraint3D k;
        k.name = "spine";
        k.kind = IKKind3D::Fabrik;
        k.joints = {"pelvis", "spine", "chest", "neck", "head"};
        k.fade_seconds = 0.5f;
        e.get_mut<Animator3D>()->add_ik(k);
        const Mat4f m = placed.matrix();
        const Vec3f pelvis = m.apply({0, 1.0f, 0});
        const Vec3f target = pelvis + m.apply_vector({0.35f, 0.5f, 0.1f});
        const auto frame = [&](float weight, Vec3f pole) {
            Animator3D& a = *e.get_mut<Animator3D>();
            a.ik("spine").set_target(target).set_weight(weight).set_pole_direction(pole);
            state.animate(dt);
            Scene3D scene;
            state.propagate_transforms();
            state.collect(scene);
            return e.get<Animator3D>();
        };
        // Half way through the fade: partly there.
        const Animator3D* a = nullptr;
        for (int i = 0; i < 15; ++i) {
            a = frame(1.0f, {0, 0, 1});
        }
        const float partway = length(joint_at(*a, biped::head, m) - target);
        for (int i = 0; i < 40; ++i) {
            a = frame(1.0f, {0, 0, 1});
        }
        const float arrived = length(joint_at(*a, biped::head, m) - target);
        assert(partway > 0.05f && arrived < 2e-3f);
        // The pole in the model's space: the bulge (the chest off the line pelvis-head) faces it.
        const Vec3f bulge_front =
            side_of(joint_at(*a, biped::chest, m), joint_at(*a, biped::pelvis, m), joint_at(*a, biped::head, m));
        for (int i = 0; i < 40; ++i) {
            a = frame(1.0f, {0, 0, -1});
        }
        const Vec3f bulge_back =
            side_of(joint_at(*a, biped::chest, m), joint_at(*a, biped::pelvis, m), joint_at(*a, biped::head, m));
        assert(length(joint_at(*a, biped::head, m) - target) < 2e-3f);
        assert(dot(normalize(bulge_front), normalize(m.apply_vector({0, 0, 1}))) > 0.5f);
        assert(dot(normalize(bulge_back), normalize(m.apply_vector({0, 0, 1}))) < -0.5f);
        // Let go: the weight falls over the fade to the clips' pose (the spine straight).
        for (int i = 0; i < 60; ++i) {
            a = frame(0.0f, {0, 0, 1});
        }
        assert(close(joint_at(*a, biped::head, m), m.apply(rest_world[biped::head].apply({0, 0, 0})), 1e-4f));
    }

    // The ground query: once a frame per leg (and toe), however often the constraints are solved in it (apply_ik, then
    // collect).
    {
        kin::EcsWorld world;
        WorldRenderState3D state{world};
        const flecs::entity e = world.raw()
                                    .entity()
                                    .set(Transform3D{})
                                    .set(SkinnedMeshRenderer3D{.model = model})
                                    .set(Animator3D{.clip = "idle"});
        IKConstraint3D feet;
        feet.name = "feet";
        feet.kind = IKKind3D::FootPlacer;
        feet.joints = {"pelvis", "hip_L", "knee_L", "ankle_L", "hip_R", "knee_R", "ankle_R"};
        feet.foot_heights = {0.05f, 0.05f};
        feet.pole = IKConstraint3D::Pole::ModelDirection;
        feet.pole_vector = {0, 0, 1};
        int casts = 0;
        feet.ground = [&](Vec3f from, Vec3f to) -> std::optional<GroundHit3D> {
            ++casts;
            const float t = (from.y - 0.0f) / std::max(from.y - to.y, 1e-6f);
            if (t < 0.0f || t > 1.0f) {
                return std::nullopt;
            }
            return GroundHit3D{{from.x, 0.0f, from.z}, {0, 1, 0}};
        };
        e.get_mut<Animator3D>()->add_ik(feet);
        for (int frame = 1; frame <= 3; ++frame) {
            state.animate(dt);
            state.propagate_transforms();
            state.apply_ik(); // the game reads a joint early...
            Scene3D a, b;
            state.collect(a); // ...and draws, and a second view draws
            state.collect(b);
            assert(casts == 2 * frame);
        }
        // The same with a toe on each leg: two more.
        casts = 0;
        e.get_mut<Animator3D>()->ik("feet").toe_joints = {"toe_L", "toe_R"};
        e.get_mut<Animator3D>()->ik("feet").toe_heights = {0.0f, 0.0f};
        e.get_mut<Animator3D>()->ik("feet").resolved_for =
            nullptr; // (a game sets toes when it builds the constraint; re-bind here)
        state.animate(dt);
        state.propagate_transforms();
        state.apply_ik();
        Scene3D s;
        state.collect(s);
        assert(casts == 4);
        // The ground moved between two solves in one frame (a different segment): asked again, not served from the
        // frame's answers.
        casts = 0;
        state.animate(dt);
        state.propagate_transforms();
        state.apply_ik();
        e.get_mut<Transform3D>()->position = {0.5f, 0, 0};
        state.propagate_transforms(); // a new frame's epoch, as it would be
        state.collect(s);
        assert(casts == 8);
    }

    // Foot locking in the world: the entity walks along z at 1 m/s; the planted foot stays in the world; unlocked it
    // goes along.
    for (const bool lock : {true, false}) {
        kin::EcsWorld world;
        WorldRenderState3D state{world};
        const flecs::entity e = world.raw()
                                    .entity()
                                    .set(Transform3D{})
                                    .set(SkinnedMeshRenderer3D{.model = model})
                                    .set(Animator3D{.clip = "idle"});
        IKConstraint3D feet;
        feet.name = "feet";
        feet.kind = IKKind3D::FootPlacer;
        feet.joints = {"pelvis", "hip_L", "knee_L", "ankle_L", "hip_R", "knee_R", "ankle_R"};
        feet.foot_heights = {0.05f, 0.05f};
        feet.pole = IKConstraint3D::Pole::ModelDirection;
        feet.pole_vector = {0, 0, 1};
        feet.two_bone.max_stretch = 1.3f;
        feet.foot.smooth_seconds = 0.0f;
        feet.foot.lock_feet = lock;
        feet.ground = [](Vec3f from, Vec3f to) -> std::optional<GroundHit3D> {
            if (from.y < 0.0f || to.y > 0.0f) {
                return std::nullopt;
            }
            return GroundHit3D{{from.x, 0.0f, from.z}, {0, 1, 0}};
        };
        e.get_mut<Animator3D>()->add_ik(feet);
        Vec3f first{};
        float drift = 0.0f;
        for (int i = 0; i < 12; ++i) {
            e.get_mut<Transform3D>()->position = {0, 0, 0.01f * static_cast<float>(i)};
            state.animate(dt);
            state.propagate_transforms();
            Scene3D scene;
            state.collect(scene);
            const Animator3D& a = *e.get<Animator3D>();
            const Vec3f ankle = joint_at(a, biped::ankle_l, e.get<Transform3D>()->matrix());
            if (i == 0) {
                first = ankle;
            }
            drift = std::max(drift, length(ankle - first));
        }
        std::cout << "  ik foot lock in the ECS, " << (lock ? "locked" : "unlocked") << ": the ankle drifts " << drift
                  << " m in the world over 11 cm of walking" << std::endl;
        assert(lock ? drift < 1e-3f : drift > 0.1f);
    }

    // Limits through the constraint: a maximum bend on an arm; a knee hinge and twist limit on the foot placer's legs
    // (the knee does not go behind however the pole asks).
    {
        kin::EcsWorld world;
        WorldRenderState3D state{world};
        const flecs::entity e = world.raw()
                                    .entity()
                                    .set(Transform3D{})
                                    .set(SkinnedMeshRenderer3D{.model = model})
                                    .set(Animator3D{.clip = "idle"});
        IKConstraint3D hand = hand_constraint();
        hand.two_bone.max_bend_degrees = 60.0f;
        e.get_mut<Animator3D>()->add_ik(hand).set_target(Vec3f{0.35f, 1.62f, 0.05f}).set_pole_direction({0, 0, -1});
        state.animate(dt);
        Scene3D scene;
        state.propagate_transforms();
        state.collect(scene);
        const Animator3D& a = *e.get<Animator3D>();
        const Vec3f sh = joint_at(a, biped::shoulder_l, Mat4f{}), el = joint_at(a, biped::elbow_l, Mat4f{}),
                    ha = joint_at(a, biped::hand_l, Mat4f{});
        const float bend = angle_degrees(el - sh, ha - el);
        assert(bend < 60.05f && length(ha - Vec3f{0.35f, 1.62f, 0.05f}) > 0.05f);
    }
    // The knee through a foot placer, a pole pointing back and the left foot on a 0.2 m step: without limits the knee
    // goes behind (z < the hip's), with a hinge about X and 25 degrees of hip twist it stays in front; the foot is on
    // the step either way.
    for (const bool limited : {false, true}) {
        kin::EcsWorld world;
        WorldRenderState3D state{world};
        const flecs::entity e = world.raw()
                                    .entity()
                                    .set(Transform3D{})
                                    .set(SkinnedMeshRenderer3D{.model = model})
                                    .set(Animator3D{.clip = "idle"});
        IKConstraint3D feet;
        feet.name = "feet";
        feet.kind = IKKind3D::FootPlacer;
        feet.joints = {"pelvis", "hip_L", "knee_L", "ankle_L", "hip_R", "knee_R", "ankle_R"};
        feet.foot_heights = {0.05f, 0.05f};
        feet.pole = IKConstraint3D::Pole::ModelDirection;
        feet.pole_vector = {0, 0, -1};
        feet.foot.smooth_seconds = 0.0f;
        if (limited) {
            feet.two_bone.hinge = true;
            feet.two_bone.hinge_axis = {1, 0, 0};
            feet.two_bone.max_twist_degrees = 25.0f;
            feet.two_bone.max_bend_degrees = 150.0f;
        }
        feet.ground = [](Vec3f from, Vec3f to) -> std::optional<GroundHit3D> {
            const float h = from.x > 0.0f ? 0.2f : 0.0f;
            if (from.y < h || to.y > h) {
                return std::nullopt;
            }
            return GroundHit3D{{from.x, h, from.z}, {0, 1, 0}};
        };
        e.get_mut<Animator3D>()->add_ik(feet);
        state.animate(dt);
        state.propagate_transforms();
        state.apply_ik();
        const Animator3D& a = *e.get<Animator3D>();
        const Vec3f hip = joint_at(a, biped::hip_l, Mat4f{}), knee = joint_at(a, biped::knee_l, Mat4f{}),
                    ankle = joint_at(a, biped::ankle_l, Mat4f{});
        assert(close(ankle.y, 0.25f, 1e-3f));
        const float side = side_of(knee, hip, ankle).z;
        std::cout << "  ik knee through the foot placer, pole back, "
                  << (limited ? "hinge + 25 degrees of hip twist" : "no limits") << ": knee " << side
                  << " m in front of the line hip-ankle" << std::endl;
        assert(limited ? side > 0.02f : side < -0.02f);
    }
    // The elbow's side swings at the constraint's bounded speed: the state is kept by the constraint (a target through
    // the pole's axis, 40 frames).
    for (const float swing : {0.0f, 360.0f}) {
        kin::EcsWorld world;
        WorldRenderState3D state{world};
        const flecs::entity e = world.raw()
                                    .entity()
                                    .set(Transform3D{})
                                    .set(SkinnedMeshRenderer3D{.model = model})
                                    .set(Animator3D{.clip = "idle"});
        IKConstraint3D hand = hand_constraint();
        hand.two_bone.bend_swing_degrees_per_second = swing;
        hand.set_pole_direction({0, 1, 0});
        e.get_mut<Animator3D>()->add_ik(hand);
        const Vec3f shoulder{rest_world[biped::shoulder_l].m[12], rest_world[biped::shoulder_l].m[13],
                             rest_world[biped::shoulder_l].m[14]};
        const Vec3f across = normalize(Vec3f{1.0f, 0.0f, 0.6f});
        Vec3f previous_side{};
        float worst = 0.0f;
        for (int i = 0; i <= 40; ++i) {
            const Vec3f target = shoulder + Vec3f{0, 0.7f, 0} + across * (static_cast<float>(i) / 40.0f * 0.6f - 0.3f);
            e.get_mut<Animator3D>()->ik("hand").set_target(target);
            state.animate(dt);
            state.propagate_transforms();
            state.apply_ik();
            const Animator3D& a = *e.get<Animator3D>();
            const Vec3f side = side_of(joint_at(a, biped::elbow_l, Mat4f{}), joint_at(a, biped::shoulder_l, Mat4f{}),
                                       joint_at(a, biped::hand_l, Mat4f{}));
            assert(length(joint_at(a, biped::hand_l, Mat4f{}) - target) < 3e-4f);
            if (i > 0) {
                worst = std::max(worst, angle_degrees(side, previous_side));
            }
            previous_side = side;
        }
        std::cout << "  ik bend swing in the ECS, " << swing << " degrees a second: worst step of the elbow's side "
                  << worst << " degrees" << std::endl;
        assert(swing == 0.0f ? worst > 15.0f : worst < 6.5f);
    }
}

// Item 7: Sponza Walk's IK panel logic with no window: Settings to the person's constraints, the panel's fields to what
// they do. (What stays untested here: the control table's widgets, the pictures, the marker and step meshes, the
// keyboard.)
void test_sponza_walk_ik_panel() {
    using namespace sponza_walk;
    const PersonRig rig = person_rig();
    assert(rig.skeleton.joint_count() == 25 && rig.skeleton.find("toe_L") == 18 && rig.skeleton.find("tail_5") == 24);
    // Every joint a constraint names exists.
    auto model = std::make_shared<Model3D>();
    model->skeletons.push_back(rig.skeleton);
    model->clips = rig.clips;
    const GroundQuery3D floor_only = [](Vec3f from, Vec3f to) { return floor_and_step(from, to, false, {}, {}); };
    for (const IKConstraint3D& k : person_constraints(floor_only)) {
        for (const std::string& n : k.joints) {
            assert(rig.skeleton.find(n) >= 0);
        }
        for (const std::string& n : k.toe_joints) {
            assert(rig.skeleton.find(n) >= 0);
        }
    }
    // The world, its render state (which must go first) and the person.
    struct Placed {
        std::unique_ptr<kin::EcsWorld> world;
        std::unique_ptr<WorldRenderState3D> state;
        flecs::entity e;
        ~Placed() {
            state.reset();
            world.reset();
        }
    };
    const auto make = [&](const GroundQuery3D& ground, const Transform3D& at) {
        auto placed = std::make_unique<Placed>();
        placed->world = std::make_unique<kin::EcsWorld>();
        placed->state = std::make_unique<WorldRenderState3D>(*placed->world);
        Animator3D a;
        a.clip = "Sway";
        for (const IKConstraint3D& k : person_constraints(ground)) {
            a.add_ik(k);
        }
        placed->e = placed->world->raw().entity().set(at).set(SkinnedMeshRenderer3D{.model = model}).set(a);
        return placed;
    };
    const float dt = 1.0f / 60.0f;
    const auto settle = [&](WorldRenderState3D& state, flecs::entity e, const PersonIk& panel, const Mat4f& base,
                            int frames) {
        for (int i = 0; i < frames; ++i) {
            drive_person(*e.get_mut<Animator3D>(), panel, base, {0.0f, 1.7f, 4.0f});
            state.animate(dt);
            state.propagate_transforms();
            state.apply_ik();
        }
        return e.get<Animator3D>();
    };
    const Transform3D placed{.position = {3, 0, -2}, .rotation = Quatf::axis_angle({0, 1, 0}, 25.0f)};
    const Mat4f base = placed.matrix();
    const auto at = [&](const Animator3D& a, const char* name) {
        const Mat4f& m = a.ik_world[static_cast<std::size_t>(rig.skeleton.find(name))];
        return base.apply({m.m[12], m.m[13], m.m[14]});
    };

    // The hand: at the panel's target for each pole choice, with the elbow on the side the choice names.
    for (int pole = 1; pole <= 4; ++pole) {
        auto placed_person = make(floor_only, placed);
        WorldRenderState3D* state = placed_person->state.get();
        flecs::entity e = placed_person->e;
        kin::EcsWorld* world = placed_person->world.get();
        (void)world;
        PersonIk panel;
        panel.on = true;
        panel.hand_x = 0.45f;
        panel.hand_y = 0.0f;
        panel.hand_z = 0.5f;
        panel.pole = pole;
        panel.fade = 0.0f;
        const Animator3D* a = settle(*state, e, panel, base, 3);
        const Vec3f target = base.apply(person_hand_target(panel));
        assert(length(at(*a, "hand_L") - target) < 1e-3f);
        const Vec3f side = side_of(at(*a, "elbow_L"), at(*a, "shoulder_L"), at(*a, "hand_L"));
        const Vec3f want = base.apply_vector(person_elbow_pole(pole));
        assert(dot(normalize(side), normalize(want)) > 0.3f);
    }
    // Weight and fade: the weight 0 leaves the clip's hand; a fade takes its time.
    {
        auto placed_person = make(floor_only, placed);
        WorldRenderState3D* state = placed_person->state.get();
        flecs::entity e = placed_person->e;
        kin::EcsWorld* world = placed_person->world.get();
        (void)world;
        PersonIk panel;
        panel.on = true;
        panel.weight = 0.0f;
        panel.fade = 0.5f;
        const Animator3D* a = settle(*state, e, panel, base, 3);
        const Vec3f clip_hand = at(*a, "hand_L");
        assert(length(clip_hand - base.apply(person_hand_target(panel))) > 0.1f);
        panel.weight = 1.0f;
        a = settle(*state, e, panel, base, 15);
        const float partway = length(at(*a, "hand_L") - base.apply(person_hand_target(panel)));
        a = settle(*state, e, panel, base, 40);
        assert(partway > 0.01f && length(at(*a, "hand_L") - base.apply(person_hand_target(panel))) < 1e-3f);
    }
    // The stretch slider: a target beyond the reach is reached to the stretch's factor.
    {
        auto placed_person = make(floor_only, placed);
        WorldRenderState3D* state = placed_person->state.get();
        flecs::entity e = placed_person->e;
        kin::EcsWorld* world = placed_person->world.get();
        (void)world;
        PersonIk panel;
        panel.hand_x = 1.2f;
        panel.hand_y = 0.0f;
        panel.hand_z = 0.0f;
        panel.fade = 0.0f;
        panel.stretch = 1.0f;
        const Animator3D* a = settle(*state, e, panel, base, 3);
        const float reach = length(at(*a, "hand_L") - at(*a, "shoulder_L"));
        panel.stretch = 1.2f;
        a = settle(*state, e, panel, base, 3);
        const float stretched = length(at(*a, "hand_L") - at(*a, "shoulder_L"));
        assert(close(reach, 0.95f, 1e-3f) && stretched > reach * 1.1f && stretched <= reach * 1.2f + 1e-3f);
    }
    // Look: the head's aim turns toward the camera by the weight, not at all with the switch off.
    {
        auto placed_person = make(floor_only, placed);
        WorldRenderState3D* state = placed_person->state.get();
        flecs::entity e = placed_person->e;
        kin::EcsWorld* world = placed_person->world.get();
        (void)world;
        PersonIk panel;
        panel.fade = 0.0f;
        panel.look = false;
        panel.look_smooth = 0.0f;
        const Animator3D* a = settle(*state, e, panel, base, 3);
        const Vec3f nose_off = at(*a, "head");
        const Quatf head_off = rotation_of(a->ik_world[static_cast<std::size_t>(rig.skeleton.find("head"))]);
        panel.look = true;
        panel.look_weight = 1.0f;
        a = settle(*state, e, panel, base, 3);
        const Quatf head_on = rotation_of(a->ik_world[static_cast<std::size_t>(rig.skeleton.find("head"))]);
        assert(angle_degrees(head_off, head_on) > 1.0f);
        (void)nose_off;
    }
    // Feet and the step: the left foot is lifted onto a 0.2 m step when the switch is on, and nothing moves when off.
    {
        const auto ground_of = [&](bool step, float height) {
            return GroundQuery3D{[=](Vec3f from, Vec3f to) -> std::optional<GroundHit3D> {
                const Mat4f inverse = base.inverse();
                const auto hit = floor_and_step(inverse.apply(from), inverse.apply(to), step, {0.0f, 0.0f, -0.4f},
                                                {0.6f, height, 0.4f});
                if (!hit) {
                    return std::nullopt;
                }
                return GroundHit3D{base.apply(hit->position),
                                   normalize(inverse.transposed().apply_vector(hit->normal))};
            }};
        };
        for (const bool on : {false, true}) {
            auto placed_person = make(ground_of(true, 0.2f), placed);
            WorldRenderState3D* state = placed_person->state.get();
            flecs::entity e = placed_person->e;
            kin::EcsWorld* world = placed_person->world.get();
            (void)world;
            PersonIk panel;
            panel.feet = on;
            panel.fade = 0.0f;
            const Animator3D* a = settle(*state, e, panel, base, 60);
            const float pelvis_y = (base.inverse().apply(at(*a, "pelvis"))).y;
            const float left_y = (base.inverse().apply(at(*a, "ankle_L"))).y;
            if (on) {
                // Standing on the floor with the left foot lifted onto the step: the pelvis stays (a raise is off), the
                // left ankle is the step's height up.
                assert(close(pelvis_y, 1.0f, 5e-3f) && close(left_y, 0.25f, 5e-3f));
            } else {
                assert(close(pelvis_y, 1.0f, 1e-4f) && close(left_y, 0.05f, 1e-4f));
            }
        }
    }
    // The tail: FABRIK to the panel's target, off with the switch.
    {
        auto placed_person = make(floor_only, placed);
        WorldRenderState3D* state = placed_person->state.get();
        flecs::entity e = placed_person->e;
        kin::EcsWorld* world = placed_person->world.get();
        (void)world;
        PersonIk panel;
        panel.fade = 0.0f;
        panel.tail = false;
        const Animator3D* a = settle(*state, e, panel, base, 3);
        const Vec3f rest_tip = at(*a, "tail_5");
        assert(close(base.inverse().apply(rest_tip), rig.world[24], 1e-4f));
        panel.tail = true;
        panel.tail_x = 0.4f;
        panel.tail_y = 0.9f;
        panel.tail_z = -0.4f;
        a = settle(*state, e, panel, base, 3);
        assert(length(at(*a, "tail_5") - base.apply(person_tail_target(panel))) < 2e-3f);
        // Bones kept: the tail's five joints are 0.18 apart.
        assert(close(length(at(*a, "tail_3") - at(*a, "tail_2")), 0.18f, 1e-3f));
    }
    // Limits, lock, toes: the knee stays forward with a pole that would push it back (the limits switch makes the foot
    // placer's knees hinges); a locked foot stays while the person walks; the toes tilt on a slope.
    {
        const auto flat = [&](Vec3f from, Vec3f to) -> std::optional<GroundHit3D> {
            const Mat4f inverse = base.inverse();
            const auto hit = floor_and_step(inverse.apply(from), inverse.apply(to), false, {}, {});
            if (!hit) {
                return std::nullopt;
            }
            return GroundHit3D{base.apply(hit->position), base.apply_vector({0, 1, 0})};
        };
        Vec3f first{};
        float drift_locked = 0.0f, drift_free = 0.0f;
        for (const bool lock : {true, false}) {
            auto placed_person = make(flat, placed);
            WorldRenderState3D* state = placed_person->state.get();
            flecs::entity e = placed_person->e;
            kin::EcsWorld* world = placed_person->world.get();
            (void)world;
            PersonIk panel;
            panel.feet = true;
            panel.fade = 0.0f;
            panel.lock = lock;
            panel.toes = true;
            panel.limits = true;
            float drift = 0.0f;
            for (int i = 0; i < 12; ++i) {
                // The person drifts 1 cm a frame along its own x.
                e.get_mut<Transform3D>()->position =
                    placed.position + base.apply_vector({0.01f * static_cast<float>(i), 0, 0});
                drive_person(*e.get_mut<Animator3D>(), panel, base, {0.0f, 1.7f, 4.0f});
                state->animate(dt);
                state->propagate_transforms();
                state->apply_ik();
                const Mat4f moved = e.get<Transform3D>()->matrix();
                const Mat4f& m = e.get<Animator3D>()->ik_world[static_cast<std::size_t>(rig.skeleton.find("ankle_R"))];
                const Vec3f ankle = moved.apply({m.m[12], m.m[13], m.m[14]});
                if (i == 0) {
                    first = ankle;
                }
                drift = std::max(drift, length(ankle - first));
            }
            (lock ? drift_locked : drift_free) = drift;
        }
        std::cout << "  sponza walk panel: right ankle drifts " << drift_locked << " m locked, " << drift_free
                  << " m unlocked, over 11 cm of drift" << std::endl;
        assert(drift_locked < 2e-3f && drift_free > 0.08f);
    }
    // The settings struct maps field for field.
    {
        Settings s;
        s.ik = true;
        s.ik_hand_x = 0.1f;
        s.ik_tail = true;
        s.ik_lock = true;
        s.ik_toes = true;
        s.ik_limits = true;
        s.ik_walk = true;
        s.ik_pole = 3;
        s.ik_step_height = 0.31f;
        const PersonIk p = person_ik(s);
        assert(p.on && p.hand_x == 0.1f && p.tail && p.lock && p.toes && p.limits && p.walk && p.pole == 3 &&
               p.step_height == 0.31f);
        assert(person_walk_offset(0.0f) == 0.0f && std::fabs(person_walk_offset(1.7f)) > 0.1f);
    }
}

// Item 7: CesiumMan (the Khronos sample, from the reference cache when it is there): an arm and a leg reach targets
// made by turning their joints. Its joint names are not a rig's: "Skeleton_arm_joint_R" to "Skeleton_arm_joint_R__3_"
// is the right arm, the leg_joint_R_1, _2, _3 the right leg; found by name, solved in the model's own (Z-up, rotated)
// space.
void test_ik_cesium_man() {
    const char* cache = std::getenv("KIN_REFERENCE_CACHE");
    const std::filesystem::path path =
        (cache ? std::filesystem::path{cache}
               : std::filesystem::path{std::getenv("HOME") ? std::getenv("HOME") : "."} / ".cache/kin/reference") /
        "cesium_man/CesiumMan.glb";
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "ik: CesiumMan is not in the reference cache (tools/reference/fetch.py): its IK test is skipped"
                  << std::endl;
        return;
    }
    const std::string bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    ModelData3D data;
    std::string error;
    if (!kin::test::read_value3d(kin::read_model_data(bytes, path), data, error)) {
        std::cerr << "ik: " << error << "" << std::endl;
        assert(false);
    }
    assert(data.skeletons.size() == 1 && !data.clips.empty());
    Skeleton3D sk = data.skeletons[0];
    struct Limb {
        const char* name;
        const char* names[3];
    };
    const Limb limbs[] = {
        {"right arm", {"Skeleton_arm_joint_R", "Skeleton_arm_joint_R__2_", "Skeleton_arm_joint_R__3_"}},
        {"left arm", {"Skeleton_arm_joint_L__4_", "Skeleton_arm_joint_L__3_", "Skeleton_arm_joint_L__2_"}},
        {"right leg", {"leg_joint_R_1", "leg_joint_R_2", "leg_joint_R_3"}},
        {"left leg", {"leg_joint_L_1", "leg_joint_L_2", "leg_joint_L_3"}},
    };
    const AnimClip3D& clip = data.clips[0];
    int total = 0;
    for (const Limb& limb : limbs) {
        const TwoBoneChain3D chain{sk.find(limb.names[0]), sk.find(limb.names[1]), sk.find(limb.names[2])};
        assert(chain.root >= 0 && chain.mid >= 0 && chain.end >= 0);
        float worst = 0.0f, worst_len = 0.0f;
        int count = 0;
        Lcg rng;
        for (const float time : {0.0f, 0.3f, 0.7f, 1.1f}) {
            Pose3D pose = sk.rest_pose();
            sample_clip(clip, sk, time, true, pose);
            std::vector<Mat4f> world;
            compute_world_matrices(sk, pose, world);
            const auto point = [&](const std::vector<Mat4f>& w, i32 j) {
                return Vec3f{w[static_cast<std::size_t>(j)].m[12], w[static_cast<std::size_t>(j)].m[13],
                             w[static_cast<std::size_t>(j)].m[14]};
            };
            const float l1 = length(point(world, chain.mid) - point(world, chain.root)),
                        l2 = length(point(world, chain.end) - point(world, chain.mid));
            for (int i = 0; i < 25; ++i) {
                // A reachable target: this pose's hand with the shoulder and elbow turned a little (the pose itself, by
                // FK).
                Pose3D goal = pose;
                goal.joints[static_cast<std::size_t>(chain.root)].rotation =
                    (Quatf::axis_angle(rng.direction(), 35.0f * (rng.next() * 0.5f + 0.5f)) *
                     goal.joints[static_cast<std::size_t>(chain.root)].rotation)
                        .normalized();
                goal.joints[static_cast<std::size_t>(chain.mid)].rotation =
                    (Quatf::axis_angle(rng.direction(), 30.0f * (rng.next() * 0.5f + 0.5f)) *
                     goal.joints[static_cast<std::size_t>(chain.mid)].rotation)
                        .normalized();
                std::vector<Mat4f> goal_world;
                compute_world_matrices(sk, goal, goal_world);
                const Vec3f target = point(goal_world, chain.end);
                Pose3D p = pose;
                std::vector<Mat4f> w = world;
                TwoBoneOptions3D o;
                o.pole_mode = PoleMode3D::Position;
                o.pole =
                    point(goal_world, chain.mid); // the elbow where the goal had it (any pole would do for the hand)
                const TwoBoneResult3D res = solve_two_bone(sk, p, w, chain, target, o);
                assert(res.solved);
                worst = std::max(worst, length(point(w, chain.end) - target));
                // The bones keep their lengths (in the root's parent's space: the model has its own scale, uniform
                // here).
                worst_len =
                    std::max(worst_len, std::fabs(length(point(w, chain.mid) - point(w, chain.root)) - l1) / l1);
                worst_len = std::max(worst_len, std::fabs(length(point(w, chain.end) - point(w, chain.mid)) - l2) / l2);
                std::vector<Mat4f> again;
                compute_world_matrices(sk, p, again);
                assert(max_difference(again, w) < 1e-4f);
                ++count;
            }
        }
        std::cout << "  ik CesiumMan " << limb.name << " (" << limb.names[0] << " ...): " << count
                  << " targets, hand within " << worst << " of them, bones kept to " << worst_len << " (relative)"
                  << std::endl;
        assert(worst < 5e-4f && worst_len < 1e-4f);
        total += count;
    }
    assert(total == 400);
}

// The cost of constraints on a character, and of 500 of them with three constraints each.
void bench_ik() {
    const BipedCharacter hero = make_biped_character();
    const auto time_us = [](auto&& fn, int reps) {
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < reps; ++i) {
            fn();
        }
        return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / reps;
    };
    // The pure maths on a 18-joint biped: world matrices, a two-bone arm, a look-at of three joints, a foot placer on
    // two legs.
    Rig rig;
    const std::array<FootLeg3D, 2> legs = {FootLeg3D{{biped::hip_l, biped::knee_l, biped::ankle_l}, 0.05f},
                                           FootLeg3D{{biped::hip_r, biped::knee_r, biped::ankle_r}, 0.05f}};
    const std::array<LookAtJoint3D, 3> chain = {LookAtJoint3D{.joint = biped::spine, .weight = 0.2f},
                                                LookAtJoint3D{.joint = biped::neck, .weight = 0.3f},
                                                LookAtJoint3D{.joint = biped::head, .weight = 0.5f}};
    const GroundQuery3D ground = [](Vec3f from, Vec3f) -> std::optional<GroundHit3D> {
        return GroundHit3D{{from.x, -0.1f, from.z}, {0, 1, 0}};
    };
    FootPlacementState3D fps;
    LookAtState3D las;
    volatile float sink = 0.0f;
    const double maths = time_us(
        [&] {
            Pose3D pose = rig.pose;
            compute_world_matrices(rig.sk, pose, rig.world);
            TwoBoneOptions3D arm;
            arm.pole_mode = PoleMode3D::Direction;
            arm.pole = {0, 0, -1};
            solve_two_bone(rig.sk, pose, rig.world, left_arm, {0.5f, 1.4f, 0.4f}, arm);
            LookAtOptions3D look;
            look.smooth_seconds = 0.1f;
            solve_look_at(rig.sk, pose, rig.world, chain, {1, 1.7f, 2}, look, &las, 1.0f / 60.0f);
            const FootPlan3D plan = plan_foot_placement(rig.sk, pose, rig.world, legs, ground, {}, &fps, 1.0f / 60.0f);
            TwoBoneOptions3D leg;
            leg.pole_mode = PoleMode3D::Direction;
            leg.pole = {0, 0, 1};
            apply_foot_placement(rig.sk, pose, rig.world, biped::pelvis, legs, plan, leg);
            std::vector<Mat4f> m(rig.sk.joint_count());
            world_to_joint_matrices(rig.sk, rig.world, m);
            sink = sink + m[0].m[0];
        },
        20000);
    // The whole system: 500 animators with a hand, a look-at and a foot placer each.
    kin::EcsWorld world;
    WorldRenderState3D state{world};
    auto& w = world.raw();
    for (int n = 0; n < 500; ++n) {
        const flecs::entity e =
            w.entity()
                .set(Transform3D{.position = {static_cast<float>(n % 25) * 2.0f, 0, static_cast<float>(n / 25) * 2.0f}})
                .set(SkinnedMeshRenderer3D{.model = hero.model})
                .set(Animator3D{.clip = "sway"});
        Animator3D& a = *e.get_mut<Animator3D>();
        a.add_ik(hand_constraint()).set_target(Vec3f{0.3f, 1.4f, 0.5f}).set_pole_direction({0, 0, -1});
        IKConstraint3D look;
        look.name = "look";
        look.kind = IKKind3D::LookAt;
        look.joints = {"spine", "neck", "head"};
        look.look_links = {{.weight = 0.2f}, {.weight = 0.3f}, {.weight = 0.5f}};
        look.look_at.smooth_seconds = 0.1f;
        look.target = {IKTarget3D::Kind::World, {5, 1.7f, 5}, 0};
        a.add_ik(look);
        IKConstraint3D feet;
        feet.name = "feet";
        feet.kind = IKKind3D::FootPlacer;
        feet.joints = {"pelvis", "hip_L", "knee_L", "ankle_L", "hip_R", "knee_R", "ankle_R"};
        feet.foot_heights = {0.05f, 0.05f};
        feet.ground = [](Vec3f from, Vec3f) -> std::optional<GroundHit3D> {
            return GroundHit3D{{from.x, -0.05f, from.z}, {0, 1, 0}};
        };
        feet.pole = IKConstraint3D::Pole::ModelDirection;
        feet.pole_vector = {0, 0, 1};
        a.add_ik(feet);
    }
    Scene3D scene;
    const auto frame = [&] {
        state.animate(1.0f / 60.0f);
        state.propagate_transforms();
        scene = Scene3D{};
        state.collect(scene);
    };
    frame();
    const double per_frame = time_us(frame, 60);
    // Without constraints, the same 500.
    kin::EcsWorld plain_world;
    WorldRenderState3D plain_state{plain_world};
    for (int n = 0; n < 500; ++n) {
        plain_world.raw()
            .entity()
            .set(Transform3D{.position = {static_cast<float>(n), 0, 0}})
            .set(SkinnedMeshRenderer3D{.model = hero.model})
            .set(Animator3D{.clip = "sway"});
    }
    const auto plain_frame = [&] {
        plain_state.animate(1.0f / 60.0f);
        plain_state.propagate_transforms();
        scene = Scene3D{};
        plain_state.collect(scene);
    };
    plain_frame();
    const double plain_per_frame = time_us(plain_frame, 60);
    std::cout << "ik, 18-joint biped: arm + look-at (3 joints) + two feet + matrices " << maths
              << " us a character; 500 animators with 3 constraints: animate + collect " << per_frame / 1000.0
              << " ms a frame (" << per_frame / 500.0 << " us each; the same 500 without constraints "
              << plain_per_frame / 1000.0 << " ms)" << std::endl;
}

// --- the 3D ECS bridge: a model replaced under an animator, a mesh entity's id, a model with a second skin ---

// An animator rebinds when its entity's model is replaced, even by one that lands at the old one's address (a freed
// model's place taken again). The address is forced here: two shared_ptrs that own nothing, over one Model3D that is
// changed in place between them, so a comparison of addresses sees no change and only the owner differs.
void test_ecs_animator_rebinds_to_a_replaced_model() {
    const BipedCharacter hero = make_biped_character();
    Model3D storage = *hero.model;
    const std::shared_ptr<const Model3D> first{&storage, [](const Model3D*) {}};
    const std::shared_ptr<const Model3D> second{&storage, [](const Model3D*) {}};
    assert(first.get() == second.get() && (first.owner_before(second) || second.owner_before(first)));

    kin::EcsWorld world;
    WorldRenderState3D state{world};
    const flecs::entity e = world.raw()
                                .entity()
                                .set(Transform3D{})
                                .set(SkinnedMeshRenderer3D{.model = first})
                                .set(Animator3D{.clip = "sway"});
    e.get_mut<Animator3D>()->add_ik(hand_constraint()).set_target(Vec3f{0.3f, 1.4f, 0.5f});
    const float dt = 0.1f;
    ik_frame(state, dt);
    ik_frame(state, dt);
    const auto resolved = [&] { return e.get<Animator3D>()->ik_constraints[0].resolved; };
    assert(close(e.get<Animator3D>()->player.time(), 2.0f * dt) && e.get<Animator3D>()->bound_to(first));
    assert(resolved() == (std::vector<i32>{biped::shoulder_l, biped::elbow_l, biped::hand_l}));

    // The same Model3D under a new owner, with the left and right arms' names swapped in it, as another file's would
    // be.
    Skeleton3D& sk = storage.skeletons[0];
    for (int j = 0; j < 3; ++j) {
        std::swap(sk.names[static_cast<std::size_t>(biped::shoulder_l + j)],
                  sk.names[static_cast<std::size_t>(biped::shoulder_r + j)]);
    }
    e.get_mut<SkinnedMeshRenderer3D>()->model = second;
    ik_frame(state, dt);
    assert(close(e.get<Animator3D>()->player.time(), dt)); // played again from the start: a new binding
    assert(resolved() == (std::vector<i32>{biped::shoulder_r, biped::elbow_r,
                                           biped::hand_r})); // looked up again in what it holds now
    assert(e.get<Animator3D>()->bound_to(second) && !e.get<Animator3D>()->bound_to(first));
}

// A MeshRenderer3D entity is named in the scene as the model and character entities are: by its own `id` when it gives
// one, else by the entity's (its low 32 bits), which is what pick reports for it.
void test_ecs_mesh_renderer_id() {
    struct FakeMesh final : IMeshBackend3D {};
    const Mesh3D mesh{std::make_shared<FakeMesh>(), 3, 3, {}};
    kin::EcsWorld world;
    WorldRenderState3D state{world};
    auto& w = world.raw();
    const flecs::entity plain = w.entity().set(Transform3D{}).set(MeshRenderer3D{.mesh = mesh});
    w.entity().set(Transform3D{}).set(MeshRenderer3D{.mesh = mesh, .id = 7});
    state.propagate_transforms();
    Scene3D scene;
    state.collect(scene);
    assert(scene.instances().size() == 2);
    const u32 plain_id = static_cast<u32>(plain.id() & 0xffffffffu);
    assert(plain_id != 0 && plain_id != 7);
    bool saw_plain = false, saw_given = false;
    for (const MeshInstance3D& instance : scene.instances()) {
        saw_plain = saw_plain || instance.id == plain_id;
        saw_given = saw_given || instance.id == 7;
    }
    assert(saw_plain && saw_given);
}

// draw_skinned(Model3D) is given the pose of the first skeleton: the parts on a second skin are not drawn (the limit,
// pinned), and that is said in the log, once for each model.
void test_draw_skinned_poses_the_first_skin_only() {
    struct FakeMesh final : IMeshBackend3D {};
    const auto make = [] {
        auto model = std::make_unique<Model3D>();
        model->skeletons = {skin_fixtures::make_tube().skeleton, skin_fixtures::make_tube().skeleton};
        for (const i32 skin : {0, 1}) {
            ModelPart3D part;
            part.name = skin ? "second" : "first";
            part.mesh = Mesh3D{std::make_shared<FakeMesh>(), 3, 3, {}, {}, {}, 3};
            part.skin = skin;
            model->parts.push_back(part);
        }
        ModelPart3D still;
        still.name = "still";
        still.mesh = Mesh3D{std::make_shared<FakeMesh>(), 3, 3, {}};
        model->parts.push_back(still);
        return model;
    };
    std::vector<LogEvent> events;
    set_logger_config({.min_level = LogLevel::Debug, .sdl_sink = false, .memory_events = &events});
    const auto lines = [&] {
        std::size_t n = 0;
        for (const LogEvent& event : events) {
            n += event.message.find("another skin") != std::string::npos ? 1 : 0;
        }
        return n;
    };
    const auto a = make(), b = make(); // both alive: two addresses
    const std::vector<Mat4f> joints(3);
    Scene3D scene;
    (void)scene.draw_skinned(*a, joints, Mat4f{}, colors::white, 5);
    assert(scene.draws().size() == 2); // the first skin's part and the one that is not skinned
    assert(scene.draws()[0].mesh == a->parts[0].mesh && scene.draws()[0].skinned);
    assert(scene.draws()[1].mesh == a->parts[2].mesh && !scene.draws()[1].skinned);
    assert(lines() == 1);
    (void)scene.draw_skinned(*a, joints, Mat4f{}, colors::white, 5);
    assert(lines() == 1); // not said again for the same model
    (void)scene.draw_skinned(*b, joints, Mat4f{}, colors::white, 6);
    assert(lines() == 2); // but for another
    reset_logger_config_from_environment();
}

// The CPU cost of the player's other states, printed (no assertion): the base a 1D blend of three clips (a locomotion
// state), a crossfade out of that blend into a clip, a fade begun in the middle of another (the held picture fades
// out), and a base clip under two layers (one masked, one additive). Each is an update and the joint matrices of a
// 24-joint character; the blends and fades combine two poses a frame, which is where a player that allocates for each
// shows.
void bench_animation_states() {
    const auto per_call = [](int iterations, const auto& body) {
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < iterations; ++i) {
            body(i);
        }
        return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / iterations;
    };
    Skeleton3D sk;
    const int n = 24;
    sk.parents.resize(n);
    sk.names.resize(n);
    sk.rest.resize(n);
    sk.inverse_bind.resize(n);
    for (int j = 0; j < n; ++j) {
        sk.parents[j] = j == 0 ? -1 : (j - 1) / 2;
        sk.names[j] = "joint" + std::to_string(j);
        sk.rest[j].translation = {0.0f, 0.2f, 0.0f};
    }
    const auto make_clip = [&](float phase) {
        AnimClip3D clip;
        clip.channels.resize(n);
        for (int j = 0; j < n; ++j) {
            JointChannels3D& c = clip.channels[j];
            for (int k = 0; k < 30; ++k) {
                const float t = static_cast<float>(k) / 29.0f;
                c.translation.times.push_back(t);
                c.translation.values.push_back({0.0f, 0.2f + 0.01f * std::sin(t * 6.0f + phase), 0.0f});
                c.rotation.times.push_back(t);
                c.rotation.values.push_back(
                    Quatf::axis_angle({0, 0, 1}, 30.0f * std::sin(t * 6.0f + static_cast<float>(j) + phase)));
                c.scale.times.push_back(t);
                c.scale.values.push_back({1, 1, 1});
            }
        }
        clip.duration = 1.0f;
        return clip;
    };
    const AnimClip3D idle = make_clip(0.0f), walk = make_clip(1.0f), run = make_clip(2.0f);
    const std::array<BlendPoint3D, 3> points{{{&idle, 0.0f}, {&walk, 1.6f}, {&run, 4.2f}}};
    float sink = 0.0f;
    std::vector<Mat4f> matrices;
    const auto frame = [&](AnimationPlayer3D& player) {
        player.update(1.0f / 60.0f);
        player.joint_matrices(matrices);
        sink += matrices[7].m[12];
    };
    AnimationPlayer3D blend{&sk};
    blend.play_blend(points, 2.5f);
    const double blended = per_call(20000, [&](int) { frame(blend); });
    AnimationPlayer3D fading{&sk};
    fading.play_blend(points, 2.5f);
    fading.crossfade_to(&idle, 1000.0f);
    const double blend_fade = per_call(20000, [&](int) { frame(fading); });
    AnimationPlayer3D frozen{&sk};
    frozen.play(&walk);
    frozen.crossfade_to(&run, 1000.0f);
    frozen.update(1.0f / 60.0f);
    frozen.crossfade_to(&idle, 1000.0f); // begun in the middle of the first: the picture so far is held and fades out
    const double held = per_call(20000, [&](int) { frame(frozen); });
    AnimationPlayer3D layered{&sk};
    layered.play(&walk);
    {
        AnimationPlayer3D::Layer& upper = layered.layer(0);
        upper.clip = &run;
        upper.mask = PoseMask3D::subtree(sk, 2, 1.0f);
        upper.weight = 0.7f;
        AnimationPlayer3D::Layer& add = layered.layer(1);
        add.clip = &idle;
        add.additive = true;
        add.weight = 0.5f;
    }
    const double layers = per_call(20000, [&](int) { frame(layered); });
    std::cerr << "animation, a 24-joint character: 1D blend of three clips " << blended
              << " us, the blend crossfaded into a clip " << blend_fade << " us, a fade begun in the middle of another "
              << held << " us, a clip under a masked and an additive layer " << layers << " us (update + matrices) ["
              << sink << "]\n";
    // The ECS animators: 200 characters in a machine's locomotion state (a 1D blend) with one layer,
    // WorldRenderState3D::animate a frame. Each frame an animator asks which state it is in and which layer clips it
    // plays; that is a string it should not have to build.
    {
        const Character hero = make_character();
        auto machine = std::make_shared<AnimationStateMachine>();
        machine->initial = "locomotion";
        machine->states = {{"idle", "idle"}, {"locomotion", "walk"}};
        Animator3D animator;
        animator.machine = machine;
        animator.blend_spaces["locomotion"] = {"speed", {{"walk", 1.5f}, {"run", 4.5f}}};
        animator.params.set_float("speed", 3.0f);
        animator.layers.push_back({.clip = "run", .mask_joint = "upper_body_spine_joint", .weight = 0.5f});
        kin::EcsWorld world;
        WorldRenderState3D state{world};
        for (int i = 0; i < 200; ++i) {
            world.raw()
                .entity()
                .set(Transform3D{})
                .set(SkinnedMeshRenderer3D{.model = hero.model})
                .set(static_cast<const Animator3D&>(animator));
        }
        for (int i = 0; i < 120; ++i) {
            state.animate(1.0f / 60.0f); // settled: the machine has made its transition, the fade is over
        }
        const double animate_us = per_call(2000, [&](int) { state.animate(1.0f / 60.0f); });
        std::cerr << "animation, WorldRenderState3D::animate for 200 characters in a blend state with a layer: "
                  << animate_us << " us a frame (" << animate_us / 200.0 << " us a character)\n";
    }
}

} // namespace

int main() {
    test_sampling_linear_and_step();
    test_sampling_cubic_spline();
    test_slerp_properties();
    test_clip_time();
    test_sample_clip();
    test_blend_masks_and_additive();
    test_joint_matrices();
    test_root_motion();
    test_player_crossfade();
    test_player_layers_and_root_motion();
    test_player_blend_1d();
    test_scene_shared_pose();
    test_ecs_animator_clip();
    test_ecs_animator_state_machine();
    test_ecs_animator_layers_and_root_motion();
    test_skin_bounds_hold_the_posed_vertices();
    test_skin_bounds_count_every_influence();
    test_cpu_skinning();
    test_mesh_skin_data();
    test_loader_skins_and_clips();
    test_loader_skin_limits();
    test_loader_shared_skin_channels();
    test_loader_texture_transform();
    test_loader_refuses_unreadable_geometry();
    test_loader_keeps_nodes_between_joints();
    test_loader_refuses_hostile_files();
    test_loader_node_animation();
    test_node_animation_draws();
    test_ik_two_bone_reach();
    test_ik_two_bone_out_of_reach();
    test_ik_two_bone_pole();
    test_ik_two_bone_weight();
    test_ik_two_bone_idempotent();
    test_ik_two_bone_continuity();
    test_ik_two_bone_scaled_and_turned();
    test_ik_two_bone_orientation();
    test_ik_look_at();
    test_ik_large_skeleton();
    test_ik_foot_placement();
    test_ik_fabrik();
    test_ik_order_and_bad_input();
    test_ecs_ik_reaches_world_target();
    test_ecs_ik_entity_target_and_fades();
    test_ecs_ik_order_and_off_path();
    test_ecs_ik_deterministic_and_unresolved();
    test_ik_joint_limits();
    test_ik_rest_bend_fallback();
    test_ik_two_bone_nonuniform_scale();
    test_ik_bend_swing_and_singularities();
    test_ik_fabrik_pole();
    test_ik_foot_locking_and_toes();
    test_ecs_ik_fabrik_locking_and_limits();
    test_sponza_walk_ik_panel();
    test_ik_cesium_man();
    test_ecs_animator_rebinds_to_a_replaced_model();
    test_ecs_mesh_renderer_id();
    test_draw_skinned_poses_the_first_skin_only();
    bench_ik();
    bench_animation();
    bench_animation_states();
    std::cout << "skeleton tests passed\n";
    return 0;
}
