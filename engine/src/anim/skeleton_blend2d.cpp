#include <algorithm>
#include <cmath>
#include <kin/anim/skeleton2d.hpp>
#include <limits>

namespace kin {
namespace {
bool valid_pose(const SkeletonAsset2D& a, const SkeletonPose2D& p) {
    return p.local.size() == a.bones.size() && std::ranges::all_of(p.local, [](const auto& b) {
               return std::isfinite(b.position.x) && std::isfinite(b.position.y) && std::isfinite(b.rotation) &&
                      std::isfinite(b.scale.x) && std::isfinite(b.scale.y);
           });
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
bool blend_skeleton_poses2d(const SkeletonAsset2D& a, const SkeletonPose2D& from, const SkeletonPose2D& to, f32 weight,
                            SkeletonPose2D& out) {
    if (!std::isfinite(weight) || !valid_pose(a, from) || !valid_pose(a, to))
        return false;
    const f32 t = std::clamp(weight, 0.0f, 1.0f);
    SkeletonPose2D result;
    result.local = from.local;
    for (std::size_t i = 0; i < result.local.size(); ++i) {
        auto& b = result.local[i];
        const auto& v = to.local[i];
        b.position = {b.position.x + (v.position.x - b.position.x) * t,
                      b.position.y + (v.position.y - b.position.y) * t};
        b.scale = {b.scale.x + (v.scale.x - b.scale.x) * t, b.scale.y + (v.scale.y - b.scale.y) * t};
        b.rotation += std::remainder(v.rotation - b.rotation, 360.0f) * t;
    }
    result.evaluate(a);
    out = std::move(result);
    return true;
}
bool blend_skeleton2d(const SkeletonAsset2D& a, std::span<const SkeletonBlendSample2D> samples, SkeletonPose2D& out) {
    if (samples.size() > 64)
        return false;
    f64 total = 0;
    std::vector<std::pair<SkeletonPose2D, f32>> poses;
    for (const auto& sample : samples) {
        if (!std::isfinite(sample.weight) || sample.weight < 0 || !std::isfinite(sample.time))
            return false;
        if (sample.weight == 0)
            continue;
        SkeletonPose2D pose;
        if (!sample_skeleton2d(a, sample.clip, sample.time, sample.loop, pose))
            return false;
        poses.emplace_back(std::move(pose), sample.weight);
        total += sample.weight;
    }
    SkeletonPose2D result;
    if (poses.empty()) {
        result.reset(a);
        out = std::move(result);
        return true;
    }
    result.local = poses.front().first.local;
    for (std::size_t i = 0; i < a.bones.size(); ++i) {
        auto& b = result.local[i];
        const auto reference = b.rotation;
        f64 x = 0, y = 0, sx = 0, sy = 0, rotation = 0;
        for (const auto& [pose, w] : poses) {
            const f64 weight = w / total;
            const auto& v = pose.local[i];
            x += v.position.x * weight;
            y += v.position.y * weight;
            sx += v.scale.x * weight;
            sy += v.scale.y * weight;
            rotation += (reference + std::remainder(v.rotation - reference, 360.0f)) * weight;
        }
        b = {{static_cast<f32>(x), static_cast<f32>(y)},
             static_cast<f32>(rotation),
             {static_cast<f32>(sx), static_cast<f32>(sy)}};
    }
    result.evaluate(a);
    out = std::move(result);
    return true;
}
bool layer_skeleton2d(const SkeletonAsset2D& a, const SkeletonPose2D& layer, f32 weight, std::span<const u8> mask,
                      bool additive, SkeletonPose2D& base) {
    if (!std::isfinite(weight) || !valid_pose(a, base) || !valid_pose(a, layer) ||
        (!mask.empty() && mask.size() != a.bones.size()))
        return false;
    const auto t = std::clamp(weight, 0.0f, 1.0f);
    SkeletonPose2D result = base;
    for (std::size_t i = 0; i < a.bones.size(); ++i) {
        if (!mask.empty() && !mask[i])
            continue;
        auto& b = result.local[i];
        const auto& v = layer.local[i];
        const auto& rest = a.bones[i].rest;
        if (additive) {
            b.position.x += (v.position.x - rest.position.x) * t;
            b.position.y += (v.position.y - rest.position.y) * t;
            b.rotation += std::remainder(v.rotation - rest.rotation, 360.0f) * t;
            b.scale.x *= 1 + (v.scale.x / rest.scale.x - 1) * t;
            b.scale.y *= 1 + (v.scale.y / rest.scale.y - 1) * t;
        } else {
            b.position.x += (v.position.x - b.position.x) * t;
            b.position.y += (v.position.y - b.position.y) * t;
            b.rotation += std::remainder(v.rotation - b.rotation, 360.0f) * t;
            b.scale.x += (v.scale.x - b.scale.x) * t;
            b.scale.y += (v.scale.y - b.scale.y) * t;
        }
    }
    result.evaluate(a);
    base = std::move(result);
    return true;
}
bool validate_skeleton_blend_space2d(const SkeletonAsset2D& a, const SkeletonBlendSpace2D& space, std::string& error) {
    auto fail = [&](const char* why) {
        error = why;
        return false;
    };
    if (space.points.empty() || space.points.size() > 64 || space.triangles.size() > 128)
        return fail("invalid blend-space size");
    for (std::size_t i = 0; i < space.points.size(); ++i) {
        const auto& p = space.points[i];
        if (!std::isfinite(p.position.x) || !std::isfinite(p.position.y) || std::abs(p.position.x) > 1e6f ||
            std::abs(p.position.y) > 1e6f || !a.clips.contains(p.clip))
            return fail("invalid blend-space point/clip");
        for (std::size_t j = 0; j < i; ++j)
            if (dot(sub(p.position, space.points[j].position), sub(p.position, space.points[j].position)) < 1e-12f)
                return fail("duplicate blend-space point");
    }
    if (space.points.size() > 2 && space.triangles.empty())
        return fail("blend-space mesh requires triangles");
    for (const auto& t : space.triangles) {
        for (auto i : t)
            if (i >= space.points.size())
                return fail("blend-space triangle index outside points");
        if (std::abs(cross(sub(space.points[t[1]].position, space.points[t[0]].position),
                           sub(space.points[t[2]].position, space.points[t[0]].position))) < 1e-8f)
            return fail("degenerate blend-space triangle");
    }
    error.clear();
    return true;
}
bool sample_skeleton_blend_space2d(const SkeletonAsset2D& a, std::string_view name, Vec2f position, f32 phase,
                                   bool loop, SkeletonPose2D& out) {
    const auto found = a.blend_spaces.find(name);
    std::string error;
    if (found == a.blend_spaces.end() || !std::isfinite(position.x) || !std::isfinite(position.y) ||
        !std::isfinite(phase) || std::abs(position.x) > 1e6f || std::abs(position.y) > 1e6f ||
        !validate_skeleton_blend_space2d(a, found->second, error))
        return false;
    const auto& space = found->second;
    std::vector<f32> weights(space.points.size(), 0);
    f32 best = std::numeric_limits<f32>::infinity();
    if (space.points.size() == 1)
        weights[0] = 1;
    auto edge = [&](u32 x, u32 y) {
        const auto p = space.points[x].position, v = sub(space.points[y].position, p), delta = sub(position, p);
        const auto t = std::clamp(dot(delta, v) / dot(v, v), 0.0f, 1.0f);
        const auto d = sub(position, {p.x + v.x * t, p.y + v.y * t});
        const auto distance = dot(d, d);
        if (distance < best) {
            best = distance;
            std::fill(weights.begin(), weights.end(), 0);
            weights[x] = 1 - t;
            weights[y] = t;
        }
    };
    if (space.points.size() == 2)
        edge(0, 1);
    else
        for (const auto& triangle : space.triangles) {
            const auto p = space.points[triangle[0]].position, v = sub(space.points[triangle[1]].position, p),
                       w = sub(space.points[triangle[2]].position, p), delta = sub(position, p);
            const auto det = cross(v, w);
            const auto y = cross(delta, w) / det, z = cross(v, delta) / det, x = 1 - y - z;
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
    std::vector<SkeletonBlendSample2D> samples;
    for (std::size_t i = 0; i < weights.size(); ++i)
        if (weights[i] > 0) {
            const auto& clip = space.points[i].clip;
            samples.push_back({clip, phase * a.clips.at(clip).duration, weights[i], loop});
        }
    return blend_skeleton2d(a, samples, out);
}
} // namespace kin
