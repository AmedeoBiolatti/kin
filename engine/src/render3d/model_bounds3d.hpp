#pragma once
#include <algorithm>
#include <kin/render3d/mesh3d.hpp>

namespace kin::detail {
class ModelBounds3D {
public:
    void add(const Bounds3D& b, const Mat4f& transform) {
        for (int i = 0; i < 8; ++i) {
            const Vec3f p = transform.apply(
                {(i & 1) ? b.max.x : b.min.x, (i & 2) ? b.max.y : b.min.y, (i & 4) ? b.max.z : b.min.z});
            bounds.min =
                first ? p
                      : Vec3f{std::min(bounds.min.x, p.x), std::min(bounds.min.y, p.y), std::min(bounds.min.z, p.z)};
            bounds.max =
                first ? p
                      : Vec3f{std::max(bounds.max.x, p.x), std::max(bounds.max.y, p.y), std::max(bounds.max.z, p.z)};
            first = false;
        }
    }
    Bounds3D bounds{};

private:
    bool first = true;
};
} // namespace kin::detail
