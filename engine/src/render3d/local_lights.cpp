#include <algorithm>
#include <cmath>
#include <kin/render3d/local_lights3d.hpp>

namespace kin {

namespace {

constexpr f32 degrees_to_radians = 3.14159265358979f / 180.0f;

Vec3f unit_or(Vec3f v, Vec3f fallback) {
    return length_squared(v) > 1e-12f ? normalize(v) : fallback;
}

// An up vector that is not along `axis`.
Vec3f up_for(Vec3f axis) {
    return std::abs(axis.y) > 0.99f ? Vec3f{0.0f, 0.0f, -1.0f} : Vec3f{0.0f, 1.0f, 0.0f};
}

} // namespace

SpotCone3D spot_cone(const SpotLightSource3D& light) {
    SpotCone3D cone;
    cone.axis = unit_or(light.direction, {0.0f, -1.0f, 0.0f});
    const f32 outer = std::clamp(light.outer_angle, 0.1f, 89.0f);
    const f32 inner = std::clamp(light.inner_angle, 0.0f, outer);
    cone.cos_outer = std::cos(outer * degrees_to_radians);
    cone.cos_inner = std::cos(inner * degrees_to_radians);
    cone.scale = 1.0f / std::max(cone.cos_inner - cone.cos_outer, 1e-3f);
    return cone;
}

f32 spot_cone_factor(const SpotCone3D& cone, f32 cos_angle) {
    const f32 t = std::clamp((cos_angle - cone.cos_outer) * cone.scale, 0.0f, 1.0f);
    return t * t;
}

f32 point_attenuation(Vec3f position, f32 radius, f32 size, Vec3f point) {
    const Vec3f to = position - point;
    const f32 d2 = dot(to, to);
    const f32 r2 = d2 / (radius * radius);
    const f32 window = std::clamp(1.0f - r2 * r2, 0.0f, 1.0f);
    const f32 s = std::max(size, 0.01f);
    return window * window / std::max(d2, s * s);
}

f32 spot_attenuation(const SpotLightSource3D& light, Vec3f point) {
    const SpotCone3D cone = spot_cone(light);
    const Vec3f from_light = point - light.position;
    const f32 d = length(from_light);
    const f32 cos_angle = d > 1e-8f ? dot(from_light, cone.axis) / d : 1.0f;
    return point_attenuation(light.position, light.radius, light.size, point) * spot_cone_factor(cone, cos_angle);
}

void spot_bounding_sphere(const SpotLightSource3D& light, Vec3f& centre, f32& radius) {
    const SpotCone3D cone = spot_cone(light);
    const f32 reach = std::max(light.radius, 0.0f);
    if (cone.cos_outer <= 0.7071068f) {
        // Wider than a right angle's half: the ball round the apex is the smaller.
        centre = light.position;
        radius = reach;
        return;
    }
    // The sphere through the apex and the rim of the cone's cap: centre reach / (2 cos) along the axis, that far in
    // radius.
    const f32 r = reach / (2.0f * cone.cos_outer);
    centre = light.position + cone.axis * r;
    radius = r;
}

f32 local_shadow_far(f32 shadow_distance, f32 radius) {
    return shadow_distance > 0.0f ? std::min(shadow_distance, radius) : radius;
}

LocalShadowView3D spot_shadow_view(const SpotLightSource3D& light, u32 resolution) {
    LocalShadowView3D v;
    const SpotCone3D cone = spot_cone(light);
    const f32 res = static_cast<f32>(std::max(resolution, 16u));
    const f32 outer = std::acos(cone.cos_outer);
    v.tan_half = std::tan(outer) * (res + 2.0f * shadow_guard_texels) / res;
    v.near_plane = std::max(light.shadow_near_plane, 1e-3f);
    v.far_plane = std::max(local_shadow_far(light.shadow_distance, light.radius), v.near_plane * 2.0f);
    const f32 fov = 2.0f * std::atan(v.tan_half) / degrees_to_radians;
    const Mat4f view = Mat4f::look_at(light.position, light.position + cone.axis, up_for(cone.axis));
    v.view_projection = Mat4f::perspective(fov, 1.0f, v.near_plane, v.far_plane) * view;
    return v;
}

u32 cube_face_of(Vec3f d) {
    const f32 ax = std::abs(d.x), ay = std::abs(d.y), az = std::abs(d.z);
    if (ax >= ay && ax >= az) {
        return d.x < 0.0f ? 1u : 0u;
    }
    if (ay >= az) {
        return d.y < 0.0f ? 3u : 2u;
    }
    return d.z < 0.0f ? 5u : 4u;
}

LocalShadowView3D cube_face_view(const PointLightSource3D& light, u32 face, u32 resolution) {
    static const Vec3f axes[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    LocalShadowView3D v;
    const f32 res = static_cast<f32>(std::max(resolution, 16u));
    v.tan_half = (res + 2.0f * shadow_guard_texels) / res; // a right angle's half (tan 45 = 1) and the guard band
    v.near_plane = std::max(light.shadow_near_plane, 1e-3f);
    v.far_plane = std::max(local_shadow_far(light.shadow_distance, light.radius), v.near_plane * 2.0f);
    const Vec3f axis = axes[std::min(face, 5u)];
    const f32 fov = 2.0f * std::atan(v.tan_half) / degrees_to_radians;
    const Mat4f view = Mat4f::look_at(light.position, light.position + axis, up_for(axis));
    v.view_projection = Mat4f::perspective(fov, 1.0f, v.near_plane, v.far_plane) * view;
    return v;
}

f32 shadow_importance(f32 intensity, f32 reach, Vec3f centre, f32 sphere_radius, Vec3f eye) {
    const f32 r = std::max(sphere_radius, 1e-3f);
    const f32 d = length(centre - eye);
    // The sphere's angular radius, over a right angle: 1 from inside it, small far away; squared (an area).
    const f32 coverage = d <= r ? 1.0f : std::min(1.0f, std::asin(std::min(1.0f, r / d)) / 1.5707963f);
    return std::max(intensity, 0.0f) * std::max(reach, 0.0f) * coverage * coverage;
}

void assign_shadow_slots(ShadowSlots3D& slots, std::span<const ShadowRequest3D> requests, f32 hysteresis,
                         std::vector<i32>& slot_of) {
    const std::size_t count = slots.held.size();
    slot_of.assign(requests.size(), -1);
    // Importance, times the hysteresis for a light that holds a slot already; the first request wins a tie.
    std::vector<std::pair<f32, std::size_t>> order;
    order.reserve(requests.size());
    for (std::size_t i = 0; i < requests.size(); ++i) {
        if (requests[i].key == 0 || !(requests[i].importance > 0.0f)) {
            continue;
        }
        const bool holds = std::find(slots.held.begin(), slots.held.end(), requests[i].key) != slots.held.end();
        order.push_back({requests[i].importance * (holds ? hysteresis : 1.0f), i});
    }
    std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    if (order.size() > count) {
        order.resize(count);
    }
    // Those that held a slot keep it; the slots of the others are freed.
    std::vector<u64> next(count, 0);
    std::vector<std::size_t> unplaced;
    for (const auto& [score, i] : order) {
        const auto held = std::find(slots.held.begin(), slots.held.end(), requests[i].key);
        if (held != slots.held.end()) {
            const std::size_t slot = static_cast<std::size_t>(held - slots.held.begin());
            next[slot] = requests[i].key;
            slot_of[i] = static_cast<i32>(slot);
        } else {
            unplaced.push_back(i);
        }
    }
    for (const std::size_t i : unplaced) {
        for (std::size_t slot = 0; slot < count; ++slot) {
            if (next[slot] == 0) {
                next[slot] = requests[i].key;
                slot_of[i] = static_cast<i32>(slot);
                break;
            }
        }
    }
    slots.held = std::move(next);
}

} // namespace kin
