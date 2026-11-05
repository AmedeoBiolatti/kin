#pragma once

// What 2D skeleton sampling, blending and the ECS animators share
// (kin/anim/skeleton2d.hpp). The *_local functions set a pose's local
// transforms only: whoever calls them evaluates the pose once, when done.

#include <kin/anim/skeleton2d.hpp>

#include <span>
#include <string_view>

namespace kin::skeleton2d_detail {

bool finite(Vec2f v);
bool finite(const BoneTransform2D& t);

// `a` turned toward `b` by `t` (0..1), the shorter way round, in degrees.
f32 mix_angle(f32 a, f32 b, f32 t);
// From `a` to `b` by `t`: position and scale in a line, rotation the shorter way round.
BoneTransform2D mix(const BoneTransform2D& a, const BoneTransform2D& b, f32 t);

// Time into a clip `duration` long: wrapped round when looping, else held within it.
f32 wrap_clip_time(f32 time, f32 duration, bool loop);

// The skeleton's rest pose.
void rest_local(const SkeletonAsset2D& skeleton, SkeletonPose2D& pose);
bool sample_local(const SkeletonAsset2D& skeleton, std::string_view clip, f32 time, bool loop, SkeletonPose2D& pose);
bool blend_local(const SkeletonAsset2D& skeleton, std::span<const SkeletonBlendSample2D> samples, SkeletonPose2D& out);
bool blend_poses_local(const SkeletonAsset2D& skeleton, const SkeletonPose2D& from, const SkeletonPose2D& to,
                       f32 weight, SkeletonPose2D& out);
bool layer_local(const SkeletonAsset2D& skeleton, const SkeletonPose2D& layer, f32 weight, std::span<const u8> mask,
                 bool additive, SkeletonPose2D& base);
bool sample_blend_space_local(const SkeletonAsset2D& skeleton, std::string_view space, Vec2f position, f32 phase,
                              bool loop, SkeletonPose2D& out);

} // namespace kin::skeleton2d_detail
