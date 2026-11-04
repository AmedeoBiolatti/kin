#include <kin/anim/skeleton2d.hpp>

#include "skeleton2d_internal.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace kin {
namespace {

using namespace skeleton2d_detail;

bool valid_pose(const SkeletonAsset2D& skeleton, const SkeletonPose2D& pose) {
    return pose.local.size() == skeleton.bones.size() &&
           std::ranges::all_of(pose.local, [](const BoneTransform2D& bone) { return finite(bone); });
}

f32 cross(Vec2f a, Vec2f b) {
    return a.x * b.y - a.y * b.x;
}

Vec2f sub(Vec2f a, Vec2f b) {
    return {a.x - b.x, a.y - b.y};
}

f32 dot(Vec2f a, Vec2f b) {
    return a.x * b.x + a.y * b.y;
}

} // namespace

namespace skeleton2d_detail {

bool blend_poses_local(const SkeletonAsset2D& skeleton, const SkeletonPose2D& from, const SkeletonPose2D& to,
                       f32 weight, SkeletonPose2D& out) {
    if (!std::isfinite(weight) || !valid_pose(skeleton, from) || !valid_pose(skeleton, to)) {
        return false;
    }
    const f32 t = std::clamp(weight, 0.0f, 1.0f);
    std::vector<BoneTransform2D> local(from.local.size());
    for (std::size_t i = 0; i < local.size(); ++i) {
        local[i] = mix(from.local[i], to.local[i], t);
    }
    out.local = std::move(local);
    return true;
}

bool blend_local(const SkeletonAsset2D& skeleton, std::span<const SkeletonBlendSample2D> samples, SkeletonPose2D& out) {
    if (samples.size() > 64) {
        return false;
    }
    f64 total = 0;
    std::vector<std::pair<SkeletonPose2D, f32>> poses;
    for (const SkeletonBlendSample2D& sample : samples) {
        if (!std::isfinite(sample.weight) || sample.weight < 0 || !std::isfinite(sample.time)) {
            return false;
        }
        if (sample.weight == 0) {
            continue;
        }
        SkeletonPose2D pose;
        if (!sample_local(skeleton, sample.clip, sample.time, sample.loop, pose)) {
            return false;
        }
        poses.emplace_back(std::move(pose), sample.weight);
        total += sample.weight;
    }
    if (poses.empty()) {
        rest_local(skeleton, out);
        return true;
    }
    std::vector<BoneTransform2D> local = poses.front().first.local;
    for (std::size_t i = 0; i < skeleton.bones.size(); ++i) {
        // Rotations go round the shorter way from the first pose's.
        const f32 reference = local[i].rotation;
        f64 x = 0, y = 0, sx = 0, sy = 0, rotation = 0;
        for (const auto& [pose, w] : poses) {
            const f64 weight = w / total;
            const BoneTransform2D& v = pose.local[i];
            x += v.position.x * weight;
            y += v.position.y * weight;
            sx += v.scale.x * weight;
            sy += v.scale.y * weight;
            rotation += (reference + std::remainder(v.rotation - reference, 360.0f)) * weight;
        }
        local[i] = {{static_cast<f32>(x), static_cast<f32>(y)},
                    static_cast<f32>(rotation),
                    {static_cast<f32>(sx), static_cast<f32>(sy)}};
    }
    out.local = std::move(local);
    return true;
}

bool layer_local(const SkeletonAsset2D& skeleton, const SkeletonPose2D& layer, f32 weight, std::span<const u8> mask,
                 bool additive, SkeletonPose2D& base) {
    if (!std::isfinite(weight) || !valid_pose(skeleton, base) || !valid_pose(skeleton, layer) ||
        (!mask.empty() && mask.size() != skeleton.bones.size())) {
        return false;
    }
    const f32 t = std::clamp(weight, 0.0f, 1.0f);
    std::vector<BoneTransform2D> local = base.local; // `layer` may be `base`
    for (std::size_t i = 0; i < skeleton.bones.size(); ++i) {
        if (!mask.empty() && !mask[i]) {
            continue;
        }
        BoneTransform2D& b = local[i];
        const BoneTransform2D& v = layer.local[i];
        if (!additive) {
            b = mix(b, v, t);
            continue;
        }
        // The layer's difference from rest, added.
        const BoneTransform2D& rest = skeleton.bones[i].rest;
        b.position.x += (v.position.x - rest.position.x) * t;
        b.position.y += (v.position.y - rest.position.y) * t;
        b.rotation += std::remainder(v.rotation - rest.rotation, 360.0f) * t;
        b.scale.x *= 1 + (v.scale.x / rest.scale.x - 1) * t;
        b.scale.y *= 1 + (v.scale.y / rest.scale.y - 1) * t;
    }
    base.local = std::move(local);
    return true;
}

bool sample_blend_space_local(const SkeletonAsset2D& skeleton, std::string_view name, Vec2f position, f32 phase,
                              bool loop, SkeletonPose2D& out) {
    // The space was validated with its rig (validate_skeleton2d); only what
    // sampling relies on is checked again here.
    const auto found = skeleton.blend_spaces.find(name);
    if (found == skeleton.blend_spaces.end() || !std::isfinite(position.x) || !std::isfinite(position.y) ||
        !std::isfinite(phase) || std::abs(position.x) > 1e6f || std::abs(position.y) > 1e6f) {
        return false;
    }
    const SkeletonBlendSpace2D& space = found->second;
    const std::size_t points = space.points.size();
    if (points == 0 || (points > 2 && space.triangles.empty())) {
        return false;
    }
    std::vector<f32> weights(points, 0);
    f32 best = std::numeric_limits<f32>::infinity();
    if (points == 1) {
        weights[0] = 1;
    }
    // The nearest point on the edge x-y, when nearer than any so far.
    auto edge = [&](u32 x, u32 y) {
        const Vec2f p = space.points[x].position;
        const Vec2f v = sub(space.points[y].position, p);
        const Vec2f delta = sub(position, p);
        const f32 t = std::clamp(dot(delta, v) / dot(v, v), 0.0f, 1.0f);
        const Vec2f d = sub(position, {p.x + v.x * t, p.y + v.y * t});
        const f32 distance = dot(d, d);
        if (distance < best) {
            best = distance;
            std::fill(weights.begin(), weights.end(), 0);
            weights[x] = 1 - t;
            weights[y] = t;
        }
    };
    if (points == 2) {
        edge(0, 1);
    } else {
        for (const auto& triangle : space.triangles) {
            if (std::ranges::any_of(triangle, [&](u32 i) { return i >= points; })) {
                return false;
            }
            const Vec2f p = space.points[triangle[0]].position;
            const Vec2f v = sub(space.points[triangle[1]].position, p);
            const Vec2f w = sub(space.points[triangle[2]].position, p);
            const Vec2f delta = sub(position, p);
            const f32 det = cross(v, w);
            if (det == 0) {
                continue;
            }
            const f32 y = cross(delta, w) / det;
            const f32 z = cross(v, delta) / det;
            const f32 x = 1 - y - z;
            if (x >= -1e-6f && y >= -1e-6f && z >= -1e-6f) {
                std::fill(weights.begin(), weights.end(), 0);
                weights[triangle[0]] = std::max(0.0f, x);
                weights[triangle[1]] = std::max(0.0f, y);
                weights[triangle[2]] = std::max(0.0f, z);
                break;
            }
            edge(triangle[0], triangle[1]);
            edge(triangle[1], triangle[2]);
            edge(triangle[2], triangle[0]);
        }
    }
    std::vector<SkeletonBlendSample2D> samples;
    for (std::size_t i = 0; i < points; ++i) {
        if (weights[i] <= 0) {
            continue;
        }
        const std::string& clip = space.points[i].clip;
        const auto sampled = skeleton.clips.find(clip);
        if (sampled == skeleton.clips.end()) {
            return false;
        }
        samples.push_back({clip, phase * sampled->second.duration, weights[i], loop});
    }
    return blend_local(skeleton, samples, out);
}

} // namespace skeleton2d_detail

