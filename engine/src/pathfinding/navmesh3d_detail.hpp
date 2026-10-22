#pragma once

#include <kin/pathfinding/navmesh3d.hpp>
#include <map>
#include <string_view>

namespace kin::navmesh_detail {

struct BoundaryPoint {
    u32 vertex;
    u8 sides; // original triangle edges containing this point
};

// Empty means the boundary cannot be triangulated without losing constraints.
std::vector<std::array<u32, 3>> triangulate_boundary(std::span<const BoundaryPoint> boundary,
                                                     std::span<const Vec3f> vertices, Vec3f up,
                                                     std::vector<u32>* conflicts = nullptr);
class BoundaryWarnings {
    std::map<u32, Vec3f> _sources;

public:
    static constexpr u32 max_reported_faces = 8;
    void record(u32 source, Vec3f point) { _sources.try_emplace(source, point); }
    void warn() const;
};
void warn_limit(Vec3f point, u32 triangle, u32 faces, std::string_view phase, u32 limit = 64);

// Each accepted split is finished, including the last one at the budget limit.
// A read-only probe distinguishes exact-budget convergence from unfinished work.
template <class Split, class Finish, class Pending>
bool refine(Split split, Finish finish, Pending pending, u32 limit = 64) {
    for (u32 pass = 0; pass < limit; ++pass) {
        if (!split())
            return false;
        finish();
    }
    return pending();
}

} // namespace kin::navmesh_detail
