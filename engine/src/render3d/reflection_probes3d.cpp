#include <algorithm>
#include <cmath>
#include <kin/render3d/reflection_probes3d.hpp>
#include <limits>

namespace kin {
namespace {
Vec3f box_centre(const ReflectionProbeSource3D& probe) {
    return probe.position + probe.box_offset;
}
// A point in the boxes' own space (their centre at the origin, their axes the axes).
Vec3f box_local(const ReflectionProbeSource3D& probe, Vec3f point) {
    return probe.rotation.conjugate().rotate(point - box_centre(probe));
}
Vec3f parallax_half(const ReflectionProbeSource3D& probe) {
    const Vec3f e = probe.parallax_extent;
    return e.x > 0.0f && e.y > 0.0f && e.z > 0.0f ? e : probe.extent;
}
} // namespace

f32 reflection_probe_weight(const ReflectionProbeSource3D& probe, Vec3f point) {
    const Vec3f local = box_local(probe, point);
    const f32 inside = std::min(
        {probe.extent.x - std::abs(local.x), probe.extent.y - std::abs(local.y), probe.extent.z - std::abs(local.z)});
    if (!(inside > 0.0f)) {
        return 0.0f;
    }
    return probe.blend > 0.0f ? std::min(inside / probe.blend, 1.0f) : 1.0f;
}

std::optional<Vec3f> reflection_probe_direction(const ReflectionProbeSource3D& probe, Vec3f point, Vec3f direction) {
    if (!probe.parallax) {
        return direction;
    }
    const Vec3f half = parallax_half(probe);
    const Vec3f p = box_local(probe, point);
    if (std::abs(p.x) > half.x || std::abs(p.y) > half.y || std::abs(p.z) > half.z) {
        return std::nullopt;
    }
    const Vec3f d = probe.rotation.conjugate().rotate(direction);
    // Where the ray leaves the box: the nearest of the three faces it heads for.
    f32 t = std::numeric_limits<f32>::max();
    const f32 ps[3] = {p.x, p.y, p.z}, ds[3] = {d.x, d.y, d.z}, hs[3] = {half.x, half.y, half.z};
    for (int a = 0; a < 3; ++a) {
        if (std::abs(ds[a]) > 1e-8f) {
            t = std::min(t, ((ds[a] > 0.0f ? hs[a] : -hs[a]) - ps[a]) / ds[a]);
        }
    }
    const Vec3f hit = box_centre(probe) + probe.rotation.rotate(p + d * t);
    const Vec3f toward = hit - probe.position;
    const f32 n = length(toward);
    return n > 1e-6f ? toward / n : direction;
}

} // namespace kin
