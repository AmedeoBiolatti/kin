#include "gpu_portal_sun3d.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <kin/render3d/shadow_cascades3d.hpp>
#include <limits>

namespace kin::gpu {
namespace {
bool finite(Vec3f v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
bool rotation(Quatf q) {
    const f32 n = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    return std::isfinite(n) && n > 1e-8f;
}
void row(f32* out, Vec3f axis, f32 w) {
    out[0] = axis.x;
    out[1] = axis.y;
    out[2] = axis.z;
    out[3] = w;
}
} // namespace

PortalSunPlan portal_sun_plan(const Scene3D& scene, u32 resolution) {
    PortalSunPlan plan;
    const auto& lights = scene.lights;
    if (!finite(lights.sun.direction) || length_squared(lights.sun.direction) < 1e-12f ||
        !(lights.sun.color.r + lights.sun.color.g + lights.sun.color.b > 0))
        return plan;
    const Vec3f sun = normalize(lights.sun.direction);
    const Bounds3D area = lights.shadow_area;
    const Vec3f size = area.size();
    const bool maps = finite(area.min) && finite(area.max) && size.x > 0 && size.y >= 0 && size.z > 0;
    const f32 side = f32(std::clamp(resolution, 64u, 8192u));
    for (const PortalLink3D& link : scene.portals) {
        if (plan.count == max_portal_sun_maps)
            break;
        const f32 width = std::min(link.entry.size.x, link.exit.size.x);
        const f32 height = std::min(link.entry.size.y, link.exit.size.y);
        if (!(width > 0 && height > 0 && std::isfinite(width + height)) || !finite(link.entry.position) ||
            !finite(link.exit.position) || !rotation(link.entry.rotation) || !rotation(link.exit.rotation))
            continue;
        const Quatf receiver = link.entry.rotation.normalized(), source = link.exit.rotation.normalized();
        const Vec3f source_normal = source.rotate({0, 0, 1});
        const bool enters_sun = dot(source_normal, sun) > 1e-4f;
        const PortalPose3D entry{link.entry.position, receiver, link.entry.size};
        const PortalPose3D exit{link.exit.position, source, link.exit.size};
        const Quatf turn = portal_rotation(exit, entry);
        const Mat4f through =
            Mat4f::translation(entry.position) * Mat4f::rotation(turn) * Mat4f::translation(-exit.position);
        const Vec3f toward = normalize(turn.rotate(sun));
        const Vec3f normal = receiver.rotate({0, 0, 1}), x = receiver.rotate({1, 0, 0}), y = receiver.rotate({0, 1, 0});
        auto& light = plan.lights[plan.count];
        row(light.plane, normal, -dot(normal, entry.position));
        row(light.x, x, -dot(x, entry.position));
        row(light.y, y, -dot(y, entry.position));
        row(light.direction, toward, width * 0.5f);
        light.aperture[0] = height * 0.5f;
        light.aperture[3] = enters_sun ? -1.0f : -2.0f;
        plan.source_clip[plan.count] = plane_through(exit.position, source_normal);
        if (maps && enters_sun) {
            Bounds3D combined = area;
            const auto include = [&](Vec3f p) {
                combined.min = {std::min(combined.min.x, p.x), std::min(combined.min.y, p.y),
                                std::min(combined.min.z, p.z)};
                combined.max = {std::max(combined.max.x, p.x), std::max(combined.max.y, p.y),
                                std::max(combined.max.z, p.z)};
            };
            for (int i = 0; i < 8; ++i)
                include(through.apply({i & 1 ? area.max.x : area.min.x, i & 2 ? area.max.y : area.min.y,
                                       i & 4 ? area.max.z : area.min.z}));
            std::array<Vec3f, 4> corners;
            for (int i = 0; i < 4; ++i) {
                corners[i] =
                    entry.position + x * ((i & 1 ? 1 : -1) * width * 0.5f) + y * ((i & 2 ? 1 : -1) * height * 0.5f);
                include(corners[i]);
            }
            const SunBox3D box = fit_sun_box(combined, toward);
            Vec2f lo{std::numeric_limits<f32>::max(), std::numeric_limits<f32>::max()}, hi{-lo.x, -lo.y};
            for (Vec3f corner : corners) {
                const Vec3f p = box.light_view.apply(corner);
                lo = {std::min(lo.x, p.x), std::min(lo.y, p.y)};
                hi = {std::max(hi.x, p.x), std::max(hi.y, p.y)};
            }
            const Vec2f margin{std::max(1e-4f, (hi.x - lo.x) * 2 / (side - 4)),
                               std::max(1e-4f, (hi.y - lo.y) * 2 / (side - 4))};
            lo = {lo.x - margin.x, lo.y - margin.y};
            hi = {hi.x + margin.x, hi.y + margin.y};
            const Mat4f vp = Mat4f::orthographic(lo.x, hi.x, lo.y, hi.y, box.z_near, box.z_far) * box.light_view;
            std::memcpy(light.shadow_matrix, vp.m, sizeof(light.shadow_matrix));
            plan.source_projection[plan.count] = vp * through;
            const f32 texel = std::max(hi.x - lo.x, hi.y - lo.y) / side;
            plan.texel_world[plan.count] = texel;
            light.aperture[1] = std::max(lights.shadow_bias, 0.0f) * texel / std::max(box.z_far - box.z_near, .01f);
            light.aperture[2] = std::max(lights.shadow_normal_offset, 0.0f) * texel;
        }
        plan.shadow_count += enters_sun ? 1u : 0u;
        ++plan.count;
    }
    return plan;
}
} // namespace kin::gpu
