#pragma once
#include <kin/render3d/errors3d.hpp>

// Navigation meshes for 3D levels: the walkable triangles of a triangle soup,
// joined where they share an edge, and the queries an agent needs to move over
// them (docs/hex_grids.md, "Navigation meshes"). Plain data, built and searched
// on the CPU, with the same result on every run. It needs nothing but kin/core,
// so it is in every build, with or without KIN_EXPERIMENTAL_3D.
//
// Distances are in the units of the vertices (metres in kin's 3D). "Seen from
// above" means looking down `up`, the direction the mesh was built with.

#include <array>
#include <cstddef>
#include <kin/core/math3d.hpp>
#include <kin/core/rng.hpp>
#include <kin/core/types.hpp>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace kin {

// No triangle: the neighbour across a border edge, or a query that found none.
constexpr u32 nav_none = 0xffffffffu;

// How build_navmesh3d reads a triangle soup.
struct NavMeshBuildOptions3D {
    // Which way is up (any length but zero; the mesh keeps it at length 1). A
    // triangle is walked on its front, counter-clockwise seen from above, as
    // MeshData3D winds its triangles: one that faces down is a ceiling.
    Vec3f up{0.0f, 1.0f, 0.0f};
    // A triangle whose normal leans further than this from `up` is too steep
    // to walk on (0 to 90 degrees).
    f32 max_slope_degrees = 45.0f;
    // Vertices this close or closer are one vertex, so that separate pieces of
    // a level join at their seams. 0 joins only vertices at the same position.
    f32 weld_distance = 1e-3f;
    // Nonzero height removes triangles whose vertical headroom intersects any
    // input geometry (including ceilings/walls). Radius then removes triangles
    // within that horizontal distance of a walkable boundary, including the
    // newly blocked areas. Both zero preserve point-agent construction.
    // Conservative: a triangle is removed whole if any part is unsafe. Tessellate
    // the walk surface to control extra margin; this does not split triangles.
    f32 agent_radius = 0.0f;
    f32 agent_height = 0.0f;
};

// One walkable triangle of a NavMesh3D.
struct NavTriangle3D {
    // Indices into NavMesh3D::vertices, counter-clockwise seen from above.
    std::array<u32, 3> vertices{};
    // neighbors[i] is the triangle across the edge from vertices[i] to
    // vertices[(i + 1) % 3], or nav_none where that edge is a border.
    std::array<u32, 3> neighbors{nav_none, nav_none, nav_none};
    // The connected part of the mesh it belongs to, 0 to region_count() - 1.
    u32 region = 0;
    Vec3f centroid{};
};

// A node of the bounding volume hierarchy over a mesh's triangles, which
// nearest() and the queries built on it search.
struct NavMeshNode3D {
    Vec3f min{};
    Vec3f max{};
    // A leaf (count > 0) holds NavMesh3D::node_triangles[first, first + count);
    // an inner node (count == 0) has the children `first` and `first + 1`.
    u32 first = 0;
    u32 count = 0;
};

// A navigation mesh: made by build_navmesh3d, read by the queries below. Plain
// data that can be copied, stored or compared; the queries never change it.
struct NavMesh3D {
    std::vector<Vec3f> vertices;
    std::vector<NavTriangle3D> triangles;
    // The box around every vertex.
    Vec3f bounds_min{};
    Vec3f bounds_max{};
    // How many connected regions the triangles form.
    u32 regions = 0;
    // What it was built with (`up` at length 1).
    NavMeshBuildOptions3D options{};
    // The search tree over the triangles; node 0 is the root.
    std::vector<NavMeshNode3D> nodes;
    std::vector<u32> node_triangles;

    // Whether it has a triangle to walk on.
    bool valid() const { return !triangles.empty(); }
    std::size_t triangle_count() const { return triangles.size(); }
    std::size_t region_count() const { return regions; }
    // The heap memory it holds, plus the struct itself.
    std::size_t memory_bytes() const;
};

