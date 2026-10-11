// Skeletons, clips, poses and the glTF skin reader (kin/render3d/skeleton.hpp, animation3d.hpp, model.hpp), without a GPU:
// keyframe sampling against hand-computed glTF values, blends, additive clips, masks, joint matrices of a known chain, root
// motion, the player's crossfade, bounds that no posed vertex leaves, and the loader on glTF files made in code.

#include "skin_fixtures.hpp"

#include <kin/ecs/render3d.hpp>
#include <kin/ecs/world.hpp>
#include <kin/render3d/animation3d.hpp>
#include <kin/render3d/ik.hpp>
#include <kin/render3d/model.hpp>
#include <kin/render3d/skeleton.hpp>

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
#include <random>

namespace {

using namespace kin;
using skin_fixtures::GltfOptions;

bool close(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }
bool close(Vec3f a, Vec3f b, float eps = 1e-5f) { return close(a.x, b.x, eps) && close(a.y, b.y, eps) && close(a.z, b.z, eps); }
bool close(Quatf a, Quatf b, float eps = 1e-5f) {
    // The same rotation whichever sign.
    const float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    return std::fabs(std::fabs(d) - 1.0f) <= eps;
}
float length_of(Quatf q) { return std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w); }

Quatf rz(float degrees) { return Quatf::axis_angle({0, 0, 1}, degrees); }

// --- sampling --------------------------------------------------------------------------------------------------------------

void test_sampling_linear_and_step() {
    Track3D<Vec3f> t;
    t.interpolation = Interpolation3D::Linear;
    t.times = {0.0f, 1.0f, 3.0f};
    t.values = {{0, 0, 0}, {2, 4, 6}, {2, 0, 0}};
    const Vec3f fallback{9, 9, 9};
    assert(close(sample_track(t, 0.5f, fallback), {1, 2, 3}));
    assert(close(sample_track(t, 2.0f, fallback), {2, 2, 3})); // halfway between keys 1 and 3 s
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
    // each key stored as in-tangent a, value v, out-tangent b. Worked by hand for dt = 1, s = 0.5: the weights are 0.5, 0.125,
    // 0.5, -0.125.
    Track3D<Vec3f> t;
    t.interpolation = Interpolation3D::CubicSpline;
    t.times = {0.0f, 1.0f};
    t.values = {{7, 7, 7}, {0, 0, 0}, {1, 2, 0},   // key 0: in (unused), value, out
                {0, 0, 4}, {2, 4, 6}, {7, 7, 7}};  // key 1: in, value, out (unused)
    Vec3f p = sample_track(t, 0.5f, {});
    // x: 0.5*0 + 0.125*1 + 0.5*2 + (-0.125)*0 = 1.125; y: 0 + 0.25 + 2 + 0 = 2.25; z: 0 + 0 + 3 + (-0.125*4) = 2.5
    assert(close(p, {1.125f, 2.25f, 2.5f}));
    // On the keys: the values (not the tangents).
    assert(close(sample_track(t, 0.0f, {}), {0, 0, 0}) && close(sample_track(t, 1.0f, {}), {2, 4, 6}));
    assert(close(sample_track(t, -3.0f, {}), {0, 0, 0}) && close(sample_track(t, 9.0f, {}), {2, 4, 6}));
    // dt scales the tangents: the same keys over two seconds, sampled at the middle (s = 0.5): tangent terms twice as big.
    t.times = {0.0f, 2.0f};
    p = sample_track(t, 1.0f, {});
    assert(close(p, {0.125f * 2 * 1 + 1.0f, 0.125f * 2 * 2 + 2.0f, 3.0f - 0.125f * 2 * 4}));
    // s = 0.25: h00 = 2/64 - 3/16 + 1 = 0.84375, h10 = 1/64 - 2/16 + 1/4 = 0.140625, h01 = 0.15625, h11 = 1/64 - 1/16 = -0.046875.
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
    assert(close(clip_time(clip, 0.5f, true), 0.5f) && close(clip_time(clip, 2.5f, true), 0.5f) && close(clip_time(clip, 4.0f, true), 0.0f));
    assert(close(clip_time(clip, -0.5f, true), 1.5f));
    assert(close(clip_time(clip, 2.5f, false), 2.0f) && close(clip_time(clip, -1.0f, false), 0.0f));
    clip.duration = 0.0f;
    assert(clip_time(clip, 3.0f, true) == 0.0f && clip_time(clip, 3.0f, false) == 0.0f);
    clip.duration = 1.0f;
    assert(clip_time(clip, std::nanf(""), true) == 0.0f);
}

