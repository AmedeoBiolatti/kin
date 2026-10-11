// Skeletons, clips, poses and the glTF skin reader (kin/render3d/skeleton.hpp, animation3d.hpp, model.hpp), without a GPU:
// keyframe sampling against hand-computed glTF values, blends, additive clips, masks, joint matrices of a known chain, root
// motion, the player's crossfade, bounds that no posed vertex leaves, and the loader on glTF files made in code.

#include "skin_fixtures.hpp"

#include <kin/ecs/render3d.hpp>
#include <kin/ecs/world.hpp>
#include <kin/render3d/animation3d.hpp>
#include <kin/render3d/model.hpp>
#include <kin/render3d/skeleton.hpp>

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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
    bench_animation();
    std::cout << "skeleton tests passed\n";
    return 0;
}
