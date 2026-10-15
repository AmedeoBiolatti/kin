#pragma once
// CPU-only conservative cell proofs used by local shadow gathering.
#include <kin/render3d/mesh.hpp>
#include <algorithm>
#include <cmath>
#include <span>

namespace kin::gpu::shadow_cull {
constexpr u8 outside = 0x80;

// A set bit means this plane still needs individual sphere tests. Zero means
// fully inside; outside is a separate sentinel. Preserve plane order/coefficients.
inline u8 pending_planes(std::span<const Vec4f> planes, const Bounds3& box) {
    const Vec3f c = box.center(), h = box.size() * 0.5f;
    const f32 margin = 1e-5f * (1 + std::max({std::abs(c.x), std::abs(c.y), std::abs(c.z), h.x, h.y, h.z}));
    if (!std::isfinite(margin) || !std::isfinite(c.x) || !std::isfinite(c.y) || !std::isfinite(c.z))
        return static_cast<u8>((1u << planes.size()) - 1);
    u8 pending = 0;
    for (u32 k = 0; k < planes.size(); ++k) {
        const Vec4f& p = planes[k];
        const f32 d = p.x * c.x + p.y * c.y + p.z * c.z + p.w;
        const f32 r = std::abs(p.x) * h.x + std::abs(p.y) * h.y + std::abs(p.z) * h.z;
        if (d + r < -margin) return outside;
        if (d - r <= margin) pending |= static_cast<u8>(1u << k);
    }
    return pending;
}

// Stronger than per-copy intersection: the entire sphere-enclosing cell must
// lie strictly inside the light radius. Near boundaries/nonfinite arithmetic
// returns false, leaving the original distance test in place.
inline bool inside_radius(const Bounds3& box, Vec3f centre, f32 radius) {
    const Vec3f far{std::max(std::abs(box.min.x - centre.x), std::abs(box.max.x - centre.x)),
                    std::max(std::abs(box.min.y - centre.y), std::abs(box.max.y - centre.y)),
                    std::max(std::abs(box.min.z - centre.z), std::abs(box.max.z - centre.z))};
    const f32 margin = 1e-5f * (1 + radius + std::max({std::abs(centre.x), std::abs(centre.y), std::abs(centre.z), far.x, far.y, far.z}));
    const f32 safe = radius - margin, distance = dot(far, far);
    return std::isfinite(safe) && std::isfinite(distance) && safe > 0 && distance < safe * safe;
}
} // namespace kin::gpu::shadow_cull