// A clip of the tube's three joints: the middle joint's rotation 0, 90, 0 degrees over two seconds; the base slides along x.
AnimClip3D make_clip(const Skeleton& sk) {
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
    Pose pose;
    sample_clip(clip, tube.skeleton, 0.5f, true, pose);
    assert(pose.joints.size() == 3);
    assert(close(pose.joints[1].rotation, rz(45)));
    assert(close(pose.joints[0].translation, {1, 0.25f, 0.5f}));
    // A joint with no tracks keeps its rest value (the top joint, and the rest translation of the middle one).
    assert(close(pose.joints[2].translation, tube.skeleton.rest[2].translation) && close(pose.joints[1].translation, tube.skeleton.rest[1].translation));
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

// --- combining -------------------------------------------------------------------------------------------------------------

void test_blend_masks_and_additive() {
    const auto tube = skin_fixtures::make_tube();
    const Skeleton& sk = tube.skeleton;
    Pose a = sk.rest_pose(), b = sk.rest_pose();
    a.joints[0].translation = {0, 0, 0};
    b.joints[0].translation = {4, 0, 0};
    a.joints[1].rotation = rz(0);
    b.joints[1].rotation = rz(90);
    a.joints[2].scale = {1, 1, 1};
    b.joints[2].scale = {3, 1, 1};
    const Pose half = blend_poses(a, b, 0.5f);
    assert(close(half.joints[0].translation, {2, 0, 0}) && close(half.joints[1].rotation, rz(45)) && close(half.joints[2].scale, {2, 1, 1}));
    assert(close(blend_poses(a, b, 0.0f).joints[0].translation, {0, 0, 0}) && close(blend_poses(a, b, 1.0f).joints[0].translation, {4, 0, 0}));

    // Masks.
    const PoseMask arm = PoseMask::subtree(sk, 1);
    assert(arm.weights == (std::vector<float>{0, 1, 1}));
    assert(PoseMask::subtree(sk, 1, 1.0f, false).weights == (std::vector<float>{0, 0, 1}));
    assert(PoseMask::subtree(sk, 0, 0.5f).weights == (std::vector<float>{0.5f, 0.5f, 0.5f}));
    assert(PoseMask::subtree(sk, 7).weights == (std::vector<float>{0, 0, 0}));
    const std::string_view names[] = {"top", "nothing"};
    assert(PoseMask::joints(sk, names).weights == (std::vector<float>{0, 0, 1}));
    PoseMask both = PoseMask::joints(sk, names);
    both.add(PoseMask::subtree(sk, 1));
    assert(both.weights == (std::vector<float>{0, 1, 1}));
    Pose base = a;
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
    blend_masked(base, b, PoseMask{{0.0f, 0.25f}}, 1.0f); // a short mask: the rest are 0
    assert(close(base.joints[1].rotation, rz(22.5f), 1e-4f) && close(base.joints[2].scale, {1, 1, 1}));

    // Additive: the change of `add` from `reference`, put on a base that is somewhere else.
    Pose reference = sk.rest_pose();
    Pose add = reference;
    add.joints[0].translation = {1, 2, 3};
    add.joints[1].rotation = rz(90);
    add.joints[2].scale = {2, 1, 1};
    Pose target = sk.rest_pose();
    target.joints[0].translation = {10, 0, 0};
    target.joints[1].rotation = rz(30);
    target.joints[2].scale = {1.5f, 1, 1};
    Pose out = target;
    apply_additive(out, add, reference, {}, 1.0f);
    assert(close(out.joints[0].translation, {11, 2, 3}));
    assert(close(out.joints[1].rotation, rz(120)));
    assert(close(out.joints[2].scale, {2.5f, 1, 1}));
    out = target;
    apply_additive(out, add, reference, {}, 0.5f);
    assert(close(out.joints[0].translation, {10.5f, 1, 1.5f}) && close(out.joints[1].rotation, rz(75), 1e-4f) && close(out.joints[2].scale, {2.0f, 1, 1}));
    out = target;
    apply_additive(out, add, reference, arm, 1.0f);
    assert(close(out.joints[0].translation, {10, 0, 0}) && close(out.joints[1].rotation, rz(120)));
    out = target;
    apply_additive(out, reference, reference, {}, 1.0f); // the reference on itself adds nothing
    assert(close(out.joints[0].translation, target.joints[0].translation) && close(out.joints[1].rotation, target.joints[1].rotation));
}

// --- joint matrices --------------------------------------------------------------------------------------------------------

void test_joint_matrices() {
    const auto tube = skin_fixtures::make_tube(25, 12, 3.0f);
    const Skeleton& sk = tube.skeleton;
    std::string why;
    assert(sk.validate(&why));
    // At rest every matrix is the identity: a skinned mesh draws as modelled.
    std::vector<Mat4f> m;
    compute_joint_matrices(sk, sk.rest_pose(), m);
    assert(m.size() == 3);
    for (const Mat4f& mat : m) {
        for (int i = 0; i < 16; ++i) {
            assert(close(mat.m[i], Mat4f{}.m[i], 1e-6f));
        }
    }
    // The middle joint turned 90 degrees about Z: the joint at (0, 1, 0) stays; the top joint's origin, (0, 2, 0) in the mesh,
    // goes to (0, 1, 0) + Rz90 (0, 1, 0) = (-1, 1, 0); and a vertex at (0, 3, 0), bound to the top, to (-2, 1, 0).
    Pose pose = skin_fixtures::bent(tube, 90.0f, 0.0f);
    compute_joint_matrices(sk, pose, m);
    assert(close(m[1].apply({0, 1, 0}), {0, 1, 0}, 1e-5f));
    assert(close(m[2].apply({0, 2, 0}), {-1, 1, 0}, 1e-5f));
    assert(close(m[2].apply({0, 3, 0}), {-2, 1, 0}, 1e-5f));
    assert(close(m[0].apply({0, 3, 0}), {0, 3, 0}));
    // The top turned as well, 90 degrees more: (0, 3, 0) is one up from the top joint, rotated by 180: (-1, 0, 0) from (-1, 1, 0).
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
    Skeleton moved = sk;
    moved.root_transform = Mat4f::translation({0, 0, 7});
    compute_joint_matrices(moved, moved.rest_pose(), m);
    assert(close(m[2].apply({0, 0, 0}), {0, 0, 7}));
    // Into room the caller keeps, and a pose that is short: the rest pose fills in.
    std::vector<Mat4f> room(3);
    Pose short_pose;
    short_pose.joints = {pose.joints[0]};
    compute_joint_matrices(sk, short_pose, std::span<Mat4f>{room});
    assert(close(room[2].apply({0, 0, 0}), {5, 0, 0}));
    // Validation: a parent must come first; too many joints are refused.
    Skeleton bad = sk;
    bad.parents = {1, 0, 1};
    assert(!bad.validate(&why) && !why.empty());
    Skeleton big;
    big.parents.assign(257, -1);
    big.rest.resize(257);
    big.inverse_bind.resize(257);
    assert(!big.validate(&why));
    assert(!Skeleton{}.validate(&why));
    // depth, find, subtree
    assert(sk.find("top") == 2 && sk.find("nope") == -1 && sk.depth(2) == 2 && sk.depth(0) == 0 && sk.root_joint() == 0);
    assert(sk.subtree(1) == (std::vector<u8>{0, 1, 1}));
}

// --- root motion -----------------------------------------------------------------------------------------------------------

void test_root_motion() {
    const auto tube = skin_fixtures::make_tube();
    const Skeleton& sk = tube.skeleton;
    // The root moves 2 m/s in x along a wobble in y, and turns 90 degrees over the 2-second clip.
    AnimClip3D clip;
    clip.channels.resize(3);
    clip.channels[0].translation.times = {0.0f, 1.0f, 2.0f};
    clip.channels[0].translation.values = {{0, 0, 0}, {2, 1, 0}, {4, 0, 0}};
    clip.channels[0].rotation.times = {0.0f, 2.0f};
    clip.channels[0].rotation.values = {rz(0), rz(90)};
    clip.duration = 2.0f;
    RootMotion m = extract_root_motion(clip, sk, 0.0f, 1.0f, true);
    assert(close(m.translation, {2, 0, 0})); // y is not carried by default
    m = extract_root_motion(clip, sk, 0.5f, 1.5f, true, {.x = true, .y = true, .z = false, .rotation = true});
    assert(close(m.translation, {2, 0.0f, 0}, 1e-5f)); // 1 -> 3 in x; y: 0.5 -> 0.5
    assert(close(m.rotation, rz(45), 1e-4f));
    // Across the end of a looping clip: the end's motion from `from`, then the start's to `to`.
    m = extract_root_motion(clip, sk, 1.5f, 2.5f, true, {.x = true, .y = false, .z = true, .rotation = true});
    assert(close(m.translation, {2.0f, 0, 0}, 1e-5f));          // 3 -> 4 (1) and 0 -> 1 (1)
    assert(close(m.rotation, rz(45), 1e-4f));                  // 67.5 -> 90, then 0 -> 22.5
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
    Pose pose;
    sample_clip(clip, sk, 1.0f, true, pose);
    pose.joints[1].rotation = rz(10);
    remove_root_motion(pose, sk);
    assert(close(pose.joints[0].translation, {0, 1, 0}) && close(pose.joints[1].rotation, rz(10)));
    sample_clip(clip, sk, 1.0f, true, pose);
    remove_root_motion(pose, sk, {.x = true, .y = true, .z = true, .rotation = true});
    assert(close(pose.joints[0].translation, {0, 0, 0}) && close(pose.joints[0].rotation, Quatf{}));
}

// --- the player ------------------------------------------------------------------------------------------------------------

AnimClip3D constant_clip(const Skeleton& sk, Vec3f where, float duration = 1.0f) {
    AnimClip3D clip;
    clip.channels.resize(sk.joint_count());
    clip.channels[0].translation.times = {0.0f};
    clip.channels[0].translation.values = {where};
    clip.duration = duration;
    return clip;
}

void test_player_crossfade() {
    const auto tube = skin_fixtures::make_tube();
    const Skeleton& sk = tube.skeleton;
    const AnimClip3D a = constant_clip(sk, {0, 0, 0}), b = constant_clip(sk, {10, 0, 0});
    AnimationPlayer3D player{&sk};
    assert(player.pose().joints.size() == 3 && !player.finished());
    player.play(&a);
    player.update(0.1f);
    assert(close(player.pose().joints[0].translation, {0, 0, 0}) && player.clip() == &a && player.fading_from() == nullptr);
    player.crossfade_to(&b, 1.0f);
    assert(player.clip() == &b && player.fading_from() == &a && close(player.fade_progress(), 0.0f));
    player.update(0.25f);
    assert(close(player.pose().joints[0].translation, {2.5f, 0, 0}) && close(player.fade_progress(), 0.25f));
    player.update(0.25f);
    assert(close(player.pose().joints[0].translation, {5, 0, 0}));
    player.update(0.5f); // done: all of b, and a is dropped
    assert(close(player.pose().joints[0].translation, {10, 0, 0}) && player.fading_from() == nullptr && close(player.fade_progress(), 1.0f));
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
    const Skeleton& sk = tube.skeleton;
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
    layer.mask = PoseMask::subtree(sk, 1);
    layer.weight = 0.5f;
    player.update(0.5f);
    assert(player.layer_count() == 1);
    assert(close(player.pose().joints[0].translation, {1, 0, 0})); // the mask keeps the base's root
    assert(close(player.pose().joints[1].rotation, rz(20), 1e-4f)); // 40 degrees at half weight
    // Additive: the clip's change from its first frame.
    player.clear_layers();
    AnimationPlayer3D::Layer& add = player.layer(0);
    add.clip = &wave;
    add.additive = true;
    add.loop = false;
    player.update(1.0f);
    assert(close(player.pose().joints[1].rotation, rz(80), 1e-4f) && close(player.pose().joints[0].translation, {1, 0, 0}));

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
}

// --- bounds ----------------------------------------------------------------------------------------------------------------

void test_skin_bounds_hold_the_posed_vertices() {
    const auto tube = skin_fixtures::make_tube();
    const std::vector<SkinBound> bounds = compute_skin_bounds(tube.mesh, 3);
    assert(bounds.size() == 3 && bounds[0].radius >= 0.0f && bounds[1].radius >= 0.0f && bounds[2].radius >= 0.0f);
    std::mt19937 rng{1234};
    std::uniform_real_distribution<float> angle(-170.0f, 170.0f), move(-3.0f, 3.0f);
    std::vector<Mat4f> m;
    for (int trial = 0; trial < 500; ++trial) {
        Pose pose = skin_fixtures::bent(tube, angle(rng), angle(rng), {move(rng), move(rng), move(rng)});
        if (trial % 3 == 0) {
            pose.joints[1].scale = {1.0f + std::fabs(move(rng)) * 0.3f, 0.5f, 1.5f}; // non-uniform scale is allowed for
        }
        compute_joint_matrices(tube.skeleton, pose, m);
        Vec3f c{};
        float r = 0.0f;
        assert(skin_sphere(bounds, m, c, r));
        const MeshData posed = skin_mesh(tube.mesh, m, /*gpu_weights=*/false);
        for (const MeshVertex& v : posed.vertices) {
            assert(length(v.position - c) <= r + 1e-4f);
        }
        const MeshData posed_gpu = skin_mesh(tube.mesh, m, /*gpu_weights=*/true);
        for (const MeshVertex& v : posed_gpu.vertices) {
            assert(length(v.position - c) <= r + 1e-4f);
        }
    }
    // At rest the sphere is not much bigger than the mesh (it is a bound, but a useful one).
    compute_joint_matrices(tube.skeleton, tube.skeleton.rest_pose(), m);
    Vec3f c{};
    float r = 0.0f;
    assert(skin_sphere(bounds, m, c, r));
    const Bounds3 box = tube.mesh.bounds();
    assert(r < box.radius() * 1.5f);
    // An arm swung out of the rest bound is inside the posed one: the case a rest bound would cull.
    const Pose out = skin_fixtures::bent(tube, 90.0f, 0.0f, {20.0f, 0, 0});
    compute_joint_matrices(tube.skeleton, out, m);
    assert(skin_sphere(bounds, m, c, r) && c.x > 15.0f);
    // No bounds, no joints used: nothing to bound.
    assert(!skin_sphere({}, m, c, r));
    // stretch_bound: 1 for a rotation, the largest scale for a scale, never below the true stretch.
    assert(close(stretch_bound(Mat4f::rotation(rz(33))), 1.0f, 1e-5f) && close(stretch_bound(Mat4f::scaling({2, 3, 1})), 3.0f, 1e-5f));
    assert(stretch_bound(Mat4f::rotation(rz(30)) * Mat4f::scaling({1, 4, 2})) >= 4.0f - 1e-5f);
}

// Every joint that pulls on a vertex bounds it, not only the strongest: a vertex held 40% by a joint that stays and 30% each by two that
// swing half a turn about the origin ends up inside the spheres only if the joints it is merely shared with count its place too.
void test_skin_bounds_count_every_influence() {
    MeshData m;
    m.vertices.assign(3, MeshVertex{});
    m.vertices[0].position = {0, 0, 0};
    m.vertices[1].position = {0, 0, 0};
    m.vertices[2].position = {10, 0, 0};
    m.indices = {0, 1, 2};
    m.skin.resize(3);
    m.skin[0] = {{0, 0, 0, 0}, {1, 0, 0, 0}};
    m.skin[1] = {{1, 0, 0, 0}, {1, 0, 0, 0}};
    m.skin[2] = {{0, 1, 2, 0}, {0.4f, 0.3f, 0.3f, 0.0f}};
    assert(m.validate());
    const std::vector<SkinBound> bounds = compute_skin_bounds(m, 3);
    assert(bounds[2].radius >= 0.0f && close(bounds[2].center, {10, 0, 0})); // joint 2 pulls on one vertex only, and still has a bound
    assert(close(bounds[1].center, {5, 0, 0}) && bounds[1].radius >= 5.0f);
    const Mat4f half_turn = Mat4f::rotation(Quatf::axis_angle({0, 0, 1}, 180.0f));
    const std::vector<Mat4f> matrices = {Mat4f{}, half_turn, half_turn};
    const MeshData posed = skin_mesh(m, matrices, false);
    assert(close(posed.vertices[2].position, {-2.0f, 0.0f, 0.0f}, 1e-4f));
    Vec3f c{};
    float r = 0.0f;
    assert(skin_sphere(bounds, matrices, c, r));
    for (const MeshVertex& v : posed.vertices) {
        assert(length(v.position - c) <= r + 1e-4f);
    }
}

void test_cpu_skinning() {
    const auto tube = skin_fixtures::make_tube();
    std::vector<Mat4f> m;
    // At rest, skinning changes nothing.
    compute_joint_matrices(tube.skeleton, tube.skeleton.rest_pose(), m);
    const MeshData same = skin_mesh(tube.mesh, m);
    for (std::size_t i = 0; i < same.vertices.size(); ++i) {
        assert(close(same.vertices[i].position, tube.mesh.vertices[i].position, 1e-5f) && close(same.vertices[i].normal, tube.mesh.vertices[i].normal, 1e-5f));
    }
    assert(!same.skinned() && same.indices == tube.mesh.indices);
    // Bent 90 degrees over the middle joint: the top ring is at (-2, 1, 0) + the radius.
    compute_joint_matrices(tube.skeleton, skin_fixtures::bent(tube, 90.0f, 0.0f), m);
    const MeshData bent = skin_mesh(tube.mesh, m);
    // The last ring is bound to the top joint alone: its vertices (x, 3, z) go to (-2, 1 + x, z) (worked by hand in test_joint_matrices).
    for (std::size_t i = bent.vertices.size() - 13; i < bent.vertices.size(); ++i) {
        const Vec3f at = tube.mesh.vertices[i].position;
        assert(close(bent.vertices[i].position, {-2.0f, 1.0f + at.x, at.z}, 1e-4f));
    }
    for (const MeshVertex& v : bent.vertices) {
        assert(std::isfinite(v.position.x) && close(length(v.normal), 1.0f, 1e-4f));
    }
}

void test_mesh_skin_data() {
    MeshData mesh = meshes::box({1, 1, 1});
    assert(!mesh.skinned() && mesh.skin_joint_count() == 0);
    mesh.skin.assign(mesh.vertices.size(), {});
    std::string why;
    assert(mesh.validate(&why) && mesh.skinned() && mesh.skin_joint_count() == 1);
    mesh.skin[3].joints = {5, 0, 0, 0};
    assert(mesh.skin_joint_count() == 6);
    mesh.skin[3].joints = {5, 7, 0, 0};
    mesh.skin[3].weights = {0.5f, 0.0f, 0, 0}; // a joint with no weight does not count
    assert(mesh.skin_joint_count() == 6);
    mesh.skin.pop_back();
    assert(!mesh.validate(&why) && why.find("skin") != std::string::npos);
    mesh.skin.push_back({});
    mesh.skin[0].weights = {0, 0, 0, 0};
    assert(!mesh.validate(&why));
    mesh.skin[0].weights = {-1, 1, 0, 0};
    assert(!mesh.validate(&why));
    mesh.skin[0].weights = {std::nanf(""), 1, 0, 0};
    assert(!mesh.validate(&why));
    mesh.skin[0] = {};
    assert(mesh.validate(&why));
    // append keeps the skin parallel, binding what has none to joint 0.
    MeshData a = meshes::box({1, 1, 1});
    MeshData b = meshes::box({1, 1, 1});
    b.skin.assign(b.vertices.size(), MeshSkinVertex{{3, 0, 0, 0}, {1, 0, 0, 0}});
    const std::size_t av = a.vertices.size();
    a.append(b, Mat4f::translation({5, 0, 0}));
    assert(a.skin.size() == a.vertices.size() && a.skin[0].joints[0] == 0 && a.skin[av].joints[0] == 3 && a.validate(&why));
    MeshData c = b;
    c.append(meshes::box({1, 1, 1}));
    assert(c.skin.size() == c.vertices.size() && c.skin.front().joints[0] == 3 && c.skin.back().joints[0] == 0 && c.validate(&why));
    MeshData plain = meshes::box({1, 1, 1});
    plain.append(meshes::box({2, 2, 2}));
    assert(!plain.skinned());
    // Weights as the GPU holds them: bytes that sum to exactly 255.
    const std::array<float, 4> cases[] = {{1, 0, 0, 0}, {0.5f, 0.5f, 0, 0}, {1.0f / 3, 1.0f / 3, 1.0f / 3, 0}, {0.25f, 0.25f, 0.25f, 0.25f}, {0.7f, 0.2f, 0.07f, 0.03f},
                                         {2, 2, 0, 0}, {0, 0, 0, 0}, {0.001f, 0.999f, 0, 0}};
    for (const auto& w : cases) {
        const std::array<u8, 4> q = quantise_skin_weights(w);
        assert(int(q[0]) + q[1] + q[2] + q[3] == 255);
    }
    assert(quantise_skin_weights({1, 0, 0, 0}) == (std::array<u8, 4>{255, 0, 0, 0}));
    assert(quantise_skin_weights({0, 0, 0, 0}) == (std::array<u8, 4>{255, 0, 0, 0}));
    assert(quantise_skin_weights({0.5f, 0.5f, 0, 0})[0] + quantise_skin_weights({0.5f, 0.5f, 0, 0})[1] == 255);
}

// --- the loader ------------------------------------------------------------------------------------------------------------

ModelData read(const skin_fixtures::Gltf& gltf) {
    ModelData data;
    std::string error;
    const bool ok = read_model_data(gltf.json, "memory.gltf", data, &error);
    if (!ok) {
        std::cerr << "read_model_data: " << error << '\n' << gltf.json << '\n';
    }
    assert(ok);
    return data;
}

// The weights of joint 0 and joint 1 of each vertex of the first part, as the loader made them.
std::array<std::array<float, 2>, 6> weights_of(const ModelData& data) {
    std::array<std::array<float, 2>, 6> out{};
    const MeshData& mesh = data.parts[0].mesh;
    assert(mesh.skin.size() == 6);
    for (std::size_t v = 0; v < 6; ++v) {
        for (std::size_t k = 0; k < 4; ++k) {
            if (mesh.skin[v].weights[k] > 0.0f) {
                assert(mesh.skin[v].joints[k] < 2);
                out[v][mesh.skin[v].joints[k]] += mesh.skin[v].weights[k];
            }
        }
        float sum = 0.0f;
        for (float w : mesh.skin[v].weights) sum += w;
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
                const ModelData data = read(gltf);
                assert(data.parts.size() == 1 && data.skeletons.size() == 1 && data.clips.size() == 1);
                const ModelPartData& part = data.parts[0];
                assert(part.skin == 0 && part.mesh.vertices.size() == 6 && part.mesh.skinned() && part.mesh.validate());
                // The skinned part's node placing is ignored.
                assert(part.transform.m[12] == 0.0f && part.transform.m[13] == 0.0f);
                const auto w = weights_of(data);
                for (std::size_t v = 0; v < 6; ++v) {
                    assert(close(w[v][0], gltf.weight[v][0], 0.004f) && close(w[v][1], gltf.weight[v][1], 0.004f));
                }
                // The skeleton: two joints, root first whatever order the file listed them in; the inverse binds follow.
                const Skeleton& sk = data.skeletons[0];
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
    const ModelData data = read(gltf);
    const Skeleton& sk = data.skeletons[0];
    const AnimClip3D& clip = data.clips[0];
    assert(clip.name == "bend" && close(clip.duration, 2.0f) && clip.skeleton == 0 && clip.channels.size() == 2);
    assert(clip.channels[1].rotation.times.size() == 3 && clip.channels[1].rotation.interpolation == Interpolation3D::Linear);
    assert(clip.channels[0].translation.interpolation == Interpolation3D::CubicSpline && clip.channels[0].translation.values.size() == 6);
    assert(clip.channels[1].translation.interpolation == Interpolation3D::Step);
    Pose pose;
    sample_clip(clip, sk, 0.5f, false, pose);
    assert(close(pose.joints[1].rotation, rz(45), 1e-5f));
    assert(close(pose.joints[0].translation, {1.125f, 0, 0}, 1e-5f)); // the spline by hand: see test_sampling_cubic_spline
    assert(close(pose.joints[1].translation, {0, 1, 0}));
    sample_clip(clip, sk, 1.0f, false, pose);
    assert(close(pose.joints[1].translation, {0, 2, 0}) && close(pose.joints[1].rotation, rz(90), 1e-5f) && close(pose.joints[0].translation, {2, 0, 0}));
    sample_clip(clip, sk, 1.75f, false, pose);
    assert(close(pose.joints[1].translation, {0, 2, 0}) && close(pose.joints[1].rotation, rz(22.5f), 1e-4f));
    Model model;
    model.clips = data.clips;
    assert(model.find_clip("bend") == &model.clips[0] && model.find_clip("nope") == nullptr && model.find_clip("bend", 1) == nullptr);

    // Weights that do not sum to 1 are made to; a vertex with none goes to joint 0; an index past the skin's joints is dropped.
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

    // Morph target weights channels are ignored (logged); the joints' clip is kept. So are animations of nodes that are no joints.
    o = {};
    o.morph_channel = true;
    o.node_animation = true;
    auto data3 = read(skin_fixtures::make_gltf(o));
    assert(data3.clips.size() == 1 && data3.clips[0].channels.size() == 2 && data3.clips[0].channels[1].rotation.times.size() == 3);
    // The loose mesh (a node that is no joint) is a static part at its node's placing; the strip is skinned.
    assert(data3.parts.size() == 2 && data3.parts[0].skin == 0 && data3.parts[1].skin == -1);

    // A mesh on a node under a joint becomes a part bound to that joint, in the space the inverse binds start from.
    o = {};
    o.attachment = true;
    o.joint_count = 3;
    auto data4 = read(skin_fixtures::make_gltf(o));
    assert(data4.parts.size() == 2);
    const ModelPartData* hat = nullptr;
    for (const auto& part : data4.parts) hat = part.name == "hat" ? &part : hat;
    assert(hat && hat->skin == 0 && hat->mesh.skinned() && hat->mesh.skin[0].joints[0] == 1 && hat->mesh.skin[0].weights[0] == 1.0f);
    // The hat's vertices are in the world at rest: the strip's y 0..2 placed at the hat node (0, 0.5) under joint1 (0, 1): 1.5 .. 3.5, x from the armature (2).
    assert(close(hat->mesh.bounds().min.y, 1.5f, 1e-5f) && close(hat->mesh.bounds().max.y, 3.5f, 1e-5f) && close(hat->mesh.bounds().min.x, 2.0f - 0.5f, 1e-5f));
    assert(hat->transform.m[12] == 0.0f && hat->transform.m[13] == 0.0f);

    // A file with no skin reads as before: no skeleton, no clips, parts not skinned.
    ModelData plain;
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
        assert(read_model_data(json, "plain.gltf", plain, &error));
        assert(plain.skeletons.empty() && plain.clips.empty() && plain.parts.size() == 1 && plain.parts[0].skin == -1 && !plain.parts[0].mesh.skinned());
    }
    // Not glTF at all: refused, with why.
    ModelData none;
    assert(!read_model_data("this is not a model", "x.gltf", none, &error) && !error.empty());
}

void test_loader_skin_limits() {
    // 256 joints are the most: this one is kept; 257 is refused, logged, the model static.
    GltfOptions o;
    o.joint_count = 256;
    auto data = read(skin_fixtures::make_gltf(o));
    assert(data.skeletons.size() == 1 && data.skeletons[0].joint_count() == 256 && data.skeletons[0].validate() && data.parts[0].mesh.skinned());
    assert(data.clips.size() == 1);
    o.joint_count = 257;
    data = read(skin_fixtures::make_gltf(o));
    assert(data.skeletons.empty() && data.clips.empty());
    assert(data.parts.size() == 1 && data.parts[0].skin == -1 && !data.parts[0].mesh.skinned());
    // Static, at its node's placing (as a model without a skin would be).
    assert(data.parts[0].mesh.vertices.size() == 6);
}

// The CPU cost of animating one character a frame, printed (no assertion: a machine's speed is not a test): a clip sampled and its joint
// matrices made, and a crossfade (two clips sampled, blended, then the matrices) through the player. The synthetic skeleton has 24 joints
// with three tracks of 30 keys each, as a game's character might; the Fox from the reference cache is timed too when it is there.
void bench_animation() {
    const auto per_call = [](int iterations, const auto& body) {
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < iterations; ++i) {
            body(i);
        }
        return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / iterations;
    };
    Skeleton sk;
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
        JointChannels& c = clip.channels[j];
        for (int k = 0; k < 30; ++k) {
            const float t = static_cast<float>(k) / 29.0f;
            c.translation.times.push_back(t);
            c.translation.values.push_back({0.0f, 0.2f + 0.01f * std::sin(t * 6.0f), 0.0f});
            c.rotation.times.push_back(t);
            c.rotation.values.push_back(Quatf::axis_angle({0, 0, 1}, 30.0f * std::sin(t * 6.0f + static_cast<float>(j))));
            c.scale.times.push_back(t);
            c.scale.values.push_back({1, 1, 1});
        }
    }
    clip.duration = 1.0f;
    float sink = 0.0f;
    Pose pose;
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
    std::cerr << "animation, a 24-joint character (3 tracks of 30 keys a joint): sample_clip " << sample << " us, joint matrices " << matrices_only
              << " us, player update + matrices " << single << " us, with a crossfade " << faded << " us (a frame at 60 Hz is 16,667 us) [" << sink << "]\n";
    // The real Fox, when the reference cache has it.
    const char* cache = std::getenv("KIN_REFERENCE_CACHE");
    const std::filesystem::path fox = (cache ? std::filesystem::path{cache} : std::filesystem::path{std::getenv("HOME") ? std::getenv("HOME") : "."} / ".cache/kin/reference") / "fox/Fox.glb";
    std::ifstream in(fox, std::ios::binary);
    if (!in) {
        std::cerr << "animation: the Fox is not in the reference cache (tools/reference/fetch.py): its clips not timed\n";
        return;
    }
    const std::string bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    ModelData data;
    std::string error;
    if (!read_model_data(bytes, fox, data, &error)) {
        std::cerr << "animation: " << error << "\n";
        return;
    }
    assert(data.skeletons.size() == 1 && data.clips.size() == 3);
    const Skeleton& fsk = data.skeletons[0];
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
    std::cerr << "animation, the real Fox (" << fsk.joint_count() << " joints, Walk clip " << data.clips[1].duration << " s): player update + matrices " << fox_us
              << " us, Walk to Run crossfade " << fox_fade << " us [" << sink << "]\n";
}

