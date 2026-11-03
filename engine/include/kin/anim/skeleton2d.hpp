#pragma once
#include <array>
#include <kin/core/affine.hpp>
#include <kin/renderer/shader.hpp>
#include <map>
#include <optional>
#include <string>

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
    Vec2f position{}, uv{}; // rest position in skeleton space, normalized atlas UV
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
};
bool validate_skeleton2d(const SkeletonAsset2D& asset, std::string& error);
// Native JSON .kinskel format; no external skeleton runtime is required.
std::optional<SkeletonAsset2D> parse_skeleton2d(std::string_view json, std::string& error);

struct SkeletonPose2D {
    std::vector<BoneTransform2D> local;
    std::vector<Affine2> world, skin;
    void reset(const SkeletonAsset2D& skeleton);
    void evaluate(const SkeletonAsset2D& skeleton);
};
// Absolute local keys, linear position/scale and shortest-angle rotation.
// Unkeyed bones retain their rest transform. False for an unknown clip.
bool sample_skeleton2d(const SkeletonAsset2D&, std::string_view clip, f32 time, bool loop, SkeletonPose2D&);
struct SkeletonBlendSample2D {
    std::string clip;
    f32 time = 0, weight = 1;
    bool loop = true;
};
// Normalized positive weights; rotations use the shortest arc from the first
// contributing pose. Failed samples leave the output unchanged.
bool blend_skeleton2d(const SkeletonAsset2D&, std::span<const SkeletonBlendSample2D>, SkeletonPose2D&);
bool blend_skeleton_poses2d(const SkeletonAsset2D&, const SkeletonPose2D&, const SkeletonPose2D&, f32 weight,
                            SkeletonPose2D&);
// Empty mask affects every bone. Additive layers apply the difference from rest.
bool layer_skeleton2d(const SkeletonAsset2D&, const SkeletonPose2D& layer, f32 weight, std::span<const u8> mask,
                      bool additive, SkeletonPose2D& base);
bool validate_skeleton_blend_space2d(const SkeletonAsset2D&, const SkeletonBlendSpace2D&, std::string& error);
// Barycentric interpolation, clamped to the closest edge outside the mesh.
// A common normalized phase keeps clips of different durations synchronized.
bool sample_skeleton_blend_space2d(const SkeletonAsset2D&, std::string_view space, Vec2f position, f32 phase, bool loop,
                                   SkeletonPose2D&);
std::vector<ShaderVertex> deform_skeleton2d(const SkeletonAsset2D&, const SkeletonPose2D&, Color tint = colors::white);
std::optional<Affine2> skeleton_attachment2d(const SkeletonAsset2D&, const SkeletonPose2D&, std::string_view name);
// A direct parent/child chain with positive uniform scales. Target is in
// skeleton space; bend is +1 or -1. Clamps unreachable targets, preserves bone
// lengths, and blends the solution with weight in [0,1].
bool solve_two_bone_ik2d(const SkeletonAsset2D&, SkeletonPose2D&, u32 root, u32 tip, Vec2f target, f32 bend = 1,
                         f32 weight = 1);
} // namespace kin
