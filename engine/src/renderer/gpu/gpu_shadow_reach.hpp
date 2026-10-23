#pragma once
// CPU-only directional shadow reach. Keep the reference float arithmetic:
// reciprocal multiplication can change membership at a frustum boundary.
#include <kin/render3d/mesh3d.hpp>
#include <algorithm>
#include <cmath>
#include <span>

namespace kin::gpu::shadow_reach {
struct Ray {
    Vec3f down{}, exit{};
    u8 axes = 0;
    Ray() = default;
    Ray(const Bounds3D& box, Vec3f direction) : down(direction) {
        const f32 d[]{down.x, down.y, down.z}, lo[]{box.min.x, box.min.y, box.min.z}, hi[]{box.max.x, box.max.y, box.max.z};
        f32 ends[3]{};
        for (u32 k = 0; k < 3; ++k) {
            if (d[k] > 1e-6f) { ends[k] = hi[k]; axes |= static_cast<u8>(1u << k); }
            else if (d[k] < -1e-6f) { ends[k] = lo[k]; axes |= static_cast<u8>(1u << k); }
        }
        exit = {ends[0], ends[1], ends[2]};
    }
    f32 leave(Vec3f c) const {
        f32 t = 1e30f;
        if (axes & 1u) t = std::min(t, (exit.x - c.x) / down.x);
        if (axes & 2u) t = std::min(t, (exit.y - c.y) / down.y);
        if (axes & 4u) t = std::min(t, (exit.z - c.z) / down.z);
        return std::max(t, 0.0f);
    }
};

// One sphere, potentially tested against several camera slices. Compute the
// swept endpoint only if a plane rejects the starting sphere, then reuse it.
struct Sweep {
    const Ray& ray;
    Vec3f centre;
    f32 radius;
    Vec3f end{};
    bool ready = false;
    Vec3f endpoint() {
        if (!ready) { end = centre + ray.down * (ray.leave(centre) + radius); ready = true; }
        return end;
    }
    bool reaches(std::span<const Vec4f> planes) {
        for (const Vec4f& p : planes) {
            if (p.x * centre.x + p.y * centre.y + p.z * centre.z + p.w < -radius) {
                const Vec3f e = endpoint();
                if (p.x * e.x + p.y * e.y + p.z * e.z + p.w < -radius) return false;
            }
        }
        return true;
    }
};

// Only skip planes proven inside for every sphere enclosed by this cell.
// An outside starting box cannot reject its swept shadows: they may enter view.
inline u8 pending_planes(std::span<const Vec4f> planes, const Bounds3D& box) {
    const Vec3f c = box.center(), h = box.size() * 0.5f;
    const f32 margin = 1e-5f * (1 + std::max({std::abs(c.x), std::abs(c.y), std::abs(c.z), h.x, h.y, h.z}));
    if (!std::isfinite(margin) || !std::isfinite(c.x) || !std::isfinite(c.y) || !std::isfinite(c.z))
        return static_cast<u8>((1u << planes.size()) - 1);
    u8 pending = 0;
    for (u32 k = 0; k < planes.size(); ++k) {
        const Vec4f& p = planes[k];
        const f32 d = p.x * c.x + p.y * c.y + p.z * c.z + p.w;
        const f32 r = std::abs(p.x) * h.x + std::abs(p.y) * h.y + std::abs(p.z) * h.z;
        if (!std::isfinite(d) || !std::isfinite(r) || d - r <= margin) pending |= static_cast<u8>(1u << k);
    }
    return pending;
}
} // namespace kin::gpu::shadow_reach