// --- P1a part 2: 1D blends in the player, a shared pose in the scene, the ECS animator ----------------------------------------

// A clip that swings `joint` about Z by `degrees` and back over `duration` seconds.
AnimClip3D swing_clip(const Skeleton& sk, const char* name, float duration, float degrees, u32 joint = 1) {
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

float max_difference(const Pose& a, const Pose& b) {
    assert(a.joints.size() == b.joints.size());
    float worst = 0.0f;
    for (std::size_t i = 0; i < a.joints.size(); ++i) {
        const Vec3f t = a.joints[i].translation - b.joints[i].translation, s = a.joints[i].scale - b.joints[i].scale;
        worst = std::max({worst, std::fabs(t.x), std::fabs(t.y), std::fabs(t.z), std::fabs(s.x), std::fabs(s.y), std::fabs(s.z)});
        const Quatf q = a.joints[i].rotation, p = b.joints[i].rotation;
        const float d = std::fabs(q.x * p.x + q.y * p.y + q.z * p.z + q.w * p.w);
        worst = std::max(worst, 1.0f - std::min(d, 1.0f));
    }
    return worst;
}

// A one-dimensional blend in the player: the clips of the two points either side of the parameter, each at the same phase of its own
// length, blended by how far the parameter is between them; the phase advances by the time over the duration blended at the parameter
// now (so a change of the parameter never jumps the phase); a parameter outside the points is held at the end; a fade to and from a blend.
void test_player_blend_1d() {
    const auto tube = skin_fixtures::make_tube();
    const Skeleton& sk = tube.skeleton;
    const AnimClip3D walk = swing_clip(sk, "walk", 1.0f, 40.0f, 1), run = swing_clip(sk, "run", 0.6f, -70.0f, 2), jog = swing_clip(sk, "jog", 0.8f, 20.0f, 1);
    // Out of order on purpose (sorted by position), and a null clip, which is dropped.
    const BlendPoint3D points[] = {{&run, 4.5f}, {nullptr, 3.0f}, {&walk, 1.5f}, {&jog, 3.0f}};
    AnimationPlayer3D player{&sk};
    player.play_blend(points, 3.0f);
    assert(player.blending() && player.clip() == nullptr && close(player.time(), 0.0f));
    // At 3.0 exactly: the jog alone (weight 1 on the point), at phase 0.
    player.update(0.0f);
    Pose expected;
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
        Pose a, b;
        sample_clip(walk, sk, phase * 1.0f, true, a);
        sample_clip(jog, sk, phase * 0.8f, true, b);
        expected = blend_poses(a, b, 0.4f);
        assert(max_difference(player.pose(), expected) < 1e-4f);
        assert(close(player.time(), phase, 1e-4f));
    }
    // The parameter moves while it plays: the phase carries on from where it is (it advances at the speed of the new duration).
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
    // A fade from a plain clip to a blend, and from the blend to a clip, is the blend of the two pictures at the fade's progress.
    AnimationPlayer3D fade{&sk};
    fade.play(&walk);
    fade.update(0.1f);
    const BlendPoint3D two[] = {{&walk, 0.0f}, {&run, 1.0f}};
    fade.crossfade_to_blend(two, 0.25f, 0.5f);
    fade.update(0.1f); // alpha 0.2
    {
        Pose from, a, b;
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

// Scene3D::draw_skinned(model): one range of matrices for all the skinned parts, each copy's offset the same; plain parts are drawn as before.
void test_scene_shared_pose() {
    const auto tube = skin_fixtures::make_tube();
    struct FakeMesh final : IMeshBackend {};
    Model model;
    model.skeletons.push_back(tube.skeleton);
    for (int i = 0; i < 4; ++i) {
        ModelPart part;
        part.mesh = Mesh{std::make_shared<FakeMesh>(), 3, 3, {}, {}, {}, i == 3 ? 2u : 3u}; // the last uses two joints only
        part.skin = i == 2 ? -1 : 0;                                                       // and one is not skinned (a rigid prop)
        if (i == 2) {
            part.mesh = Mesh{std::make_shared<FakeMesh>(), 3, 3, {}};
        }
        model.parts.push_back(part);
    }
    std::vector<Mat4f> pose;
    compute_joint_matrices(tube.skeleton, tube.skeleton.rest_pose(), pose);
    Scene3D scene;
    scene.draw_skinned(model, pose, Mat4f::identity(), colors::white, 17);
    // Three skinned copies (0, 1, 3), one plain (2); the pose held once, of the most any part uses (3 joints).
    assert(scene.draws().size() == 4 && scene.instances().size() == 4);
    assert(scene.joint_matrices().size() == 3 && scene.skin_offsets().size() == 3);
    assert(scene.skin_offsets()[0] == 0 && scene.skin_offsets()[1] == 0 && scene.skin_offsets()[2] == 0);
    assert(scene.instances()[0].id == 17 && scene.instances()[2].id == 17);
    // A second character after it: its own range.
    scene.draw_skinned(model, pose, Mat4f::translation({1, 0, 0}));
    assert(scene.joint_matrices().size() == 6 && scene.skin_offsets().size() == 6 && scene.skin_offsets()[3] == 3 && scene.skin_offsets()[5] == 3);
    // Too few matrices: nothing skinned drawn (and the plain part is).
    Scene3D few;
    few.draw_skinned(model, std::span<const Mat4f>{pose}.first(2), Mat4f::identity());
    assert(few.instances().size() == 1 && few.joint_matrices().empty());
}

// --- the ECS animator ---

struct Character {
    std::shared_ptr<const Model> model;
    const Skeleton* skeleton = nullptr;
    const AnimClip3D* clip(const char* name) const { return model->find_clip(name); }
};

Character make_character() {
    const auto tube = skin_fixtures::make_tube();
    struct FakeMesh final : IMeshBackend {};
    auto model = std::make_shared<Model>();
    model->skeletons.push_back(tube.skeleton);
    const Skeleton& sk = model->skeletons[0];
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
    ModelPart part;
    part.name = "tube";
    part.mesh = Mesh{std::make_shared<FakeMesh>(), 3, 3, {}, {}, {}, 3};
    part.skin = 0;
    model->parts.push_back(part);
    Character c;
    c.model = model;
    c.skeleton = &c.model->skeletons[0];
    return c;
}

std::vector<Mat4f> matrices_of(const Pose& pose, const Skeleton& sk) {
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

// An entity with Animator3D plays its clip by name: the joint matrices at every step are those of the CPU maths (the clip sampled at the
// accumulated time, at the speed given), the draw is the model in that pose with the entity's id, and two runs agree to the byte.
void test_ecs_animator_clip() {
    const Character hero = make_character();
    const float dt = 1.0f / 60.0f;
    const AnimClip3D& walk = *hero.clip("walk");
    const auto run = [&](std::vector<u64>* hashes, bool check) {
        kin::EcsWorld world;
        WorldRenderState3D state{world};
        auto& w = world.raw();
        const flecs::entity e = w.entity().set(Transform3D{.position = {1, 2, 3}}).set(SkinnedMeshRenderer{.model = hero.model}).set(Animator3D{.clip = "walk", .speed = 1.5f});
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
                assert(scene.instances()[0].id == static_cast<u32>(e.id() & 0xffffffffu) && scene.instances()[0].id != 0);
                const Vec3f at{scene.instances()[0].transform.m[12], scene.instances()[0].transform.m[13], scene.instances()[0].transform.m[14]};
                assert(close(at, {1, 2, 3}));
                got.assign(scene.joint_matrices().begin(), scene.joint_matrices().end());
            }
            if (check) {
                Pose pose;
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
    world.raw().entity().set(Transform3D{}).set(SkinnedMeshRenderer{.model = hero.model}).set(Animator3D{.clip = "walk"});
    state.animate(0.25f);
    const std::vector<Mat4f> at_quarter = collected_matrices(state);
    world.raw().query<Animator3D>().each([](Animator3D& a) { a.playing = false; });
    state.animate(0.25f);
    assert(max_difference(collected_matrices(state), at_quarter) == 0.0f);
    // Before its first animate(): the rest pose, drawn.
    kin::EcsWorld fresh;
    WorldRenderState3D fresh_state{fresh};
    fresh.raw().entity().set(Transform3D{}).set(SkinnedMeshRenderer{.model = hero.model}).set(Animator3D{.clip = "walk"});
    assert(max_difference(collected_matrices(fresh_state), matrices_of(hero.skeleton->rest_pose(), *hero.skeleton)) < 1e-6f);
    // A renderer with no animator is drawn at rest, and an invisible one is not drawn.
    kin::EcsWorld bare;
    WorldRenderState3D bare_state{bare};
    bare.raw().entity().set(Transform3D{}).set(SkinnedMeshRenderer{.model = hero.model});
    bare.raw().entity().set(Transform3D{}).set(SkinnedMeshRenderer{.model = hero.model, .visible = false});
    assert(collected_matrices(bare_state).size() == 3);
}

// A state machine (kin/anim's) drives the animator: parameters choose the state, a transition crossfades over the length the animator has for
// it, a state with a blend space plays the 1D blend by its parameter, a trigger is consumed by the transition that takes it. The pose
// in the middle of a fade is the blend of the two pictures at its progress, to float rounding, as the CPU maths gives it.
void test_ecs_animator_state_machine() {
    const Character hero = make_character();
    const Skeleton& sk = *hero.skeleton;
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
    world.raw().entity().set(Transform3D{}).set(SkinnedMeshRenderer{.model = hero.model}).set(static_cast<const Animator3D&>(animator));
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
        Pose p;
        sample_clip(idle, sk, idle_t, true, p);
        assert(max_difference(collected_matrices(state), matrices_of(p, sk)) < 1e-5f);
        assert(animator_of().state == "idle" && animator_of().player.fading_from() == nullptr);
    }
    // Speed 3.0: to locomotion at this step (the fade is 0.25 s); the blend is walk at 1.5 and run at 4.5, w = 0.5, duration 0.8.
    animator_of().params.set_float("speed", 3.0f);
    float phase = 0.0f;
    for (int k = 1; k <= 8; ++k) {
        state.animate(dt);
        idle_t = clip_time(idle, idle_t + dt, true);
        phase += dt / 0.8f;
        phase -= std::floor(phase);
        assert(animator_of().state == "locomotion");
        const float alpha = std::min(1.0f, static_cast<float>(k) * dt / 0.25f);
        Pose from, a, b;
        sample_clip(idle, sk, idle_t, true, from);
        sample_clip(walk, sk, phase * 1.0f, true, a);
        sample_clip(run, sk, phase * 0.6f, true, b);
        const Pose expected = blend_poses(from, blend_poses(a, b, 0.5f), alpha);
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
        Pose a, b;
        sample_clip(walk, sk, phase * 1.0f, true, a);
        sample_clip(run, sk, phase * 0.6f, true, b);
        assert(max_difference(collected_matrices(state), matrices_of(blend_poses(a, b, 0.5f), sk)) < 3e-4f);
    }
    // The speed sweeps: the pose follows the weight (a at 1.5 is walk alone, at 4.5 run alone).
    animator_of().params.set_float("speed", 1.5f);
    state.animate(0.0f);
    {
        Pose p;
        sample_clip(walk, sk, animator_of().player.time() * 1.0f, true, p);
        assert(max_difference(collected_matrices(state), matrices_of(p, sk)) < 1e-5f);
    }
    animator_of().params.set_float("speed", 4.5f);
    state.animate(0.0f);
    {
        Pose p;
        sample_clip(run, sk, animator_of().player.time() * 0.6f, true, p);
        assert(max_difference(collected_matrices(state), matrices_of(p, sk)) < 1e-5f);
    }
    // A trigger: to jump from anywhere at once (0.1 s fade), consumed; another transition does not fire in the same step.
    animator_of().params.set_bool("grounded", false);
    animator_of().params.set_trigger("jump");
    state.animate(dt);
    assert(animator_of().state == "jump" && !animator_of().params.triggers.contains("jump"));
    assert(animator_of().player.fading_from() == nullptr); // the fade is from a blend (fading_from names a plain clip, or none)
    assert(close(animator_of().player.fade_progress(), dt / 0.1f, 1e-4f));
    for (int i = 0; i < 8; ++i) {
        state.animate(dt); // 9 steps of 1/60 s: past the fade
    }
    assert(close(animator_of().player.fade_progress(), 1.0f));
    {
        Pose p;
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
        w.raw().entity().set(Transform3D{}).set(SkinnedMeshRenderer{.model = hero.model}).set(static_cast<const Animator3D&>(animator));
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

// Layers as data (a clip on a joint's subtree over the base, by weight), additive layers and root motion moved onto the entity.
void test_ecs_animator_layers_and_root_motion() {
    const Character hero = make_character();
    const Skeleton& sk = *hero.skeleton;
    const float dt = 1.0f / 60.0f;
    {
        Animator3D a;
        a.clip = "walk";
        a.layers = {{.clip = "wave", .mask_joint = "middle", .include_root = true, .weight = 0.75f}};
        kin::EcsWorld world;
        WorldRenderState3D state{world};
        world.raw().entity().set(Transform3D{}).set(SkinnedMeshRenderer{.model = hero.model}).set(a);
        float t_base = 0.0f, t_layer = 0.0f;
        for (int i = 0; i < 20; ++i) {
            state.animate(dt);
            t_base = clip_time(*hero.clip("walk"), t_base + dt, true);
            t_layer = clip_time(*hero.clip("wave"), t_layer + dt, true);
        }
        Pose base, layer;
        sample_clip(*hero.clip("walk"), sk, t_base, true, base);
        sample_clip(*hero.clip("wave"), sk, t_layer, true, layer);
        blend_masked(base, layer, PoseMask::subtree(sk, sk.find("middle"), 1.0f, true), 0.75f);
        assert(max_difference(collected_matrices(state), matrices_of(base, sk)) < 1e-5f);
    }
    {
        // Root motion: 2 m a second along the entity's own x; the entity turned a quarter about Y goes along -z. The root joint stays at rest.
        Animator3D a;
        a.clip = "root_walk";
        a.apply_root_motion = true;
        kin::EcsWorld world;
        WorldRenderState3D state{world};
        const flecs::entity e = world.raw().entity().set(Transform3D{.position = {5, 0, 0}, .rotation = Quatf::axis_angle({0, 1, 0}, 90.0f)})
                                    .set(SkinnedMeshRenderer{.model = hero.model}).set(a);
        for (int i = 0; i < 30; ++i) {
            state.animate(dt);
        }
        const Transform3D* t = e.get<Transform3D>();
        assert(close(t->position, {5.0f, 0.0f, -1.0f}, 1e-3f));
        Pose p;
        p = e.get<Animator3D>()->player.pose();
        assert(close(p.joints[0].translation.x, 0.0f, 1e-6f)); // held at rest in x
    }
}

// --- inverse kinematics (kin/render3d/ik.hpp), CPU only --------------------------------------------------------------------

namespace biped = skin_fixtures::biped;

// A skeleton, its local pose and the world matrices the solvers keep in step.
struct Rig {
    Skeleton sk;
    Pose pose;
    std::vector<Mat4f> world;

    explicit Rig(Skeleton s = skin_fixtures::make_biped()) : sk(std::move(s)) {
        pose = sk.rest_pose();
        refresh();
    }
    void refresh() { compute_world_matrices(sk, pose, world); }
    Vec3f at(int j) const { return {world[static_cast<std::size_t>(j)].m[12], world[static_cast<std::size_t>(j)].m[13], world[static_cast<std::size_t>(j)].m[14]}; }
    float bone(int a, int b) const { return length(at(a) - at(b)); }
    Quatf rotation(int j) const { return rotation_of(world[static_cast<std::size_t>(j)]); }
};

float angle_degrees(Quatf a, Quatf b) {
    const float d = std::min(1.0f, std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w));
    return 2.0f * std::acos(d) * 180.0f / 3.14159265f;
}
float angle_degrees(Vec3f a, Vec3f b) { return std::atan2(length(cross(a, b)), dot(a, b)) * 180.0f / 3.14159265f; }

const TwoBoneChain left_arm{biped::shoulder_l, biped::elbow_l, biped::hand_l};
const TwoBoneChain left_leg{biped::hip_l, biped::knee_l, biped::ankle_l};

// The arm bent a little and turned, so that it has a bend plane and nothing is aligned with an axis.
Rig bent_arm_rig() {
    Rig rig;
    rig.pose.joints[biped::shoulder_l].rotation = Quatf::axis_angle({0, 0, 1}, 12.0f) * Quatf::axis_angle({0, 1, 0}, 20.0f);
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

void check_only_chain_changed(const Pose& before, const Pose& after, std::initializer_list<int> rotated) {
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

// Reachable targets in every direction: the hand arrives, the bones keep their lengths, and nothing but the two rotations changed.
void test_ik_two_bone_reach() {
    Lcg rng;
    for (const TwoBoneChain chain : {left_arm, left_leg}) {
        const Rig base = bent_arm_rig();
        const float l1 = base.bone(chain.root, chain.mid), l2 = base.bone(chain.mid, chain.end);
        for (int i = 0; i < 400; ++i) {
            Rig rig = base;
            const Pose before = rig.pose;
            const Vec3f root = rig.at(chain.root);
            const float d = (0.12f + 0.86f * (rng.next() * 0.5f + 0.5f)) * (l1 + l2);
            const Vec3f target = root + rng.direction() * d;
            TwoBoneOptions o;
            o.pole_mode = PoleMode::Direction;
            o.pole = rng.direction();
            const TwoBoneResult r = solve_two_bone(rig.sk, rig.pose, rig.world, chain, target, o);
            assert(r.solved && r.reached);
            assert(close(rig.at(chain.end), target, 2e-4f));
            assert(close(rig.bone(chain.root, chain.mid), l1, 1e-5f) && close(rig.bone(chain.mid, chain.end), l2, 1e-5f));
            check_only_chain_changed(before, rig.pose, {chain.root, chain.mid});
            // The matrices kept in step are the pose's own.
            std::vector<Mat4f> again;
            compute_world_matrices(rig.sk, rig.pose, again);
            assert(max_difference(again, rig.world) < 1e-5f);
            assert(close(length_of(rig.pose.joints[static_cast<std::size_t>(chain.mid)].rotation), 1.0f, 1e-5f));
        }
    }
}

// Out of reach: the arm is straight at the target; with a stretch it lengthens softly toward the limit and never beyond; a
// minimum bend keeps the elbow from locking.
void test_ik_two_bone_out_of_reach() {
    const Rig base = bent_arm_rig();
    const float reach = base.bone(biped::shoulder_l, biped::elbow_l) + base.bone(biped::elbow_l, biped::hand_l);
    const Vec3f root = base.at(biped::shoulder_l);
    const Vec3f dir = normalize({0.3f, 0.5f, -0.7f});
    {
        Rig rig = base;
        TwoBoneOptions o;
        o.pole_mode = PoleMode::Direction;
        o.pole = {0, 1, 0};
        const TwoBoneResult r = solve_two_bone(rig.sk, rig.pose, rig.world, left_arm, root + dir * 2.0f, o);
        assert(r.solved && !r.reached);
        assert(close(rig.at(biped::hand_l), root + dir * reach, 1e-4f));
        const Vec3f e = rig.at(biped::elbow_l) - root;
        assert(length(cross(e, dir)) < 1e-4f);
        assert(r.bend_degrees < 0.1f);
    }
    {
        // A stretch of 1.3: monotonic and continuous in the target's distance, tends to 1.3 and stays under it.
        TwoBoneOptions o;
        o.max_stretch = 1.3f;
        o.pole_mode = PoleMode::Direction;
        o.pole = {0, 1, 0};
        float last = 0.5f * reach;
        for (int i = 0; i <= 600; ++i) {
            const float d = 0.5f * reach + 0.004f * static_cast<float>(i) * reach;
            Rig rig = base;
            const TwoBoneResult r = solve_two_bone(rig.sk, rig.pose, rig.world, left_arm, root + dir * d, o);
            const float got = length(rig.at(biped::hand_l) - root);
            assert(got <= 1.3f * reach + 1e-4f && got >= last - 1e-5f && got - last < 0.004f * reach * 1.02f + 1e-5f);
            if (d <= reach) {
                assert(close(got, d, 2e-4f) && r.stretch < 1.0001f);
            } else {
                assert(r.stretch > 1.0f - 1e-6f && r.stretch <= 1.3f);
                assert(close(rig.bone(biped::shoulder_l, biped::elbow_l) + rig.bone(biped::elbow_l, biped::hand_l), reach * r.stretch, 1e-4f));
                // Out of reach, the hand is at the end of the stretched chain, on the line to the target.
                assert(close(got, reach * r.stretch, 2e-4f) && length(cross(rig.at(biped::hand_l) - root, dir)) < 2e-4f);
            }
            last = got;
        }
        assert(last > 1.25f * reach);
    }
    {
        // A minimum bend of 10 degrees: the hand stops short of full reach, on the line, with that much bend.
        Rig rig = base;
        TwoBoneOptions o;
        o.min_bend_degrees = 10.0f;
        o.pole_mode = PoleMode::Direction;
        o.pole = {0, 1, 0};
        const TwoBoneResult r = solve_two_bone(rig.sk, rig.pose, rig.world, left_arm, root + dir * 2.0f, o);
        assert(close(r.bend_degrees, 10.0f, 1e-2f));
        const Vec3f h = rig.at(biped::hand_l) - root;
        assert(length(cross(h, dir)) < 1e-4f && length(h) < reach && length(h) > 0.98f * reach);
    }
}

// The elbow goes to the pole's side; opposite poles mirror the bend across the line from the shoulder to the hand; a pole given
// as a point is the same as a direction to it.
void test_ik_two_bone_pole() {
    const Rig base = bent_arm_rig();
    const Vec3f root = base.at(biped::shoulder_l);
    const Vec3f target = root + Vec3f{0.35f, -0.2f, 0.4f};
    const auto solve = [&](Vec3f pole, PoleMode mode) {
        Rig rig = base;
        TwoBoneOptions o;
        o.pole_mode = mode;
        o.pole = pole;
        solve_two_bone(rig.sk, rig.pose, rig.world, left_arm, target, o);
        return rig;
    };
    const Rig up = solve({0, 1, 0}, PoleMode::Direction);
    const Rig down = solve({0, -1, 0}, PoleMode::Direction);
    assert(up.at(biped::elbow_l).y > root.y && down.at(biped::elbow_l).y < root.y);
    // Mirrored about the axis: the midpoint of the two elbows lies on the line root-target.
    const Vec3f axis = normalize(target - root);
    const Vec3f mid = (up.at(biped::elbow_l) + down.at(biped::elbow_l)) * 0.5f - root;
    assert(length(cross(mid, axis)) < 1e-4f);
    assert(close(length(up.at(biped::elbow_l) - root), length(down.at(biped::elbow_l) - root), 1e-5f));
    // Toward the front (+Z) and the back.
    assert(solve({0, 0, 1}, PoleMode::Direction).at(biped::elbow_l).z > root.z);
    assert(solve({0, 0, -1}, PoleMode::Direction).at(biped::elbow_l).z < root.z);
    // A point: seen from the shoulder.
    const Rig by_point = solve(root + Vec3f{0, 3, 0}, PoleMode::Position);
    assert(close(by_point.at(biped::elbow_l), up.at(biped::elbow_l), 1e-5f));
}

// Weight 0 leaves the pose bit for bit; 0.5 lies between; the end reaches at 1.
void test_ik_two_bone_weight() {
    const Rig base = bent_arm_rig();
    const Vec3f target = base.at(biped::shoulder_l) + Vec3f{0.1f, -0.5f, 0.45f};
    TwoBoneOptions o;
    o.pole_mode = PoleMode::Direction;
    o.pole = {0, 1, 0};
    Rig none = base;
    o.weight = 0.0f;
    assert(!solve_two_bone(none.sk, none.pose, none.world, left_arm, target, o).solved);
    assert(std::memcmp(none.pose.joints.data(), base.pose.joints.data(), base.pose.joints.size() * sizeof(JointPose)) == 0);
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
        const float whole = angle_degrees(base.pose.joints[static_cast<std::size_t>(j)].rotation, full.pose.joints[static_cast<std::size_t>(j)].rotation);
        const float part = angle_degrees(base.pose.joints[static_cast<std::size_t>(j)].rotation, half.pose.joints[static_cast<std::size_t>(j)].rotation);
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
        TwoBoneOptions o;
        o.max_stretch = 1.2f;
        o.min_bend_degrees = 3.0f;
        if (with_pole) {
            o.pole_mode = PoleMode::Direction;
            o.pole = {0.2f, 1, -0.3f};
        }
        solve_two_bone(rig.sk, rig.pose, rig.world, left_arm, target, o);
        const Pose once = rig.pose;
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

// The largest change of the chain's pose (the angle of either rotation, and the elbow's direction from the shoulder) between
// two consecutive solves.
struct PoseStep {
    float root_degrees = 0.0f, mid_degrees = 0.0f, elbow_degrees = 0.0f;
    float worst() const { return std::max({root_degrees, mid_degrees, elbow_degrees}); }
};
PoseStep pose_step(const Rig& a, const Rig& b, const TwoBoneChain& c) {
    PoseStep s;
    s.root_degrees = angle_degrees(a.pose.joints[static_cast<std::size_t>(c.root)].rotation, b.pose.joints[static_cast<std::size_t>(c.root)].rotation);
    s.mid_degrees = angle_degrees(a.pose.joints[static_cast<std::size_t>(c.mid)].rotation, b.pose.joints[static_cast<std::size_t>(c.mid)].rotation);
    s.elbow_degrees = angle_degrees(a.at(c.mid) - a.at(c.root), b.at(c.mid) - b.at(c.root));
    return s;
}

// A target sweeping through the cases where a solver flips: through the root, along the chain's axis, round and through the pole.
// The change from one step to the next stays small (proportional to the step), the pose never jumps.
void test_ik_two_bone_continuity() {
    // Steps of `steps` over u in [0, 1]; `jumps_allowed` is how many steps may turn a joint more than `max_step_degrees` (the
    // twist at a fold's very centre: 180 degrees with the joints in the same places); the elbow and hand never move more than
    // `max_move` in one step.
    const auto sweep = [&](const char* what, Rig start, const TwoBoneChain& chain, TwoBoneOptions o, int steps, auto target_at, float max_step_degrees,
                           int jumps_allowed = 0, float max_move = 0.02f) {
        Rig previous;
        float worst = 0.0f, worst_move = 0.0f;
        int jumps = 0;
        for (int i = 0; i <= steps; ++i) {
            Rig rig = start;
            const Vec3f target = target_at(static_cast<float>(i) / static_cast<float>(steps));
            const TwoBoneResult r = solve_two_bone(rig.sk, rig.pose, rig.world, chain, target, o);
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
                worst_move = std::max({worst_move, length(rig.at(chain.mid) - previous.at(chain.mid)), length(rig.at(chain.end) - previous.at(chain.end))});
            }
            previous = std::move(rig);
        }
        std::cout << "  ik continuity (" << what << "): worst step " << worst << " degrees, " << jumps << " jump(s) over " << max_step_degrees
                  << ", elbow/hand move at most " << worst_move << " a step" << std::endl;
        assert(jumps <= jumps_allowed && worst_move <= max_move);
    };
    const Rig base = bent_arm_rig();
    const Vec3f root = base.at(biped::shoulder_l);
    TwoBoneOptions pole;
    pole.pole_mode = PoleMode::Direction;
    pole.pole = {0, 1, 0};
    // Through the root, equal bones (a fold flat on itself is the only pose there): continuous. (Not sampled at the root's very
    // point, where every direction is as good as another and the solver keeps the chain's own.)
    Rig equal = base;
    equal.pose.joints[biped::hand_l].translation = {0.5f, 0, 0};
    equal.refresh();
    const Vec3f line = normalize({0.4f, 0.1f, 0.5f});
    sweep("through the root, equal bones", equal, left_arm, pole, 1999, [&](float u) { return root + line * (u * 1.6f - 0.8f); }, 6.0f, 1);
    // The same with no pole at all (the pose's own bend plane).
    sweep("through the root, no pole", equal, left_arm, {}, 1999, [&](float u) { return root + line * (u * 1.6f - 0.8f); }, 6.0f, 1);
    // Along the arm's own axis, to well out of reach, straight through the full extension (the elbow's angle goes with the
    // square root of the distance short of it, so a step of 0.1% of the reach turns it a few degrees: that is the bound).
    const Vec3f axis = normalize(base.at(biped::hand_l) - root);
    sweep("along the axis, past full reach", base, left_arm, pole, 2000, [&](float u) { return root + axis * (u * 2.1f + 0.1f); }, 6.0f);
    // Through the root of an arm whose bones differ (a hole of 5 cm round the shoulder where the hand cannot be): the chain
    // turns end over end within those 5 cm, steeply (a step of 0.8 mm there is about 1 degree per 0.01 of the hole's radius) but
    // without a jump.
    sweep("through the root, unequal bones", base, left_arm, pole, 1999, [&](float u) { return root + line * (u * 1.6f - 0.8f); }, 15.0f, 0, 0.1f);
    sweep("along the axis, past full reach, stretch", base, left_arm, [&] { TwoBoneOptions o = pole; o.max_stretch = 1.25f; return o; }(), 2000,
          [&](float u) { return root + axis * (u * 2.1f + 0.1f); }, 6.0f);
    // A straight chain (the rest pose: no bend plane of its own) with no pole: still finite and continuous.
    Rig straight;
    sweep("straight chain, no pole", straight, left_arm, {}, 2000, [&](float u) { return root + Vec3f{0.3f + 0.4f * std::cos(u * 6.2831853f), 0.4f * std::sin(u * 6.2831853f), 0.2f}; }, 4.0f);
    // Round the pole: the target on a cone about the pole's direction, wide and narrow.
    for (const float half_angle : {40.0f, 8.0f, 2.0f}) {
        const float c = std::cos(half_angle * 3.14159265f / 180.0f), s = std::sin(half_angle * 3.14159265f / 180.0f);
        sweep("round the pole", base, left_arm, pole, 3000,
              [&](float u) { return root + Vec3f{s * std::cos(u * 6.2831853f), c, s * std::sin(u * 6.2831853f)} * 0.7f; }, 3.0f);
    }
    // Through the pole's axis, straight across it. The elbow has to swing to the other side; the pull toward the pose's own bend
    // makes it a turn over a few degrees of the target's travel, not a jump.
    for (const Vec3f across : {Vec3f{1, 0, 0}, Vec3f{0.6f, 0, 0.8f}, Vec3f{0, 0, 1}}) {
        sweep("through the pole's axis", base, left_arm, pole, 4000, [&](float u) { return root + Vec3f{0, 0.7f, 0} + across * (u * 0.6f - 0.3f); }, 12.0f);
    }
}

// A scaled, turned model and a scaled joint above the chain: the maths is done in the space of the root's parent, so the hand
// still arrives and the bones keep the lengths they are drawn at there.
void test_ik_two_bone_scaled_and_turned() {
    Rig rig = bent_arm_rig();
    rig.sk.root_transform = Mat4f::trs({1.0f, 2.0f, -3.0f}, Quatf::axis_angle({0.3f, 1, 0.2f}, 70.0f), {1.5f, 0.6f, 2.0f});
    rig.pose.joints[biped::chest].scale = {1.3f, 0.8f, 1.1f};
    rig.pose.joints[biped::spine].rotation = Quatf::axis_angle({0, 0, 1}, 15.0f);
    rig.refresh();
    const Mat4f parent = rig.world[biped::chest];
    const Mat4f inverse = parent.inverse();
    const auto lengths_in_parent = [&](const Rig& r) {
        const Vec3f a = inverse.apply(r.at(biped::shoulder_l)), b = inverse.apply(r.at(biped::elbow_l)), c = inverse.apply(r.at(biped::hand_l));
        return std::array<float, 2>{length(b - a), length(c - b)};
    };
    const auto before = lengths_in_parent(rig);
    Lcg rng;
    for (int i = 0; i < 200; ++i) {
        Rig r = rig;
        // A target inside the reach, as the parent's space sees it.
        const Vec3f local = inverse.apply(r.at(biped::shoulder_l)) + rng.direction() * (0.1f + 0.8f * (rng.next() * 0.5f + 0.5f)) * (before[0] + before[1]);
        const Vec3f target = parent.apply(local);
        TwoBoneOptions o;
        o.pole_mode = PoleMode::Direction;
        o.pole = rng.direction();
        const TwoBoneResult res = solve_two_bone(r.sk, r.pose, r.world, left_arm, target, o);
        assert(res.solved && res.reached);
        assert(close(r.at(biped::hand_l), target, 1e-3f));
        const auto after = lengths_in_parent(r);
        assert(close(after[0], before[0], 2e-5f) && close(after[1], before[1], 2e-5f));
        std::vector<Mat4f> again;
        compute_world_matrices(r.sk, r.pose, again);
        assert(max_difference(again, r.world) < 1e-4f);
    }
}

// The end joint is turned to a given orientation (a hand to a hold's surface), by `rotation_weight`; without it the hand only
// rides on the forearm.
void test_ik_two_bone_orientation() {
    const Rig base = bent_arm_rig();
    const Vec3f target = base.at(biped::shoulder_l) + Vec3f{0.3f, -0.2f, 0.5f};
    const Quatf want = Quatf::axis_angle({0.4f, 1, -0.3f}, 110.0f);
    TwoBoneOptions o;
    o.pole_mode = PoleMode::Direction;
    o.pole = {0, 1, 0};
    Rig plain = base;
    solve_two_bone(plain.sk, plain.pose, plain.world, left_arm, target, o);
    // Unrotated: the hand's local rotation is what the pose had.
    assert(plain.pose.joints[biped::hand_l].rotation == base.pose.joints[biped::hand_l].rotation);
    o.match_rotation = true;
    o.end_rotation = want;
    Rig matched = base;
    const TwoBoneResult r = solve_two_bone(matched.sk, matched.pose, matched.world, left_arm, target, o);
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

// A look-at turns the head's aim axis to the target; the weights share the turn, limits hold, the weight scales it, a target
// going round behind never flips the head, and smoothing approaches.
void test_ik_look_at() {
    const std::vector<LookAtJoint> chain = {{.joint = biped::spine, .weight = 0.2f}, {.joint = biped::neck, .weight = 0.3f}, {.joint = biped::head, .weight = 0.5f}};
    const Rig base;
    const Vec3f head = base.at(biped::head);
    const auto aim_of = [](const Rig& r) { return normalize(r.world[biped::head].apply_vector({0, 0, 1})); };
    const auto yaw_of = [](Vec3f d) { return std::atan2(d.x, d.z) * 180.0f / 3.14159265f; };
    const auto pitch_of = [](Vec3f d) { return std::asin(std::clamp(d.y, -1.0f, 1.0f)) * 180.0f / 3.14159265f; };

    // Unlimited: the aim arrives, and the shares are the weights'.
    {
        Rig r = base;
        const Vec3f target = head + normalize(Vec3f{3, 0.8f, 2}) * 4.0f;
        const LookAtResult res = solve_look_at(r.sk, r.pose, r.world, chain, target);
        assert(res.solved);
        assert(angle_degrees(aim_of(r), normalize(target - head)) < 0.05f);
        // A pure yaw (target level with the eyes): the three joints turned 20%, 30%, 50% of it.
        Rig level = base;
        const Vec3f level_target = head + normalize(Vec3f{1, 0, 1}) * 4.0f;
        solve_look_at(level.sk, level.pose, level.world, chain, level_target);
        const float total = angle_degrees(Quatf{}, level.pose.joints[biped::spine].rotation) + angle_degrees(Quatf{}, level.pose.joints[biped::neck].rotation) +
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
        const std::vector<LookAtJoint> limited = {{.joint = biped::spine, .weight = 0.2f, .max_yaw_degrees = 5.0f, .max_pitch_up_degrees = 5.0f, .max_pitch_down_degrees = 5.0f},
                                                  {.joint = biped::neck, .weight = 0.3f, .max_yaw_degrees = 15.0f, .max_pitch_up_degrees = 15.0f, .max_pitch_down_degrees = 15.0f},
                                                  {.joint = biped::head, .weight = 0.5f, .max_yaw_degrees = 25.0f, .max_pitch_up_degrees = 25.0f, .max_pitch_down_degrees = 25.0f}};
        Rig r = base;
        LookAtOptions o;
        o.release_degrees = 0.0f;
        solve_look_at(r.sk, r.pose, r.world, limited, head + Vec3f{4, 0, 0}, o); // 90 degrees to the side
        assert(close(angle_degrees(Quatf{}, r.pose.joints[biped::spine].rotation), 5.0f, 0.05f));
        assert(close(angle_degrees(Quatf{}, r.pose.joints[biped::neck].rotation), 15.0f, 0.05f));
        assert(close(angle_degrees(Quatf{}, r.pose.joints[biped::head].rotation), 25.0f, 0.05f));
        assert(close(yaw_of(aim_of(r)), 45.0f, 0.1f));
        // Pitch: 10 degrees up and 10 down of a chain whose joints take 5, 15 and 25 are all inside the limits: reached.
        Rig up = base;
        solve_look_at(up.sk, up.pose, up.world, limited, head + Vec3f{0, std::tan(10.0f * 3.14159265f / 180.0f) * 4, 4});
        assert(close(pitch_of(aim_of(up)), 10.0f, 0.05f));
        // A joint with different limits up and down.
        const std::vector<LookAtJoint> asym = {{.joint = biped::head, .weight = 1.0f, .max_yaw_degrees = 60.0f, .max_pitch_up_degrees = 10.0f, .max_pitch_down_degrees = 40.0f}};
        Rig u = base, d = base;
        solve_look_at(u.sk, u.pose, u.world, asym, head + Vec3f{0, 4, 1}, o);
        solve_look_at(d.sk, d.pose, d.world, asym, head + Vec3f{0, -4, 1}, o);
        assert(close(pitch_of(aim_of(u)), 10.0f, 0.1f) && close(pitch_of(aim_of(d)), -40.0f, 0.1f));
    }
    // Weight: 0 is bit for bit, 0.5 half the turn; the eye offset moves the point looking.
    {
        Rig none = base, half = base, full = base;
        LookAtOptions o;
        o.weight = 0.0f;
        const Vec3f target = head + Vec3f{3, 1, 3};
        assert(!solve_look_at(none.sk, none.pose, none.world, chain, target, o).solved);
        assert(std::memcmp(none.pose.joints.data(), base.pose.joints.data(), base.pose.joints.size() * sizeof(JointPose)) == 0);
        solve_look_at(full.sk, full.pose, full.world, chain, target);
        o.weight = 0.5f;
        solve_look_at(half.sk, half.pose, half.world, chain, target, o);
        assert(close(angle_degrees(aim_of(base), aim_of(half)), angle_degrees(aim_of(base), aim_of(full)) * 0.5f, 0.1f));
    }
    // Round behind: a target circling the head at eye height. The aim follows to the limit, is eased back to forward as the
    // target goes behind, and comes round the other side without a jump: no step of the aim bigger than a little over the
    // target's own.
    {
        const std::vector<LookAtJoint> neck = {{.joint = biped::neck, .weight = 0.4f, .max_yaw_degrees = 50.0f, .max_pitch_up_degrees = 30.0f, .max_pitch_down_degrees = 30.0f},
                                               {.joint = biped::head, .weight = 0.6f, .max_yaw_degrees = 70.0f, .max_pitch_up_degrees = 40.0f, .max_pitch_down_degrees = 40.0f}};
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
        std::cout << "  ik look-at round behind: worst step of the aim " << worst << " degrees (the target moves 0.5), at the back " << behind
                  << " from forward" << std::endl;
        assert(worst < 3.0f && behind < 1.0f);
    }
    // Smoothing: toward a fixed target the aim approaches monotonically and arrives; the speed cap holds; dt 0 holds.
    {
        LookAtOptions o;
        o.smooth_seconds = 0.2f;
        LookAtState state;
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
        // The time constant: after one step the aim has hardly moved, after 0.2 s (the constant) a fraction 1 / e of the way is left.
        LookAtState timed;
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
        LookAtOptions capped;
        capped.max_speed_degrees = 90.0f;
        LookAtState s2;
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

    GroundQuery query() {
        return [this](Vec3f from, Vec3f to) -> std::optional<GroundHit> {
            ++casts;
            // March down the segment to the first sample under the surface, then bisect.
            const auto below = [&](float t) {
                const Vec3f p = from + (to - from) * t;
                return p.y <= height(p.x, p.z);
            };
            if (below(0.0f)) {
                return GroundHit{from, normal};
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
            return GroundHit{{p.x, height(p.x, p.z), p.z}, normal};
        };
    }
};

// Foot placement on a fake ground: flat ground moves nothing; a step down lowers the pelvis by the drop and puts the foot on it;
// a slope tilts the foot to the normal up to the clamp; the pelvis offset eases and a foot rising onto a step is not eased.
void test_ik_foot_placement() {
    const Rig base;
    const std::array<FootLeg, 2> legs = {FootLeg{{biped::hip_l, biped::knee_l, biped::ankle_l}, 0.05f}, FootLeg{{biped::hip_r, biped::knee_r, biped::ankle_r}, 0.05f}};
    TwoBoneOptions knee;
    knee.pole_mode = PoleMode::Direction;
    knee.pole = {0, 0, 1};
    FootPlacementOptions options;
    options.smooth_seconds = 0.0f;

    // Flat: nothing moves, to rounding, and the pelvis stays.
    {
        FakeGround ground{[](float, float) { return 0.0f; }};
        Rig r = base;
        const FootPlan plan = plan_foot_placement(r.sk, r.pose, r.world, legs, ground.query(), options);
        assert(ground.casts == 2 && plan.pelvis_offset == 0.0f);
        for (std::size_t i = 0; i < 2; ++i) {
            assert(plan.feet[i].grounded && close(plan.feet[i].target, r.at(legs[i].leg.end), 1e-5f));
            assert(angle_degrees(plan.feet[i].rotation, r.rotation(legs[i].leg.end)) < 0.01f);
        }
        apply_foot_placement(r.sk, r.pose, r.world, biped::pelvis, legs, plan, knee);
        assert(max_difference(r.world, base.world) < 1e-4f);
    }
    // A step down under the left foot (x > 0): the pelvis goes down by the drop, the left foot lands on it, the right stays.
    {
        FakeGround ground{[](float x, float) { return x > 0.0f ? -0.2f : 0.0f; }};
        Rig r = base;
        const FootPlan plan = plan_foot_placement(r.sk, r.pose, r.world, legs, ground.query(), options);
        assert(close(plan.pelvis_offset, -0.2f, 1e-5f));
        assert(close(plan.feet[0].target.y, 0.05f - 0.2f, 1e-5f) && close(plan.feet[1].target.y, 0.05f, 1e-5f));
        apply_foot_placement(r.sk, r.pose, r.world, biped::pelvis, legs, plan, knee);
        assert(close(r.at(biped::pelvis).y, 0.8f, 1e-5f));
        assert(close(r.at(biped::ankle_l), plan.feet[0].target, 2e-4f) && close(r.at(biped::ankle_r), plan.feet[1].target, 2e-4f));
        // The leg that was long enough stays straight; the other bends, its knee forward (+Z, the pole).
        assert(r.at(biped::knee_r).z > base.at(biped::knee_r).z + 0.01f);
        // Bones kept their lengths.
        assert(close(r.bone(biped::hip_l, biped::knee_l), 0.5f, 1e-5f) && close(r.bone(biped::knee_r, biped::ankle_r), 0.45f, 1e-5f));
    }
    // A step up under the left foot: the pelvis stays (a raise is off by default), the foot is lifted onto it; with a raise
    // allowed the pelvis takes the lowest rise.
    {
        FakeGround ground{[](float x, float) { return x > 0.0f ? 0.15f : 0.0f; }};
        Rig r = base;
        FootPlan plan = plan_foot_placement(r.sk, r.pose, r.world, legs, ground.query(), options);
        assert(plan.pelvis_offset == 0.0f && close(plan.feet[0].target.y, 0.2f, 1e-5f));
        FakeGround both{[](float, float) { return 0.1f; }};
        FootPlacementOptions raise = options;
        raise.max_pelvis_raise = 0.3f;
        plan = plan_foot_placement(r.sk, r.pose, r.world, legs, both.query(), raise);
        assert(close(plan.pelvis_offset, 0.1f, 1e-5f));
    }
    // A slope of 20 degrees (normal tilted toward +X): the foot's up follows the normal; a slope of 50 is clamped to 30.
    for (const float degrees : {20.0f, 50.0f}) {
        const float rad = degrees * 3.14159265f / 180.0f;
        const Vec3f normal{std::sin(rad), std::cos(rad), 0};
        FakeGround ground{[&](float x, float) { return -std::tan(rad) * x; }, normal};
        Rig r = base;
        const FootPlan plan = plan_foot_placement(r.sk, r.pose, r.world, legs, ground.query(), options);
        // (The tilted foot's ankle sits a little off the straight-down place: a leg at its full length wants a little stretch.)
        TwoBoneOptions stretchy = knee;
        stretchy.max_stretch = 1.5f;
        apply_foot_placement(r.sk, r.pose, r.world, biped::pelvis, legs, plan, stretchy);
        const float expect = std::min(degrees, options.max_tilt_degrees);
        const Vec3f foot_up = normalize(r.world[biped::ankle_l].apply_vector({0, 1, 0}));
        assert(close(angle_degrees(foot_up, {0, 1, 0}), expect, 0.1f));
        assert(close(angle_degrees(foot_up, normal), degrees - expect, 0.1f));
        assert(close(r.at(biped::ankle_l), plan.feet[0].target, 2e-4f));
    }
    // The pelvis eases to its offset: a drop is snapped to on the first call, then the ground comes back and the offset rises by
    // the time constant; a foot that falls away eases down, one that rises is lifted at once.
    {
        FootPlacementOptions eased = options;
        eased.smooth_seconds = 0.1f;
        FootPlacementState state;
        float x_drop = 1.0f;
        FakeGround ground{[&](float x, float) { return x > 0.0f ? -0.2f * x_drop : 0.0f; }};
        Rig r = base;
        FootPlan plan = plan_foot_placement(r.sk, r.pose, r.world, legs, ground.query(), eased, &state, 1.0f / 60.0f);
        assert(close(plan.pelvis_offset, -0.2f, 1e-5f));
        x_drop = 0.0f;
        float last = plan.pelvis_offset;
        for (int i = 0; i < 20; ++i) {
            plan = plan_foot_placement(r.sk, r.pose, r.world, legs, ground.query(), eased, &state, 1.0f / 60.0f);
            assert(plan.pelvis_offset > last - 1e-6f && plan.pelvis_offset <= 0.0f);
            last = plan.pelvis_offset;
        }
        assert(last > -0.2f * std::exp(-20.0f / 60.0f / 0.1f) - 1e-3f && last < -0.2f * std::exp(-20.0f / 60.0f / 0.1f) + 1e-3f);
        // The foot: ground at 0, then it falls away to -0.2 (eases), then returns (at once).
        FootPlacementState fs;
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
        const FootPlan plan = plan_foot_placement(r.sk, r.pose, r.world, legs, ground.query(), options);
        assert(!plan.feet[0].grounded && plan.feet[1].grounded && plan.pelvis_offset == 0.0f);
        assert(close(plan.feet[0].target, r.at(biped::ankle_l), 1e-6f));
    }
}

// FABRIK: the tip reaches, bones keep their lengths, out of reach the chain is straight at the target, a free root follows.
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
        const FabrikResult res = solve_chain_fabrik(r.sk, r.pose, r.world, spine, target);
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
        const FabrikResult res = solve_chain_fabrik(r.sk, r.pose, r.world, spine, root + dir * 3.0f);
        assert(res.solved && !res.reached);
        assert(close(r.at(biped::head), root + dir * reach, 1e-3f));
        assert(length(cross(r.at(biped::neck) - root, dir)) < 1e-3f);
    }
    {
        Rig r = base;
        FabrikOptions o;
        o.fix_root = false;
        const Vec3f target = root + Vec3f{2.0f, 1.0f, 0.0f};
        const FabrikResult res = solve_chain_fabrik(r.sk, r.pose, r.world, spine, target, o);
        assert(res.reached && close(r.at(biped::head), target, 1e-3f));
        assert(length(r.at(biped::pelvis) - root) > 0.5f);
        for (std::size_t k = 0; k + 1 < spine.size(); ++k) {
            assert(close(r.bone(spine[k], spine[k + 1]), base.bone(spine[k], spine[k + 1]), 1e-4f));
        }
    }
    {
        Rig r = base;
        FabrikOptions o;
        o.weight = 0.0f;
        assert(!solve_chain_fabrik(r.sk, r.pose, r.world, spine, root + Vec3f{0.3f, 0.3f, 0.3f}, o).solved);
        assert(std::memcmp(r.pose.joints.data(), base.pose.joints.data(), base.pose.joints.size() * sizeof(JointPose)) == 0);
    }
}

// Solvers in order, each on what the one before made: a second solve of the same chain wins, a foot placer and an arm see the
// pelvis where the first put it; and bad input does nothing and says so.
void test_ik_order_and_bad_input() {
    const Rig base = bent_arm_rig();
    const Vec3f a = base.at(biped::shoulder_l) + Vec3f{0.4f, 0.1f, 0.3f}, b = base.at(biped::shoulder_l) + Vec3f{0.2f, -0.5f, 0.4f};
    for (const bool a_first : {true, false}) {
        Rig r = base;
        solve_two_bone(r.sk, r.pose, r.world, left_arm, a_first ? a : b);
        solve_two_bone(r.sk, r.pose, r.world, left_arm, a_first ? b : a);
        assert(close(r.at(biped::hand_l), a_first ? b : a, 2e-4f));
    }
    // Pelvis lowered, then an arm reaching a fixed point: the arm's shoulder has moved with the pelvis and the hand still arrives.
    {
        Rig r = base;
        const std::array<FootLeg, 1> legs = {FootLeg{{biped::hip_l, biped::knee_l, biped::ankle_l}, 0.05f}};
        FakeGround ground{[](float, float) { return -0.15f; }};
        const FootPlan plan = plan_foot_placement(r.sk, r.pose, r.world, legs, ground.query(), {});
        apply_foot_placement(r.sk, r.pose, r.world, biped::pelvis, legs, plan);
        assert(close(r.at(biped::shoulder_l).y, base.at(biped::shoulder_l).y - 0.15f, 1e-5f));
        const Vec3f hold = base.at(biped::shoulder_l) + Vec3f{0.3f, 0.4f, 0.3f};
        solve_two_bone(r.sk, r.pose, r.world, left_arm, hold);
        assert(close(r.at(biped::hand_l), hold, 2e-4f));
    }
    // Bad chains: nothing happens.
    Rig r = base;
    assert(!solve_two_bone(r.sk, r.pose, r.world, {biped::hand_l, biped::elbow_l, biped::shoulder_l}, {1, 1, 1}).solved);
    assert(!solve_two_bone(r.sk, r.pose, r.world, {biped::shoulder_l, biped::shoulder_l, biped::hand_l}, {1, 1, 1}).solved);
    assert(!solve_two_bone(r.sk, r.pose, r.world, {-1, 3, 4}, {1, 1, 1}).solved);
    assert(!solve_two_bone(r.sk, r.pose, r.world, {0, 1, 99}, {1, 1, 1}).solved);
    // A zero-length bone.
    r.pose.joints[biped::hand_l].translation = {};
    r.refresh();
    assert(!solve_two_bone(r.sk, r.pose, r.world, left_arm, {1, 1, 1}).solved);
}

// --- IK in the ECS animator ---

struct BipedCharacter {
    std::shared_ptr<const Model> model;
    const Skeleton* skeleton = nullptr;
    std::vector<Mat4f> rest_world; // the joints' world matrices at rest (a joint matrix times these is the world matrix)
};

BipedCharacter make_biped_character() {
    struct FakeMesh final : IMeshBackend {};
    auto model = std::make_shared<Model>();
    model->skeletons.push_back(skin_fixtures::make_biped());
    const Skeleton& sk = model->skeletons[0];
    // A clip that sways the spine, so that the pose the IK sees is not the rest pose.
    AnimClip3D sway;
    sway.name = "sway";
    sway.duration = 1.0f;
    sway.channels.resize(sk.joint_count());
    sway.channels[biped::spine].rotation.times = {0.0f, 0.5f, 1.0f};
    sway.channels[biped::spine].rotation.values = {Quatf{}, Quatf::axis_angle({0, 0, 1}, 15.0f), Quatf{}};
    model->clips.push_back(sway);
    ModelPart part;
    part.name = "body";
    part.mesh = Mesh{std::make_shared<FakeMesh>(), 3, 3, {}, {}, {}, sk.joint_count()};
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

Vec3f world_position_of(const BipedCharacter& c, std::span<const Mat4f> joint_matrices, int joint, const Mat4f& entity) {
    return entity.apply((joint_matrices[static_cast<std::size_t>(joint)] * c.rest_world[static_cast<std::size_t>(joint)]).apply({0, 0, 0}));
}

IKConstraint hand_constraint(float fade = 0.0f) {
    IKConstraint k;
    k.name = "hand";
    k.kind = IKKind::TwoBone;
    k.joints = {"shoulder_L", "elbow_L", "hand_L"};
    k.fade_seconds = fade;
    return k;
}

// A hand reaches a target given in the world, for an entity that is moved, turned and scaled; the matrices drawn are the IK pose's.
void test_ecs_ik_reaches_world_target() {
    const BipedCharacter hero = make_biped_character();
    for (const float scale : {1.0f, 1.7f}) {
        kin::EcsWorld world;
        WorldRenderState3D state{world};
        auto& w = world.raw();
        const flecs::entity e = w.entity()
                                    .set(Transform3D{.position = {2, 0, 5}, .rotation = Quatf::axis_angle({0, 1, 0}, 70.0f), .scale = {scale, scale, scale}})
                                    .set(SkinnedMeshRenderer{.model = hero.model})
                                    .set(Animator3D{.clip = "sway"});
        Animator3D& a = *e.get_mut<Animator3D>();
        a.add_ik(hand_constraint());
        // A point in front of the character's left shoulder, in the world.
        const Mat4f entity_matrix = e.get<Transform3D>()->matrix();
        const Vec3f shoulder = entity_matrix.apply(hero.skeleton->rest[biped::shoulder_l].translation + Vec3f{0, 1.45f, 0});
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
        assert(close(entity_matrix.apply(Vec3f{after.ik_world[biped::hand_l].m[12], after.ik_world[biped::hand_l].m[13], after.ik_world[biped::hand_l].m[14]}), target, 2e-4f * scale));
        // The entity moved and turned: the hand stays on the target in the world (the target is a world point).
        e.set(Transform3D{.position = {2.2f, 0, 5.1f}, .rotation = Quatf::axis_angle({0, 1, 0}, 85.0f), .scale = {scale, scale, scale}});
        const Mat4f moved = e.get<Transform3D>()->matrix();
        got = ik_frame(state, 1.0f / 60.0f);
        const Vec3f shoulder_now = world_position_of(hero, got, biped::shoulder_l, moved);
        if (length(target - shoulder_now) < 0.9f * scale) {
            assert(close(world_position_of(hero, got, biped::hand_l, moved), target, 2e-4f * scale));
        }
        // The elbow is on the pole's side: the pole was model -Z, so the elbow is behind the line shoulder-hand in the model.
        const Mat4f inv = moved.inverse();
        const Vec3f s = inv.apply(world_position_of(hero, got, biped::shoulder_l, moved)), el = inv.apply(world_position_of(hero, got, biped::elbow_l, moved)),
                    h = inv.apply(world_position_of(hero, got, biped::hand_l, moved));
        const Vec3f axis = normalize(h - s);
        const Vec3f off = (el - s) - axis * dot(el - s, axis);
        assert(off.z < -0.01f);
    }
}

// An entity as the target follows it; the entity going away is safe and lets go over the fade (from the last place it was);
// a fade in takes its time and a set_weight(0) lets go.
void test_ecs_ik_entity_target_and_fades() {
    const BipedCharacter hero = make_biped_character();
    kin::EcsWorld world;
    WorldRenderState3D state{world};
    auto& w = world.raw();
    const flecs::entity hold = w.entity().set(Transform3D{.position = {0.5f, 1.45f, 0.5f}});
    const flecs::entity e = w.entity().set(Transform3D{}).set(SkinnedMeshRenderer{.model = hero.model}).set(Animator3D{.clip = "sway"});
    Animator3D& a = *e.get_mut<Animator3D>();
    a.add_ik(hand_constraint(0.5f)).set_target(hold, {0, 0.05f, 0}).set_pole_direction({0, 0, -1});
    const float dt = 0.1f;
    const Vec3f rest_hand = world_position_of(hero, matrices_of(hero.skeleton->rest_pose(), *hero.skeleton), biped::hand_l, Mat4f{});
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
    // Destroyed: safe, and the hand lets go from where the hold was, over the fade (the weight falls steadily, the hand is
    // still on the old place at first and then on the clips' pose).
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
    Pose clip_pose = after.player.pose();
    assert(close(world_position_of(hero, got, biped::hand_l, Mat4f{}), world_position_of(hero, matrices_of(clip_pose, *hero.skeleton), biped::hand_l, Mat4f{}), 1e-4f));
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
    assert(close(world_position_of(hero, got, biped::hand_l, Mat4f{}), world_position_of(hero, matrices_of(e.get<Animator3D>()->player.pose(), *hero.skeleton), biped::hand_l, Mat4f{}), 1e-4f));
}

// Constraints run in list order, each on the pose the one before left; an animator with none is exactly what it was (the
// matrices are those of the player's pose, to the bit, and no IK pose is kept).
void test_ecs_ik_order_and_off_path() {
    const BipedCharacter hero = make_biped_character();
    const auto run = [&](bool with_ik, bool swapped) {
        kin::EcsWorld world;
        WorldRenderState3D state{world};
        auto& w = world.raw();
        const flecs::entity e = w.entity().set(Transform3D{.position = {1, 0, 0}}).set(SkinnedMeshRenderer{.model = hero.model}).set(Animator3D{.clip = "sway"});
        if (with_ik) {
            Animator3D& a = *e.get_mut<Animator3D>();
            IKConstraint first = hand_constraint();
            first.name = "first";
            first.target = {IKTarget::Kind::World, {1.4f, 1.5f, 0.4f}, 0};
            IKConstraint second = hand_constraint();
            second.name = "second";
            second.target = {IKTarget::Kind::World, {1.2f, 1.1f, 0.5f}, 0};
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
        const flecs::entity e = world.raw().entity().set(Transform3D{}).set(SkinnedMeshRenderer{.model = hero.model}).set(Animator3D{.clip = "sway"});
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
            const flecs::entity e = w.entity().set(Transform3D{.position = {static_cast<float>(n), 0, 0}, .rotation = Quatf::axis_angle({0, 1, 0}, 20.0f * static_cast<float>(n))})
                                        .set(SkinnedMeshRenderer{.model = hero.model})
                                        .set(Animator3D{.clip = "sway"});
            Animator3D& a = *e.get_mut<Animator3D>();
            a.add_ik(hand_constraint(0.2f)).set_target(Vec3f{static_cast<float>(n) + 0.3f, 1.4f, 0.5f}).set_pole_direction({0, 0, -1});
            IKConstraint look;
            look.name = "look";
            look.kind = IKKind::LookAt;
            look.joints = {"neck", "head"};
            look.look_links = {{.weight = 0.4f}, {.weight = 0.6f, .max_yaw_degrees = 70.0f}};
            look.look_at.smooth_seconds = 0.15f;
            look.target = {IKTarget::Kind::World, {static_cast<float>(n) + 2.0f, 1.7f, 1.0f}, 0};
            a.add_ik(look);
        }
        for (int i = 0; i < 40; ++i) {
            const auto m = ik_frame(state, 1.0f / 60.0f);
            h = (h ^ std::hash<std::string_view>{}(std::string_view{reinterpret_cast<const char*>(m.data()), m.size() * sizeof(Mat4f)})) * 1099511628211ull;
        }
        return h;
    };
    assert(run() == run());
    // Joints that do not exist: no effect (and no crash), the plain pose.
    kin::EcsWorld world;
    WorldRenderState3D state{world};
    const flecs::entity e = world.raw().entity().set(Transform3D{}).set(SkinnedMeshRenderer{.model = hero.model}).set(Animator3D{.clip = "sway"});
    IKConstraint bad = hand_constraint();
    bad.joints = {"shoulder_L", "no_such_joint", "hand_L"};
    e.get_mut<Animator3D>()->add_ik(bad).set_target(Vec3f{1, 1, 1});
    const auto with = ik_frame(state, 0.1f);
    std::vector<Mat4f> direct;
    e.get<Animator3D>()->player.joint_matrices(direct);
    assert(max_difference(with, direct) < 1e-6f);
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
    // The pure maths on a 18-joint biped: world matrices, a two-bone arm, a look-at of three joints, a foot placer on two legs.
    Rig rig;
    const std::array<FootLeg, 2> legs = {FootLeg{{biped::hip_l, biped::knee_l, biped::ankle_l}, 0.05f}, FootLeg{{biped::hip_r, biped::knee_r, biped::ankle_r}, 0.05f}};
    const std::array<LookAtJoint, 3> chain = {LookAtJoint{.joint = biped::spine, .weight = 0.2f}, LookAtJoint{.joint = biped::neck, .weight = 0.3f},
                                              LookAtJoint{.joint = biped::head, .weight = 0.5f}};
    const GroundQuery ground = [](Vec3f from, Vec3f) -> std::optional<GroundHit> { return GroundHit{{from.x, -0.1f, from.z}, {0, 1, 0}}; };
    FootPlacementState fps;
    LookAtState las;
    volatile float sink = 0.0f;
    const double maths = time_us([&] {
        Pose pose = rig.pose;
        compute_world_matrices(rig.sk, pose, rig.world);
        TwoBoneOptions arm;
        arm.pole_mode = PoleMode::Direction;
        arm.pole = {0, 0, -1};
        solve_two_bone(rig.sk, pose, rig.world, left_arm, {0.5f, 1.4f, 0.4f}, arm);
        LookAtOptions look;
        look.smooth_seconds = 0.1f;
        solve_look_at(rig.sk, pose, rig.world, chain, {1, 1.7f, 2}, look, &las, 1.0f / 60.0f);
        const FootPlan plan = plan_foot_placement(rig.sk, pose, rig.world, legs, ground, {}, &fps, 1.0f / 60.0f);
        TwoBoneOptions leg;
        leg.pole_mode = PoleMode::Direction;
        leg.pole = {0, 0, 1};
        apply_foot_placement(rig.sk, pose, rig.world, biped::pelvis, legs, plan, leg);
        std::vector<Mat4f> m(rig.sk.joint_count());
        world_to_joint_matrices(rig.sk, rig.world, m);
        sink = sink + m[0].m[0];
    }, 20000);
    // The whole system: 500 animators with a hand, a look-at and a foot placer each.
    kin::EcsWorld world;
    WorldRenderState3D state{world};
    auto& w = world.raw();
    for (int n = 0; n < 500; ++n) {
        const flecs::entity e = w.entity().set(Transform3D{.position = {static_cast<float>(n % 25) * 2.0f, 0, static_cast<float>(n / 25) * 2.0f}})
                                    .set(SkinnedMeshRenderer{.model = hero.model})
                                    .set(Animator3D{.clip = "sway"});
        Animator3D& a = *e.get_mut<Animator3D>();
        a.add_ik(hand_constraint()).set_target(Vec3f{0.3f, 1.4f, 0.5f}).set_pole_direction({0, 0, -1});
        IKConstraint look;
        look.name = "look";
        look.kind = IKKind::LookAt;
        look.joints = {"spine", "neck", "head"};
        look.look_links = {{.weight = 0.2f}, {.weight = 0.3f}, {.weight = 0.5f}};
        look.look_at.smooth_seconds = 0.1f;
        look.target = {IKTarget::Kind::World, {5, 1.7f, 5}, 0};
        a.add_ik(look);
        IKConstraint feet;
        feet.name = "feet";
        feet.kind = IKKind::FootPlacer;
        feet.joints = {"pelvis", "hip_L", "knee_L", "ankle_L", "hip_R", "knee_R", "ankle_R"};
        feet.foot_heights = {0.05f, 0.05f};
        feet.ground = [](Vec3f from, Vec3f) -> std::optional<GroundHit> { return GroundHit{{from.x, -0.05f, from.z}, {0, 1, 0}}; };
        feet.pole = IKConstraint::Pole::ModelDirection;
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
        plain_world.raw().entity().set(Transform3D{.position = {static_cast<float>(n), 0, 0}}).set(SkinnedMeshRenderer{.model = hero.model}).set(Animator3D{.clip = "sway"});
    }
    const auto plain_frame = [&] {
        plain_state.animate(1.0f / 60.0f);
        plain_state.propagate_transforms();
        scene = Scene3D{};
        plain_state.collect(scene);
    };
    plain_frame();
    const double plain_per_frame = time_us(plain_frame, 60);
    std::cout << "ik, 18-joint biped: arm + look-at (3 joints) + two feet + matrices " << maths << " us a character; 500 animators with 3 constraints: animate + collect "
              << per_frame / 1000.0 << " ms a frame (" << per_frame / 500.0 << " us each; the same 500 without constraints " << plain_per_frame / 1000.0 << " ms)" << std::endl;
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
    test_ik_two_bone_reach();
    test_ik_two_bone_out_of_reach();
    test_ik_two_bone_pole();
    test_ik_two_bone_weight();
    test_ik_two_bone_idempotent();
    test_ik_two_bone_continuity();
    test_ik_two_bone_scaled_and_turned();
    test_ik_two_bone_orientation();
    test_ik_look_at();
    test_ik_foot_placement();
    test_ik_fabrik();
    test_ik_order_and_bad_input();
    test_ecs_ik_reaches_world_target();
    test_ecs_ik_entity_target_and_fades();
    test_ecs_ik_order_and_off_path();
    test_ecs_ik_deterministic_and_unresolved();
    bench_ik();
    bench_animation();
    std::cout << "skeleton tests passed\n";
    return 0;
}