bool blend_skeleton_poses2d(const SkeletonAsset2D& skeleton, const SkeletonPose2D& from, const SkeletonPose2D& to,
                            f32 weight, SkeletonPose2D& out) {
    if (!blend_poses_local(skeleton, from, to, weight, out)) {
        return false;
    }
    out.evaluate(skeleton);
    return true;
}

bool blend_skeleton2d(const SkeletonAsset2D& skeleton, std::span<const SkeletonBlendSample2D> samples,
                      SkeletonPose2D& out) {
    if (!blend_local(skeleton, samples, out)) {
        return false;
    }
    out.evaluate(skeleton);
    return true;
}

bool layer_skeleton2d(const SkeletonAsset2D& skeleton, const SkeletonPose2D& layer, f32 weight,
                      std::span<const u8> mask, bool additive, SkeletonPose2D& base) {
    if (!layer_local(skeleton, layer, weight, mask, additive, base)) {
        return false;
    }
    base.evaluate(skeleton);
    return true;
}

bool validate_skeleton_blend_space2d(const SkeletonAsset2D& skeleton, const SkeletonBlendSpace2D& space,
                                     std::string& error) {
    auto fail = [&](const char* why) {
        error = why;
        return false;
    };
    if (space.points.empty() || space.points.size() > 64 || space.triangles.size() > 128) {
        return fail("invalid blend-space size");
    }
    for (std::size_t i = 0; i < space.points.size(); ++i) {
        const SkeletonBlendPoint2D& p = space.points[i];
        if (!std::isfinite(p.position.x) || !std::isfinite(p.position.y) || std::abs(p.position.x) > 1e6f ||
            std::abs(p.position.y) > 1e6f || !skeleton.clips.contains(p.clip)) {
            return fail("invalid blend-space point/clip");
        }
        for (std::size_t j = 0; j < i; ++j) {
            const Vec2f d = sub(p.position, space.points[j].position);
            if (dot(d, d) < 1e-12f) {
                return fail("duplicate blend-space point");
            }
        }
    }
    if (space.points.size() > 2 && space.triangles.empty()) {
        return fail("blend-space mesh requires triangles");
    }
    for (const auto& t : space.triangles) {
        for (const u32 i : t) {
            if (i >= space.points.size()) {
                return fail("blend-space triangle index outside points");
            }
        }
        if (std::abs(cross(sub(space.points[t[1]].position, space.points[t[0]].position),
                           sub(space.points[t[2]].position, space.points[t[0]].position))) < 1e-8f) {
            return fail("degenerate blend-space triangle");
        }
    }
    error.clear();
    return true;
}

bool sample_skeleton_blend_space2d(const SkeletonAsset2D& skeleton, std::string_view space, Vec2f position, f32 phase,
                                   bool loop, SkeletonPose2D& pose) {
    if (!sample_blend_space_local(skeleton, space, position, phase, loop, pose)) {
        return false;
    }
    pose.evaluate(skeleton);
    return true;
}

} // namespace kin