// Builds a navigation mesh from a triangle soup: three indices a triangle, or,
// when `indices` is empty, the vertices three by three. It welds vertices
// (`weld_distance`), keeps the triangles that face up and are no steeper than
// `max_slope_degrees` (and drops degenerate and repeated ones), joins two of
// them across an edge when exactly two walkable triangles share it, one on each
// side (touching at a vertex does not join them), and labels connected regions.
// Agent clearance can remove further triangles and split/discard connected regions.
// The triangles are kept as they are, in input order, not merged into polygons.
// Invalid input (an index out of range, a vertex or option that is not finite or
// out of range, a count that is not a whole number of triangles) returns a failed
// result with diagnostics. Valid empty input returns a successful empty mesh.
Result3D<NavMesh3D> build_navmesh3d(std::span<const Vec3f> vertices, std::span<const u32> indices,
                                    const NavMeshBuildOptions3D& options = {});

#ifdef KIN_EXPERIMENTAL_3D
struct MeshData3D;
// The same from a level's mesh (kin/render3d/mesh3d.hpp), its positions moved by
// `transform` (a mirroring transform keeps the triangles facing up).
Result3D<NavMesh3D> build_navmesh3d(const MeshData3D& mesh, const Mat4f& transform = Mat4f::identity(),
                                    const NavMeshBuildOptions3D& options = {});
#endif

// A point on a navigation mesh, and the triangle it is on.
struct NavPoint3D {
    Vec3f position{};
    u32 triangle = nav_none;
};

// The point of the mesh closest to `point`: of the triangles whose closest
// point is within `extents` of `point` on every world axis, the closest (the
// lower index on a tie). Nothing when there is none, or for a point or extents
// that are not finite.
std::optional<NavPoint3D> nearest(const NavMesh3D& mesh, Vec3f point, Vec3f extents = {1.0f, 2.0f, 1.0f});

// How find_path searches.
struct NavPathOptions3D {
    // How far from the mesh the two ends may be (as nearest() takes it).
    Vec3f extents{1.0f, 2.0f, 1.0f};
};

// A short way from `from` to `to` over the mesh, as its corners: `from` and
// `to` moved onto the mesh (nearest()), with the border vertices where the
// path turns between them. An A* search over the triangles' edges finds the
// corridor, the funnel algorithm (string pulling) straightens it, and the
// corridor is moved past any vertex the string turns at that is not on a
// border. The path is taut seen from above, and the shortest there is round
// the obstacles on the sides it takes; a corridor round the other side of an
// obstacle can be shorter, by about 1 % at most in kin's tests. Its corners
// are on the mesh, but a straight segment between two of them can pass under
// a crest or over a dip that it crosses. Empty when an end is not on the mesh,
// or the two are in different regions. Ties are broken the same way on every
// run.
std::vector<Vec3f> find_path(const NavMesh3D& mesh, Vec3f from, Vec3f to, NavPathOptions3D options = {});

// Where a straight walk left the mesh (raycast()).
struct NavRayHit3D {
    // On the border edge, at the mesh's height there.
    Vec3f position{};
    // How far along the walk, 0 at `from` to 1 at `to`.
    f32 fraction = 0.0f;
    // Square to `up` and to the border edge, pointing back onto the mesh.
    Vec3f normal{};
    // The last triangle walked on.
    u32 triangle = nav_none;
};

// Walks the mesh in a straight line, seen from above, from the triangle under
// `from` (nearest(), within `extents`) toward `to`, following the surface.
// Nothing when the walk reaches `to` without leaving the mesh; otherwise where
// it first crossed a border. A `from` that is not on the mesh is a hit there,
// at fraction 0 with no normal.
std::optional<NavRayHit3D> raycast(const NavMesh3D& mesh, Vec3f from, Vec3f to, Vec3f extents = {1.0f, 2.0f, 1.0f});

// The region of `triangle`, or nav_none when it is out of range.
u32 region_of(const NavMesh3D& mesh, u32 triangle);

// A point picked uniformly by area over the whole mesh (a wandering agent's
// next goal); the same key gives the same point. Nothing for an empty mesh.
// Costs a pass over the triangles.
std::optional<NavPoint3D> random_point(const NavMesh3D& mesh, RngKey key);

} // namespace kin
