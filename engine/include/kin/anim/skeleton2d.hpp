#pragma once

// 2D skeletons: rigs of bones skinning one textured mesh, their clips, poses
// sampled and blended from them, and two-bone IK (docs/skeleton_2d.md). The
// native .kinskel format needs no external runtime; kin/ecs/skeleton2d.hpp
// puts rigs on entities.
//
//     std::optional<kin::SkeletonAsset2D> rig = kin::parse_skeleton2d(text, error);
//     kin::SkeletonPose2D pose;
//     pose.reset(*rig);
//     kin::sample_skeleton2d(*rig, "walk", time, true, pose);
//     const std::vector<kin::ShaderVertex> mesh = kin::deform_skeleton2d(*rig, pose);
//
// Angles are clockwise degrees; poses are in skeleton space.

#include <kin/core/affine.hpp>
#include <kin/renderer/shader.hpp>

#include <array>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kin {

struct BoneTransform2D {
    Vec2f position{};
    f32 rotation = 0;
    Vec2f scale{1, 1};
};

struct Bone2D {
    std::string name;
    i32 parent = -1; // parent precedes child
    BoneTransform2D rest{};
    f32 length = 0;
};

struct BoneKey2D {
    f32 time = 0;
    BoneTransform2D transform;
};

struct BoneTrack2D {
    u32 bone = 0;
    std::vector<BoneKey2D> keys;
};

struct SkeletonClip2D {
    f32 duration = 1;
    std::vector<BoneTrack2D> tracks;
};

struct SkinVertex2D {
    Vec2f position{}; // at rest, in skeleton space
    Vec2f uv{};       // normalized, in the atlas
    std::array<u32, 4> bones{};
    std::array<f32, 4> weights{1, 0, 0, 0};
};

struct BoneAttachment2D {
    std::string name;
    u32 bone = 0;
    BoneTransform2D transform;
};

struct SkeletonBlendPoint2D {
    Vec2f position{};
    std::string clip;
};

struct SkeletonBlendSpace2D {
    std::vector<SkeletonBlendPoint2D> points;
    std::vector<std::array<u32, 3>> triangles; // required for three or more points
};

struct SkeletonAsset2D {
    std::string texture_asset; // content-relative atlas
    std::vector<Bone2D> bones;
    std::vector<SkinVertex2D> vertices;
    std::vector<u32> indices;
    std::vector<BoneAttachment2D> attachments;
    std::map<std::string, SkeletonClip2D, std::less<>> clips;
    std::map<std::string, SkeletonBlendSpace2D, std::less<>> blend_spaces;
    // Each bone's rest transform in skeleton space, inverted: made by
    // prepare_skeleton2d (parse_skeleton2d prepares what it reads). Poses of
    // a rig without them work them out each time they are evaluated.
    std::vector<Affine2> inverse_bind;
};

bool validate_skeleton2d(const SkeletonAsset2D& asset, std::string& error);
// Validates a rig built in code and makes its inverse_bind, as loading does:
// do it once, before posing it (and again after changing its bones).
bool prepare_skeleton2d(SkeletonAsset2D& asset, std::string& error);
// Reads (and prepares) a .kinskel file's JSON.
std::optional<SkeletonAsset2D> parse_skeleton2d(std::string_view json, std::string& error);

struct SkeletonPose2D {
    std::vector<BoneTransform2D> local;
    std::vector<Affine2> world;
    std::vector<Affine2> skin;
    // At rest. Throws for a rig whose bones aren't parents before children.
    void reset(const SkeletonAsset2D& skeleton);
    // `world` and `skin` from `local`.
    void evaluate(const SkeletonAsset2D& skeleton);
};

// Absolute local keys, linear position/scale and shortest-angle rotation.
// Unkeyed bones retain their rest transform. False for an unknown clip.
bool sample_skeleton2d(const SkeletonAsset2D& skeleton, std::string_view clip, f32 time, bool loop,
                       SkeletonPose2D& pose);

struct SkeletonBlendSample2D {
    std::string clip;
    f32 time = 0;
    f32 weight = 1;
    bool loop = true;
};

// Normalized positive weights; rotations use the shortest arc from the first
// contributing pose. Failed samples leave the output unchanged.
bool blend_skeleton2d(const SkeletonAsset2D& skeleton, std::span<const SkeletonBlendSample2D> samples,
                      SkeletonPose2D& out);
bool blend_skeleton_poses2d(const SkeletonAsset2D& skeleton, const SkeletonPose2D& from, const SkeletonPose2D& to,
                            f32 weight, SkeletonPose2D& out);
// Empty mask affects every bone. Additive layers apply the difference from rest.
bool layer_skeleton2d(const SkeletonAsset2D& skeleton, const SkeletonPose2D& layer, f32 weight,
                      std::span<const u8> mask, bool additive, SkeletonPose2D& base);
bool validate_skeleton_blend_space2d(const SkeletonAsset2D& skeleton, const SkeletonBlendSpace2D& space,
                                     std::string& error);
// Barycentric interpolation, clamped to the closest edge outside the mesh.
// A common normalized phase keeps clips of different durations synchronized.
bool sample_skeleton_blend_space2d(const SkeletonAsset2D& skeleton, std::string_view space, Vec2f position, f32 phase,
                                   bool loop, SkeletonPose2D& pose);

std::vector<ShaderVertex> deform_skeleton2d(const SkeletonAsset2D& skeleton, const SkeletonPose2D& pose,
                                            Color tint = colors::white);
std::optional<Affine2> skeleton_attachment2d(const SkeletonAsset2D& skeleton, const SkeletonPose2D& pose,
                                             std::string_view name);
// A direct parent/child chain with positive uniform scales. Target is in
// skeleton space; bend is +1 or -1. Clamps unreachable targets, preserves bone
// lengths, and blends the solution with weight in [0,1].
bool solve_two_bone_ik2d(const SkeletonAsset2D& skeleton, SkeletonPose2D& pose, u32 root, u32 tip, Vec2f target,
                         f32 bend = 1, f32 weight = 1);

} // namespace kin
