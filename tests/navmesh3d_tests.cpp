#include "result3d_helpers.hpp"

#include <kin/pathfinding/navmesh3d.hpp>
#include <kin/platform/log.hpp>

#ifdef KIN_EXPERIMENTAL_3D
#include <kin/render3d/mesh3d.hpp>
#endif

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace {

using kin::f32;
using kin::u32;
using kin::Vec3f;

struct Soup {
    std::vector<Vec3f> vertices;
    std::vector<u32> indices;

    void append(const Soup& other) {
        const auto base = static_cast<u32>(vertices.size());
        vertices.insert(vertices.end(), other.vertices.begin(), other.vertices.end());
        for (const u32 i : other.indices) {
            indices.push_back(base + i);
        }
    }
};

// A cols x rows grid of `cell`-wide quads on the XZ plane at `height`, two
// triangles each, counter-clockwise from +Y (as meshes::plane winds them),
// without the quads `hole(x, z)` says.
template <class Hole> Soup grid(int cols, int rows, f32 cell, f32 height, Hole hole, Vec3f offset = {}) {
    Soup s;
    for (int z = 0; z <= rows; ++z) {
        for (int x = 0; x <= cols; ++x) {
            s.vertices.push_back(Vec3f{static_cast<f32>(x) * cell, height, static_cast<f32>(z) * cell} + offset);
        }
    }
    for (int z = 0; z < rows; ++z) {
        for (int x = 0; x < cols; ++x) {
            if (hole(x, z)) {
                continue;
            }
            const auto a = static_cast<u32>(z * (cols + 1) + x);
            const u32 b = a + 1;
            const u32 c = a + static_cast<u32>(cols) + 2;
            const u32 d = a + static_cast<u32>(cols) + 1;
            s.indices.insert(s.indices.end(), {a, d, c, a, c, b});
        }
    }
    return s;
}

Soup grid(int cols, int rows, f32 cell = 1.0f, f32 height = 0.0f, Vec3f offset = {}) {
    return grid(cols, rows, cell, height, [](int, int) { return false; }, offset);
}

// The 2 x 2 hole in the middle of a 10 x 10 grid.
bool middle_hole(int x, int z) {
    return x >= 4 && x < 6 && z >= 4 && z < 6;
}

kin::NavMesh3D build(const Soup& s, const kin::NavMeshBuildOptions3D& options = {}) {
    std::string error = "unset";
    kin::NavMesh3D mesh = kin::test::value3d(kin::build_navmesh3d(s.vertices, s.indices, options), error);
    assert(error.empty());
    return mesh;
}

bool near(Vec3f a, Vec3f b, f32 tolerance = 1e-4f) {
    return kin::length(a - b) <= tolerance;
}

f32 path_length(const std::vector<Vec3f>& path) {
    f32 total = 0.0f;
    for (std::size_t i = 1; i < path.size(); ++i) {
        total += kin::length(path[i] - path[i - 1]);
    }
    return total;
}

std::size_t linked_edges(const kin::NavMesh3D& mesh) {
    std::size_t count = 0;
    for (const kin::NavTriangle3D& t : mesh.triangles) {
        for (const u32 n : t.neighbors) {
            count += n != kin::nav_none ? 1 : 0;
        }
    }
    return count;
}

// Every field of a mesh, as bytes, to compare two builds.
std::vector<unsigned char> bytes_of(const kin::NavMesh3D& mesh) {
    std::vector<unsigned char> out;
    const auto put = [&](const void* data, std::size_t size) {
        const auto* p = static_cast<const unsigned char*>(data);
        out.insert(out.end(), p, p + size);
    };
    const auto put_vec3 = [&](Vec3f v) {
        put(&v.x, sizeof(f32));
        put(&v.y, sizeof(f32));
        put(&v.z, sizeof(f32));
    };
    for (const Vec3f v : mesh.vertices) {
        put_vec3(v);
    }
    for (const kin::NavTriangle3D& t : mesh.triangles) {
        put(t.vertices.data(), sizeof(t.vertices));
        put(t.neighbors.data(), sizeof(t.neighbors));
        put(&t.region, sizeof(t.region));
        for (bool step : t.steps)
            put(&step, sizeof(step));
        put_vec3(t.centroid);
    }
    put_vec3(mesh.bounds_min);
    put_vec3(mesh.bounds_max);
    put(&mesh.regions, sizeof(mesh.regions));
    put_vec3(mesh.options.up);
    put(&mesh.options.max_slope_degrees, sizeof(f32));
    put(&mesh.options.weld_distance, sizeof(f32));
    put(&mesh.options.agent_radius, sizeof(f32));
    put(&mesh.options.agent_height, sizeof(f32));
    put(&mesh.options.max_step_height, sizeof(f32));
    for (const kin::NavMeshNode3D& n : mesh.nodes) {
        put_vec3(n.min);
        put_vec3(n.max);
        put(&n.first, sizeof(n.first));
        put(&n.count, sizeof(n.count));
    }
    put(mesh.node_triangles.data(), mesh.node_triangles.size() * sizeof(u32));
    return out;
}

void test_grid_with_hole_adjacency() {
    const kin::NavMesh3D mesh = build(grid(10, 10, 1.0f, 0.0f, middle_hole));
    assert(mesh.valid());
    assert(mesh.triangle_count() == 192); // 96 quads
    assert(mesh.vertices.size() == 120);  // the hole's middle vertex is used by no triangle
    assert(mesh.region_count() == 1);
    // 96 diagonals and 168 edges between quads, each seen from both sides;
    // the other 48 edges are the outside (40) and the hole's rim (8).
    assert(linked_edges(mesh) == 528);
    assert(mesh.triangles.size() * 3 - linked_edges(mesh) == 48);
    for (u32 t = 0; t < mesh.triangles.size(); ++t) {
        for (const u32 n : mesh.triangles[t].neighbors) {
            if (n == kin::nav_none) {
                continue;
            }
            const auto& back = mesh.triangles[n].neighbors;
            assert(back[0] == t || back[1] == t || back[2] == t);
        }
        assert(kin::region_of(mesh, t) == 0);
    }
    assert(kin::region_of(mesh, 192) == kin::nav_none);
    assert((mesh.bounds_min == Vec3f{0.0f, 0.0f, 0.0f}));
    assert((mesh.bounds_max == Vec3f{10.0f, 0.0f, 10.0f}));
    assert(mesh.memory_bytes() > mesh.triangles.size() * sizeof(kin::NavTriangle3D));
}

void test_path_around_the_hole() {
    const kin::NavMesh3D mesh = build(grid(10, 10, 1.0f, 0.0f, middle_hole));
    const f32 optimum = 2.0f * std::sqrt(6.0f * 6.0f + 4.0f * 4.0f);

    // Along the triangles' diagonals, then across them: the hole is in the way both times.
    const std::vector<Vec3f> path = kin::find_path(mesh, {0.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 10.0f});
    assert(path.size() == 3);
    assert((path.front() == Vec3f{0.0f, 0.0f, 0.0f}));
    assert((path.back() == Vec3f{10.0f, 0.0f, 10.0f}));
    assert(near(path[1], {6.0f, 0.0f, 4.0f}) || near(path[1], {4.0f, 0.0f, 6.0f}));
    assert(std::abs(path_length(path) - optimum) <= 0.01f * optimum);

    const std::vector<Vec3f> across = kin::find_path(mesh, {10.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 10.0f});
    assert(across.size() == 3);
    assert(near(across[1], {4.0f, 0.0f, 4.0f}) || near(across[1], {6.0f, 0.0f, 6.0f}));
    assert(std::abs(path_length(across) - optimum) <= 0.01f * optimum);

    // Ends above the mesh are moved onto it; an end too far away gives nothing.
    const std::vector<Vec3f> lifted = kin::find_path(mesh, {0.2f, 0.5f, 0.3f}, {9.5f, 1.5f, 9.0f});
    assert(lifted.size() == 3);
    assert(near(lifted.front(), {0.2f, 0.0f, 0.3f}));
    assert(near(lifted.back(), {9.5f, 0.0f, 9.0f}));
    // In the hole, 1 m from its rim: to the nearest point of the rim (the lowest triangle of four).
    const std::vector<Vec3f> to_rim = kin::find_path(mesh, {0.0f, 0.0f, 0.0f}, {5.0f, 0.0f, 5.0f});
    assert(to_rim.size() == 2 && near(to_rim.back(), {5.0f, 0.0f, 4.0f}));
    assert(kin::find_path(mesh, {0.0f, 0.0f, 0.0f}, {5.0f, 0.0f, 5.0f}, {.extents = {0.5f, 2.0f, 0.5f}}).empty());
    assert(kin::find_path(mesh, {0.0f, 3.0f, 0.0f}, {9.0f, 0.0f, 9.0f}).empty());
    assert(kin::find_path(mesh, {0.0f, 3.0f, 0.0f}, {9.0f, 0.0f, 9.0f}, {.extents = {1.0f, 4.0f, 1.0f}}).size() == 3);

    // Nothing in the way: a straight line, whatever its direction.
    const kin::NavMesh3D open = build(grid(10, 10));
    const Vec3f ends[][2] = {
        {{0.5f, 0.0f, 0.5f}, {9.5f, 0.0f, 8.7f}},
        {{9.5f, 0.0f, 0.5f}, {0.5f, 0.0f, 9.5f}},
        {{0.1f, 0.0f, 9.9f}, {9.9f, 0.0f, 0.1f}},
        {{3.0f, 0.0f, 0.0f}, {3.0f, 0.0f, 10.0f}},
    };
    for (const auto& e : ends) {
        const std::vector<Vec3f> straight = kin::find_path(open, e[0], e[1]);
        assert(straight.size() == 2);
        assert(near(straight[0], e[0]) && near(straight[1], e[1]));
    }
    // One triangle, and one point.
    assert(kin::find_path(open, {0.6f, 0.0f, 0.2f}, {0.8f, 0.0f, 0.3f}).size() == 2);
    assert(kin::find_path(open, {0.6f, 0.0f, 0.2f}, {0.6f, 0.0f, 0.2f}).size() == 1);
}

// A strip 2 m wide: flat for z 0 to 2, a ramp up `degrees` to z = 4, then flat again to z = 6.
Soup floor_ramp_plateau(f32 degrees) {
    const f32 rise = 2.0f * std::tan(degrees * 3.14159265f / 180.0f);
    const f32 heights[] = {0.0f, 0.0f, rise, rise};
    Soup s;
    for (int z = 0; z < 4; ++z) {
        for (int x = 0; x <= 2; ++x) {
            s.vertices.push_back({static_cast<f32>(x), heights[z], 2.0f * static_cast<f32>(z)});
        }
    }
    for (u32 z = 0; z < 3; ++z) {
        for (u32 x = 0; x < 2; ++x) {
            const u32 a = z * 3 + x;
            s.indices.insert(s.indices.end(), {a, a + 3, a + 4, a, a + 4, a + 1});
        }
    }
    return s;
}

void test_slopes() {
    const auto ramp = [](f32 degrees) {
        const f32 h = std::tan(degrees * 3.14159265f / 180.0f);
        return Soup{{{0, 0, 0}, {1, 0, 0}, {1, h, 1}, {0, h, 1}}, {0, 3, 2, 0, 2, 1}};
    };
    const kin::NavMeshBuildOptions3D options{.max_slope_degrees = 45.0f};
    assert(build(ramp(30.0f), options).triangle_count() == 2);
    assert(!build(ramp(60.0f), options).valid());
    assert(build(ramp(60.0f), {.max_slope_degrees = 65.0f}).triangle_count() == 2);
    // Upside down, a floor is a ceiling.
    assert(!build(Soup{{{0, 0, 0}, {1, 0, 0}, {1, 0, 1}}, {0, 1, 2}}).valid());

    const f32 rise = 2.0f * std::tan(30.0f * 3.14159265f / 180.0f);
    const kin::NavMesh3D walkable = build(floor_ramp_plateau(30.0f), options);
    assert(walkable.triangle_count() == 12 && walkable.region_count() == 1);
    const std::vector<Vec3f> up_the_ramp = kin::find_path(walkable, {0.5f, 0.0f, 0.5f}, {0.5f, rise, 5.5f});
    assert(up_the_ramp.size() == 2);
    assert(near(up_the_ramp.back(), {0.5f, rise, 5.5f}));
    const std::vector<Vec3f> diagonal = kin::find_path(walkable, {0.1f, 0.0f, 0.1f}, {1.9f, rise, 5.9f});
    assert(diagonal.size() == 2);

    const kin::NavMesh3D steep = build(floor_ramp_plateau(60.0f), options);
    assert(steep.triangle_count() == 8 && steep.region_count() == 2);
    const f32 cliff = 2.0f * std::tan(60.0f * 3.14159265f / 180.0f);
    assert(kin::find_path(steep, {1.0f, 0.0f, 0.5f}, {1.0f, cliff, 5.5f}).empty());
}

void test_islands() {
    Soup two = grid(3, 3);
    two.append(grid(3, 3, 1.0f, 0.0f, Vec3f{10.0f, 0.0f, 0.0f}));
    const kin::NavMesh3D mesh = build(two);
    assert(mesh.triangle_count() == 36 && mesh.region_count() == 2);
    const auto a = kin::nearest(mesh, {1.5f, 0.0f, 1.5f});
    const auto b = kin::nearest(mesh, {11.5f, 0.0f, 1.5f});
    assert(a && b);
    assert(kin::region_of(mesh, a->triangle) == 0 && kin::region_of(mesh, b->triangle) == 1);
    assert(kin::find_path(mesh, {1.5f, 0.0f, 1.5f}, {11.5f, 0.0f, 1.5f}).empty());
    assert(kin::find_path(mesh, {0.5f, 0.0f, 0.5f}, {2.5f, 0.0f, 2.5f}).size() == 2);
    assert(kin::find_path(mesh, {10.5f, 0.0f, 2.5f}, {12.5f, 0.0f, 0.5f}).size() == 2);

    // Two squares that touch at one corner are not joined.
    const kin::NavMesh3D corner = build(grid(2, 2, 1.0f, 0.0f, [](int x, int z) { return x != z; }));
    assert(corner.triangle_count() == 4 && corner.region_count() == 2);
    assert(kin::find_path(corner, {0.5f, 0.0f, 0.5f}, {1.5f, 0.0f, 1.5f}).empty());
}

void test_weld() {
    // Two quads as a soup of separate triangles; the second quad's shared edge is 0.1 mm off.
    const f32 e = 1e-4f;
    const Soup soup{{
                        {0, 0, 0},
                        {0, 0, 1},
                        {1, 0, 1},
                        {0, 0, 0},
                        {1, 0, 1},
                        {1, 0, 0},
                        {1 + e, 0, 0},
                        {1 + e, 0, 1},
                        {2, 0, 1},
                        {1 + e, 0, 0},
                        {2, 0, 1},
                        {2, 0, 0},
                    },
                    {}};
    const kin::NavMesh3D welded = build(soup);
    assert(welded.triangle_count() == 4 && welded.vertices.size() == 6 && welded.region_count() == 1);
    assert(linked_edges(welded) == 6);
    // The first of a cluster keeps its position.
    assert((welded.vertices[0] == Vec3f{0, 0, 0}));
    const kin::NavMesh3D apart = build(soup, {.weld_distance = 0.0f});
    assert(apart.triangle_count() == 4 && apart.vertices.size() == 8 && apart.region_count() == 2);
    // A repeated triangle, and one that welds to a point, are dropped.
    const Soup repeats{
        {{0, 0, 0}, {0, 0, 1}, {1, 0, 1}, {1, 0, 1}, {0, 0, 0}, {0, 0, 1}, {0, 0, 0}, {0, 0, e}, {e, 0, 0}}, {}};
    assert(build(repeats).triangle_count() == 1);
}

void test_nearest() {
    const kin::NavMesh3D mesh = build(grid(10, 10));
    const auto inside = kin::nearest(mesh, {2.25f, 0.5f, 2.75f});
    assert(inside && near(inside->position, {2.25f, 0.0f, 2.75f}));
    // Quad (2, 2)'s triangle toward -X +Z: corners (2, 2), (2, 3), (3, 3).
    const kin::NavTriangle3D& t = mesh.triangles[inside->triangle];
    assert((t.centroid == (Vec3f{2, 0, 2} + Vec3f{2, 0, 3} + Vec3f{3, 0, 3}) / 3.0f));
    assert(kin::nearest(mesh, {5.0f, 5.0f, 5.0f}) == std::nullopt); // 5 m above
    assert(kin::nearest(mesh, {5.0f, 5.0f, 5.0f}, {1.0f, 6.0f, 1.0f}));
    const auto beside = kin::nearest(mesh, {10.5f, 0.0f, 5.5f});
    assert(beside && near(beside->position, {10.0f, 0.0f, 5.5f}));
    assert(!kin::nearest(mesh, {11.5f, 0.0f, 5.5f}));
    assert(!kin::nearest(mesh, {std::nanf(""), 0.0f, 0.0f}));
    assert(!kin::nearest(kin::NavMesh3D{}, {0.0f, 0.0f, 0.0f}));

    // Two floors 3 m apart: the closer one.
    Soup floors = grid(10, 10);
    floors.append(grid(10, 10, 1.0f, 3.0f));
    const kin::NavMesh3D stacked = build(floors);
    assert(stacked.region_count() == 2);
    const auto low = kin::nearest(stacked, {5.3f, 1.0f, 5.6f});
    const auto high = kin::nearest(stacked, {5.3f, 2.0f, 5.6f});
    assert(low && near(low->position, {5.3f, 0.0f, 5.6f}));
    assert(high && near(high->position, {5.3f, 3.0f, 5.6f}));
    // A point exactly between them takes the lower triangle number: the first floor's.
    const auto middle = kin::nearest(stacked, {5.3f, 1.5f, 5.6f});
    assert(middle && middle->position.y == 0.0f);
}

void test_raycast() {
    const kin::NavMesh3D mesh = build(grid(10, 10, 1.0f, 0.0f, middle_hole));
    const auto hole = kin::raycast(mesh, {1.0f, 0.0f, 4.5f}, {9.0f, 0.0f, 4.5f});
    assert(hole && near(hole->position, {4.0f, 0.0f, 4.5f}));
    assert(std::abs(hole->fraction - 3.0f / 8.0f) < 1e-5f);
    assert(near(hole->normal, {-1.0f, 0.0f, 0.0f}));
    assert(hole->triangle != kin::nav_none);
    assert(!kin::raycast(mesh, {1.0f, 0.0f, 1.0f}, {9.0f, 0.0f, 2.0f}));
    // Along a row of edges, through their vertices.
    assert(!kin::raycast(mesh, {0.5f, 0.0f, 3.0f}, {9.5f, 0.0f, 3.0f}));
    assert(!kin::raycast(mesh, {0.0f, 0.0f, 0.0f}, {3.0f, 0.0f, 3.0f}));
    const auto edge = kin::raycast(mesh, {5.0f, 0.0f, 9.0f}, {5.0f, 0.0f, 12.0f});
    assert(edge && near(edge->position, {5.0f, 0.0f, 10.0f}) && near(edge->normal, {0.0f, 0.0f, -1.0f}));
    assert(std::abs(edge->fraction - 1.0f / 3.0f) < 1e-5f);
    // Not on the mesh to begin with.
    const auto off = kin::raycast(mesh, {50.0f, 0.0f, 50.0f}, {1.0f, 0.0f, 1.0f});
    assert(off && off->fraction == 0.0f && off->triangle == kin::nav_none);
    // It follows a slope.
    const kin::NavMesh3D ramp = build(floor_ramp_plateau(30.0f));
    const auto over = kin::raycast(ramp, {1.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 9.0f});
    const f32 rise = 2.0f * std::tan(30.0f * 3.14159265f / 180.0f);
    assert(over && near(over->position, {1.0f, rise, 6.0f}));
}

void test_up_axis() {
    // The same holed grid standing in the XY plane, walked with +Z up.
    Soup s;
    for (int y = 0; y <= 10; ++y) {
        for (int x = 0; x <= 10; ++x) {
            s.vertices.push_back({static_cast<f32>(x), static_cast<f32>(y), 0.0f});
        }
    }
    for (u32 y = 0; y < 10; ++y) {
        for (u32 x = 0; x < 10; ++x) {
            if (middle_hole(static_cast<int>(x), static_cast<int>(y))) {
                continue;
            }
            const u32 a = y * 11 + x;
            s.indices.insert(s.indices.end(), {a, a + 1, a + 12, a, a + 12, a + 11});
        }
    }
    assert(!build(s).valid()); // walls, with +Y up
    const kin::NavMesh3D mesh = build(s, {.up = {0.0f, 0.0f, 2.0f}});
    assert(mesh.triangle_count() == 192 && mesh.region_count() == 1);
    assert((mesh.options.up == Vec3f{0.0f, 0.0f, 1.0f}));
    const std::vector<Vec3f> path = kin::find_path(mesh, {0, 0, 0}, {10, 10, 0});
    assert(path.size() == 3);
    const f32 optimum = 2.0f * std::sqrt(52.0f);
    assert(std::abs(path_length(path) - optimum) <= 0.01f * optimum);
}

void test_invalid_input() {
    const std::vector<Vec3f> triangle{{0, 0, 0}, {0, 0, 1}, {1, 0, 0}};
    const std::vector<u32> indices{0, 1, 2};
    std::string error;
    assert(!kin::test::value3d(kin::build_navmesh3d(triangle, std::vector<u32>{0, 1, 3}, {}), error).valid());
    assert(error.find("out of range") != std::string::npos);
    assert(!kin::test::value3d(kin::build_navmesh3d(triangle, std::vector<u32>{0, 1, 2, 0}, {}), error).valid() &&
           !error.empty());
    const std::vector<Vec3f> bad{{0, 0, 0}, {0, std::nanf(""), 1}, {1, 0, 0}};
    assert(!kin::test::value3d(kin::build_navmesh3d(bad, indices, {}), error).valid());
    assert(error.find("not finite") != std::string::npos);
    assert(!kin::test::value3d(kin::build_navmesh3d(triangle, indices, {.up = {}}), error).valid() && !error.empty());
    assert(!kin::test::value3d(kin::build_navmesh3d(triangle, indices, {.max_slope_degrees = 120.0f}), error).valid());
    assert(!kin::test::value3d(kin::build_navmesh3d(triangle, indices, {.weld_distance = -1.0f}), error).valid());
    assert(
        !kin::test::value3d(kin::build_navmesh3d(triangle, indices, {.agent_radius = std::nanf("")}), error).valid());
    // Nothing to build is not an error.
    assert(!kin::test::value3d(kin::build_navmesh3d(std::span<const Vec3f>{}, {}, {}), error).valid() && error.empty());
    const kin::NavMesh3D one = kin::test::value3d(
        kin::build_navmesh3d(triangle, indices, {.agent_radius = 0.4f, .agent_height = 1.8f}), error);
    assert(!one.valid() && error.empty()); // the whole coarse triangle touches an unsafe border
    assert(one.options.agent_radius == 0.4f && one.options.agent_height == 1.8f);

    // Failures carry their diagnostics without requiring logging or an owner.
    const auto rejected = kin::build_navmesh3d(triangle, std::vector<u32>{0, 1, 9});
    assert(!rejected && !rejected.value && rejected.diagnostics.size() == 1);
    const auto empty = kin::build_navmesh3d(std::span<const Vec3f>{}, {});
    assert(empty && !empty->valid() && empty.diagnostics.empty());
}

// Clearance changes every query's geometry, rather than only blocking A* portals.
void test_agent_clearance() {
    const Soup doorway = grid(60, 30, .1f, 0.f, [](int x, int z) { return x >= 20 && x < 40 && (z < 13 || z >= 16); });
    const Vec3f from{1, 0, 1.45f}, to{5, 0, 1.45f};
    const auto point = build(doorway);
    const auto small = build(doorway, {.agent_radius = .04f});
    const auto large = build(doorway, {.agent_radius = .2f});
    assert(!kin::find_path(point, from, to).empty());
    assert(!kin::find_path(small, from, to).empty());
    assert(kin::find_path(large, from, to).empty() && large.region_count() == 2);
    assert(!kin::nearest(large, {3, 0, 1.45f}, {.02f, .1f, .02f}));
    const auto edge = kin::raycast(large, {1, 0, 1}, {-1, 0, 1});
    assert(edge && edge->position.x >= .2f - 1e-5f);
    for (kin::u64 i = 0; i < 200; ++i) {
        const auto p = kin::random_point(large, kin::make_key(i));
        assert(p && p->position.x >= .2f - 1e-5f && p->position.x <= 5.8f + 1e-5f && p->position.z >= .2f - 1e-5f &&
               p->position.z <= 2.8f + 1e-5f);
    }

    Soup low = grid(30, 10, .1f);
    Soup roof = grid(4, 10, .1f, 1.5f, {1.3f, 0, 0});
    for (std::size_t i = 0; i < roof.indices.size(); i += 3)
        std::swap(roof.indices[i], roof.indices[i + 1]);
    low.append(roof);
    const auto short_agent = build(low, {.agent_radius = .04f, .agent_height = 1.2f});
    const auto tall_agent = build(low, {.agent_radius = .04f, .agent_height = 1.8f});
    assert(!kin::find_path(short_agent, {.5f, 0, .5f}, {2.5f, 0, .5f}).empty());
    assert(kin::find_path(tall_agent, {.5f, 0, .5f}, {2.5f, 0, .5f}).empty());
    assert(!kin::nearest(tall_agent, {1.5f, 0, .5f}, {.02f, .1f, .02f}));
    assert(bytes_of(tall_agent) == bytes_of(build(low, {.agent_radius = .04f, .agent_height = 1.8f})));

    // A small sloping ceiling misses vertex/centroid samples but intersects the footprint.
    Soup partial = grid(2, 2);
    partial.append({{{.05f, .55f, .05f}, {.08f, .65f, .05f}, {.05f, .75f, .08f}}, {0, 1, 2}});
    assert(kin::nearest(build(partial), {.06f, 0, .06f}, {.001f, .01f, .001f}));
    assert(!kin::nearest(build(partial, {.agent_height = 1}), {.06f, 0, .06f}, {.001f, .01f, .001f}));

    // A vertical wall embedded in otherwise continuous floor is an obstruction too.
    Soup wall = grid(30, 10, .1f);
    wall.append({{{1.5f, 0, 0}, {1.5f, 2, 0}, {1.5f, 2, 1}, {1.5f, 0, 1}}, {0, 1, 2, 0, 2, 3}});
    assert(kin::find_path(build(wall, {.agent_radius = .04f, .agent_height = 1.2f}), {.5f, 0, .5f}, {2.5f, 0, .5f})
               .empty());

    // A 1.0 m doorway remains usable by an 0.8 m-wide agent. Vertex-only
    // jamb contacts must not consume another pair of cells on either side.
    Soup door = grid(40, 40, .1f);
    for (const auto z : {std::pair{0.f, 1.5f}, std::pair{2.5f, 4.f}})
        door.append({{{2, 0, z.first}, {2, 2.3f, z.first}, {2, 2.3f, z.second}, {2, 0, z.second}}, {0, 1, 2, 0, 2, 3}});
    door.append({{{2, 2, 1.5f}, {2, 2.3f, 1.5f}, {2, 2.3f, 2.5f}, {2, 2, 2.5f}}, {0, 1, 2, 0, 2, 3}});
    auto distant_ramp = grid(20, 10, .1f, 0, {4, 0, 0});
    for (auto& p : distant_ramp.vertices)
        p.y = (p.x - 4) * .5f;
    door.append(distant_ramp);
    assert(!kin::find_path(build(door, {.agent_radius = .4f, .agent_height = 1.8f}), {1, 0, 2}, {3, 0, 2}).empty());
    Soup room = grid(20, 20, .1f);
    for (const auto x : {0.f, 2.f})
        room.append({{{x, 0, 0}, {x, 2, 0}, {x, 2, 2}, {x, 0, 2}}, {0, 1, 2, 0, 2, 3}});
    for (const auto z : {0.f, 2.f})
        room.append({{{0, 0, z}, {0, 2, z}, {2, 2, z}, {2, 0, z}}, {0, 1, 2, 0, 2, 3}});
    assert(build(room, {.agent_height = 1.8f}).region_count() == 1);

    // Interior wall lines preserve the two corner fan triangles' links.
    Soup interior = grid(40, 40, .1f);
    for (const f32 x : {1.f, 3.f})
        interior.append({{{x, 0, 1}, {x, 2, 1}, {x, 2, 3}, {x, 0, 3}}, {0, 1, 2, 0, 2, 3}});
    interior.append({{{1, 0, 3}, {1, 2, 3}, {3, 2, 3}, {3, 0, 3}}, {0, 1, 2, 0, 2, 3}});
    for (const auto span : {std::pair{1.f, 1.5f}, std::pair{2.5f, 3.f}})
        interior.append(
            {{{span.first, 0, 1}, {span.first, 2, 1}, {span.second, 2, 1}, {span.second, 0, 1}}, {0, 1, 2, 0, 2, 3}});
    const auto no_islands = build(interior, {.agent_height = 1.8f});
    assert(no_islands.region_count() == 1);
    assert(!kin::find_path(no_islands, {1.01f, 0, 2.99f}, {2, 0, .5f}).empty());
    // Other layers' borders cannot erode this floor merely because they project onto it.
    Soup layers = grid(20, 20, .1f);
    layers.append(grid(8, 8, .1f, 3, {.6f, 0, .6f}));
    const auto layered = build(layers, {.agent_radius = .1f, .agent_height = 1.8f});
    assert(kin::nearest(layered, {.6f, 0, 1}, {.01f, .1f, .01f}));
    assert(kin::nearest(layered, {1, 3, 1}, {.01f, .1f, .01f}));

    // A ramp joins the two storeys into ONE region, away from the balcony edge.
    // Local clearance must still leave the ground directly below that edge.
    Soup connected = grid(90, 30, .1f, 0, [](int x, int z) {
        // The ramp meets ground on its far side; avoid a folded/nonmanifold
        // seam with a ground triangle directly below its final triangle.
        return x == 79 && z >= 10 && z < 20;
    });
    connected.append(grid(20, 10, .1f, 2, {2, 0, 1}));
    Soup ramp = grid(40, 10, .1f, 0, {4, 0, 1});
    for (auto& p : ramp.vertices)
        p.y = (8 - p.x) * .5f;
    connected.append(ramp);
    assert(build(connected).region_count() == 1);
    const auto balcony = build(connected, {.agent_radius = .1f, .agent_height = 1.8f});
    assert(kin::nearest(balcony, {2, 0, 1.5f}, {.01f, .01f, .01f}));
    assert(!kin::nearest(balcony, {2, 2, 1.5f}, {.01f, .01f, .01f}));

    // The same ramp on uncut continuous floor: headroom removal resolves the
    // three-face seam before erosion, so the foot is not treated as a wall.
    Soup uncut = grid(90, 30, .1f);
    uncut.append(grid(20, 10, .1f, 2, {2, 0, 1}));
    uncut.append(ramp);
    const auto ramp_walk = build(uncut, {.agent_radius = .1f, .agent_height = 1.8f});
    assert(!kin::find_path(ramp_walk, {8.5f, 0, 1.5f}, {3, 2, 1.5f}).empty());
    for (f32 radius : {0.f, .1f})
        assert(!kin::find_path(build(uncut, {.agent_radius = radius}), {8.5f, 0, 1.5f}, {3, 2, 1.5f}).empty());

    // Vertical risers touch tread boundaries without occupying their interiors.
    Soup stairs;
    for (int step = 0; step < 4; ++step) {
        const f32 x = f32(step), y = f32(step) * .2f;
        stairs.append(grid(1, 1, 1, y, {x, 0, 0}));
        stairs.append({{{x + 1, y, 0}, {x + 1, y + .2f, 0}, {x + 1, y + .2f, 1}, {x + 1, y, 1}}, {0, 1, 2, 0, 2, 3}});
    }
    const auto treads = build(stairs, {.agent_height = 1.8f});
    for (int step = 0; step < 4; ++step)
        assert(kin::nearest(treads, {f32(step) + .5f, f32(step) * .2f, .5f}, {.01f, .01f, .01f}));

    // A vertical character's uphill centre is higher than its downhill footprint.
    Soup sloped = grid(30, 20, .1f);
    for (auto& p : sloped.vertices)
        p.y = p.x * .5f;
    Soup parallel = sloped;
    for (auto& p : parallel.vertices)
        p.y += 1.25f;
    for (std::size_t i = 0; i < parallel.indices.size(); i += 3)
        std::swap(parallel.indices[i], parallel.indices[i + 1]);
    sloped.append(parallel);
    assert(kin::nearest(build(sloped, {.agent_radius = .2f, .agent_height = 1.0f}), {1, .5f, 1}, {.01f, .01f, .01f}));
    assert(!kin::nearest(build(sloped, {.agent_radius = .2f, .agent_height = 1.2f}), {1, .5f, 1}, {.01f, .01f, .01f}));

    const auto joined = build(stairs, {.agent_height = 1.8f, .max_step_height = .21f});
    const auto route = kin::find_path(joined, {.25f, 0, .5f}, {3.75f, .6f, .5f});
    assert(joined.region_count() == 1 && route.size() >= 8);
    assert(near(route.front(), {.25f, 0, .5f}) && near(route.back(), {3.75f, .6f, .5f}));
    assert(!kin::find_path(joined, {3.75f, .6f, .5f}, {.25f, 0, .5f}).empty());
    const auto too_high = build(stairs, {.agent_height = 1.8f, .max_step_height = .19f});
    assert(kin::find_path(too_high, {.25f, 0, .5f}, {3.75f, .6f, .5f}).empty());

    // Tessellated stairs need clearance through every join, not just a point path.
    Soup wide_stairs;
    for (int step = 0; step < 4; ++step) {
        const f32 x = f32(step), y = f32(step) * .2f;
        wide_stairs.append(grid(10, 30, .1f, y, {x, 0, 0}));
        wide_stairs.append(
            {{{x + 1, y, 0}, {x + 1, y + .2f, 0}, {x + 1, y + .2f, 3}, {x + 1, y, 3}}, {0, 1, 2, 0, 2, 3}});
    }
    for (const f32 radius : {0.f, .1f, .3f, .4f}) {
        const auto walk = build(wide_stairs, {.agent_radius = radius, .agent_height = 1.8f, .max_step_height = .21f});
        assert(!kin::find_path(walk, {.5f, 0, 1.5f}, {3.5f, .6f, 1.5f}).empty());
        assert(!kin::find_path(walk, {3.5f, .6f, 1.5f}, {.5f, 0, 1.5f}).empty());
    }
    Soup blocked_steps = wide_stairs;
    blocked_steps.append({{{2, .4f, 0}, {2, 2.5f, 0}, {2, 2.5f, 3}, {2, .4f, 3}}, {0, 1, 2, 0, 2, 3}});
    for (const f32 radius : {0.f, .3f})
        assert(kin::find_path(
                   build(blocked_steps, {.agent_radius = radius, .agent_height = 1.8f, .max_step_height = .21f}),
                   {.5f, 0, 1.5f}, {3.5f, .6f, 1.5f})
                   .empty());

    Soup kerb = grid(20, 30, .1f);
    auto raised_ramp = grid(20, 30, .1f, .2f, {2, 0, 0});
    for (auto& point : raised_ramp.vertices)
        point.y += (point.x - 2) * .5f;
    kerb.append(raised_ramp);
    kerb.append(grid(8, 30, .1f, 1.85f));
    const auto kerb_walk = build(kerb, {.agent_radius = .3f, .agent_height = 1.8f, .max_step_height = .21f});
    assert(kin::nearest(kerb_walk, {.5f, 0, 1.5f}, {.01f, .01f, .01f}));
    assert(!kin::find_path(kerb_walk, {1.4f, 0, 1.5f}, {3.5f, .95f, 1.5f}).empty());
    // A straight shortcut across a raised rectangle must retain its vertical
    // step points, even if the search initially chose a corridor around it.
    Soup kerb_map;
    const std::array<std::array<int, 4>, 6> patches{
        {{21, 14, 3, 3}, {9, 7, 5, 6}, {5, 11, 2, 5}, {7, 0, 6, 6}, {21, 23, 1, 3}, {20, 11, 4, 6}}};
    for (int x = 0; x < 24; ++x)
        for (int z = 0; z < 24; ++z) {
            bool raised = false;
            for (const auto& patch : patches)
                raised |= x >= patch[0] && x < patch[0] + patch[2] && z >= patch[1] && z < patch[1] + patch[3];
            kerb_map.append(grid(1, 1, .25f, raised ? .15f : 0.f, {x * .25f, 0, z * .25f}));
        }
    const auto kerbs = build(kerb_map, {.max_step_height = .25f});
    const auto kerb_route = kin::find_path(kerbs, {3.882f, 0, .981f}, {.969f, 0, 2.608f});
    assert(!kerb_route.empty());
    for (std::size_t i = 1; i < kerb_route.size(); ++i) {
        const auto delta = kerb_route[i] - kerb_route[i - 1];
        const auto horizontal = std::sqrt(delta.x * delta.x + delta.z * delta.z);
        if (horizontal < 1e-4f)
            continue;
        const int samples = std::max(2, int(horizontal / .01f));
        for (int k = 1; k < samples; ++k) {
            const auto at = kerb_route[i - 1] + delta * (f32(k) / samples);
            const auto surface = kin::nearest(kerbs, at, {.002f, .2f, .002f});
            assert(surface && std::abs(surface->position.y - at.y) < .01f);
        }
    }
    // Exported stair noses overlap the lower tread by 3 cm and use a
    // coarse strip beside a finely tessellated tread (different edge splits).
    Soup noses = grid(20, 20, .1f, 0, {-2, 0, 0});
    for (int k = 0; k < 4; ++k) {
        const f32 x = f32(k), y = (k + 1) * .2f;
        noses.append(grid(10, 20, .1f, y, {x, 0, 0}));
        noses.append({{{x - .03f, y, 0}, {x - .03f, y, 2}, {x, y, 2}, {x, y, 0}}, {0, 1, 2, 0, 2, 3}});
        noses.append({{{x, y - .2f, 0}, {x, y - .02f, 0}, {x, y - .02f, 2}, {x, y - .2f, 2}}, {0, 1, 2, 0, 2, 3}});
        noses.append({{{x - .03f, y - .02f, 0}, {x - .03f, y, 0}, {x - .03f, y, 2}, {x - .03f, y - .02f, 2}},
                      {0, 1, 2, 0, 2, 3}});
    }
    noses.append(grid(20, 20, .1f, .8f, {4, 0, 0}));
    for (f32 radius : {0.f, .1f, .3f, .4f}) {
        const auto mesh = build(noses, {.agent_radius = radius, .agent_height = 1.8f, .max_step_height = .25f});
        const auto up = kin::find_path(mesh, {-1, 0, 1}, {5, .8f, 1});
        assert(!up.empty());
        assert(!kin::find_path(mesh, {5, .8f, 1}, {-1, 0, 1}).empty());
        int steps = 0;
        for (std::size_t i = 1; i < up.size(); ++i)
            if (up[i].y - up[i - 1].y > .15f)
                ++steps;
        assert(steps == 4);
        for (std::size_t i = 1; i < up.size(); ++i)
            assert(up[i].x >= up[i - 1].x - 1e-5f); // no backward nosing jog
        assert(bytes_of(mesh) ==
               bytes_of(build(noses, {.agent_radius = radius, .agent_height = 1.8f, .max_step_height = .25f})));
    }
    // Grid triangles with cuts along a short leg used to lose every cut.
    // Exercise alternating tessellations, nosings, both clearance modes and
    // exact-position welding. A continuous supporting floor is normal authoring.
    for (const f32 cell : {.05f, .25f, .5f})
        for (const bool continuous : {false, true})
            for (const f32 nose : {0.f, .03f}) {
                Soup mixed = grid(continuous ? 80 : 20, 20, .1f, 0, {-2, 0, 0});
                for (int k = 0; k < 4; ++k) {
                    const f32 x = f32(k), y = (k + 1) * .2f;
                    const f32 density = k % 2 ? .1f : cell;
                    mixed.append(
                        grid(int(std::round(1 / density)), int(std::round(2 / density)), density, y, {x, 0, 0}));
                    if (nose > 0) {
                        mixed.append({{{x - nose, y, 0}, {x - nose, y, 2}, {x, y, 2}, {x, y, 0}}, {0, 1, 2, 0, 2, 3}});
                        mixed.append(
                            {{{x - nose, y - .02f, 0}, {x - nose, y, 0}, {x - nose, y, 2}, {x - nose, y - .02f, 2}},
                             {0, 1, 2, 0, 2, 3}});
                    }
                    mixed.append({{{x, y - .2f, 0},
                                   {x, y - (nose > 0 ? .02f : 0.f), 0},
                                   {x, y - (nose > 0 ? .02f : 0.f), 2},
                                   {x, y - .2f, 2}},
                                  {0, 1, 2, 0, 2, 3}});
                }
                mixed.append(grid(20, 20, .1f, .8f, {4, 0, 0}));
                for (const f32 height : {0.f, 1.8f})
                    for (const f32 radius : {0.f, .1f, .3f, .4f}) {
                        const auto options = kin::NavMeshBuildOptions3D{.weld_distance = 0,
                                                                        .agent_radius = radius,
                                                                        .agent_height = height,
                                                                        .max_step_height = .25f};
                        const auto mesh = build(mixed, options);
                        if (kin::find_path(mesh, {-1, 0, 1}, {5, .8f, 1}).empty())
                            std::fprintf(stderr, "mixed stair cell=%g continuous=%d nose=%g height=%g radius=%g\n",
                                         cell, continuous, nose, height, radius);
                        assert(!kin::find_path(mesh, {-1, 0, 1}, {5, .8f, 1}).empty());
                        assert(!kin::find_path(mesh, {5, .8f, 1}, {-1, 0, 1}).empty());
                    }
                mixed.append({{{2, .6f, 0}, {2, 3, 0}, {2, 3, 2}, {2, .6f, 2}}, {0, 1, 2, 0, 2, 3}});
                assert(
                    kin::find_path(build(mixed, {.agent_radius = .3f, .agent_height = 1.8f, .max_step_height = .25f}),
                                   {-1, 0, 1}, {5, .8f, 1})
                        .empty());
            }
    // A low roof must use nearby ramp clearance, while distant floor keeps
    // its own headroom. This fails if propagation is simply removed.
    Soup margin = grid(60, 40, .1f);
    auto slope = grid(20, 40, .1f, 0, {6, 0, 0});
    for (auto& point : slope.vertices)
        point.y = (point.x - 6) * .577f;
    margin.append(slope);
    margin.append({{{0, 1.9f, 0}, {5.8f, 1.9f, 0}, {5.8f, 1.9f, 4}, {0, 1.9f, 4}}, {0, 1, 2, 0, 2, 3}});
    const auto low_margin = build(margin, {.agent_radius = .4f, .agent_height = 1.8f});
    assert(!kin::nearest(low_margin, {5.5f, 0, 2}, {.01f, .01f, .01f}));
    assert(kin::nearest(low_margin, {2, 0, 2}, {.01f, .01f, .01f}));
    for (auto& point : wide_stairs.vertices)
        point = {point.x, -point.z, point.y};
    for (const f32 radius : {.1f, .3f, .4f})
        assert(!kin::find_path(
                    build(wide_stairs,
                          {.up = {0, 0, 1}, .agent_radius = radius, .agent_height = 1.8f, .max_step_height = .21f}),
                    {.5f, -1.5f, 0}, {3.5f, -1.5f, .6f})
                    .empty());

    for (auto& vertex : stairs.vertices)
        vertex = {vertex.x, -vertex.z, vertex.y};
    const auto rotated_steps = build(stairs, {.up = {0, 0, 1}, .agent_height = 1.8f, .max_step_height = .21f});
    assert(!kin::find_path(rotated_steps, {.25f, -.5f, 0}, {3.75f, -.5f, .6f}).empty());

    // Neither clearance dimension assumes Y-up.
    for (auto& vertex : low.vertices)
        vertex = {vertex.x, -vertex.z, vertex.y};
    const auto standing = build(low, {.up = {0, 0, 1}, .agent_radius = .04f, .agent_height = 1.8f});
    assert(kin::find_path(standing, {.5f, -.5f, 0}, {2.5f, -.5f, 0}).empty());
    assert(!kin::nearest(standing, {1.5f, -.5f, 0}, {.02f, .02f, .1f}));
}

void test_random_point() {
    const kin::NavMesh3D mesh = build(grid(10, 10, 1.0f, 0.0f, middle_hole));
    kin::RngKey key = kin::make_key(7);
    f32 lowest_x = 10.0f;
    f32 highest_x = 0.0f;
    for (int i = 0; i < 200; ++i) {
        const auto [use, next] = kin::split(key);
        key = next;
        const auto p = kin::random_point(mesh, use);
        assert(p && p->triangle < mesh.triangle_count());
        const auto on = kin::nearest(mesh, p->position, {0.01f, 0.01f, 0.01f});
        assert(on && near(on->position, p->position));
        assert(!(p->position.x > 4.0f && p->position.x < 6.0f && p->position.z > 4.0f && p->position.z < 6.0f));
        lowest_x = std::min(lowest_x, p->position.x);
        highest_x = std::max(highest_x, p->position.x);
        const auto again = kin::random_point(mesh, use);
        assert(again && again->position == p->position && again->triangle == p->triangle);
    }
    assert(lowest_x < 1.0f && highest_x > 9.0f);
    assert(!kin::random_point(kin::NavMesh3D{}, key));
}

void test_deterministic() {
    const Soup soup = grid(10, 10, 1.0f, 0.0f, middle_hole);
    const kin::NavMesh3D a = build(soup);
    const kin::NavMesh3D b = build(soup);
    assert(bytes_of(a) == bytes_of(b));
    assert(kin::find_path(a, {0, 0, 0}, {10, 0, 10}) == kin::find_path(b, {0, 0, 0}, {10, 0, 10}));
}

// Whether segment p-q passes through the inside of the square `side` wide from (x0, z0) on the XZ plane.
bool crosses_square(Vec3f p, Vec3f q, f32 x0, f32 z0, f32 side) {
    const f32 e = 1e-3f; // touching a side or a corner is not crossing
    f32 t0 = 0.0f;
    f32 t1 = 1.0f;
    const auto clip = [&](f32 from, f32 d, f32 low, f32 high) {
        if (std::abs(d) < 1e-12f) {
            return from > low && from < high;
        }
        f32 a = (low - from) / d;
        f32 b = (high - from) / d;
        if (a > b) {
            std::swap(a, b);
        }
        t0 = std::max(t0, a);
        t1 = std::min(t1, b);
        return t0 < t1;
    };
    return clip(p.x, q.x - p.x, x0 + e, x0 + side - e) && clip(p.z, q.z - p.z, z0 + e, z0 + side - e);
}

void test_against_shortest_paths() {
    // 3 m square pillars in a 36 m square, checked against the shortest paths
    // a visibility graph of the pillars' corners gives.
    const auto pillar = [](int x, int z) { return x % 12 >= 4 && x % 12 < 7 && z % 12 >= 4 && z % 12 < 7; };
    const kin::NavMesh3D mesh = build(grid(36, 36, 1.0f, 0.0f, pillar));
    std::vector<Vec3f> squares;
    std::vector<Vec3f> corners;
    for (int z = 4; z < 36; z += 12) {
        for (int x = 4; x < 36; x += 12) {
            squares.push_back({static_cast<f32>(x), 0.0f, static_cast<f32>(z)});
            for (const int cz : {z, z + 3}) {
                for (const int cx : {x, x + 3}) {
                    corners.push_back({static_cast<f32>(cx), 0.0f, static_cast<f32>(cz)});
                }
            }
        }
    }
    const auto visible = [&](Vec3f a, Vec3f b) {
        for (const Vec3f s : squares) {
            if (crosses_square(a, b, s.x, s.z, 3.0f)) {
                return false;
            }
        }
        return true;
    };
    kin::RngKey key = kin::make_key(11);
    f32 excess = 0.0f;
    for (int i = 0; i < 100; ++i) {
        const auto [a_key, rest] = kin::split(key);
        const auto [b_key, next] = kin::split(rest);
        key = next;
        const Vec3f from = kin::random_point(mesh, a_key)->position;
        const Vec3f to = kin::random_point(mesh, b_key)->position;
        const std::vector<Vec3f> path = kin::find_path(mesh, from, to);
        assert(path.size() >= 2 && near(path.front(), from) && near(path.back(), to));
        for (std::size_t k = 1; k < path.size(); ++k) {
            assert(visible(path[k - 1], path[k]));
        }
        // Dijkstra over the two ends and the corners.
        std::vector<Vec3f> points{from, to};
        points.insert(points.end(), corners.begin(), corners.end());
        std::vector<f32> dist(points.size(), 1e30f);
        std::vector<bool> done(points.size(), false);
        dist[0] = 0.0f;
        while (!done[1]) {
            std::size_t u = 0;
            while (done[u]) {
                ++u;
            }
            for (std::size_t w = u + 1; w < points.size(); ++w) {
                if (!done[w] && dist[w] < dist[u]) {
                    u = w;
                }
            }
            done[u] = true;
            for (std::size_t w = 0; w < points.size(); ++w) {
                if (!done[w] && visible(points[u], points[w])) {
                    dist[w] = std::min(dist[w], dist[u] + kin::length(points[w] - points[u]));
                }
            }
        }
        const f32 length = path_length(path);
        assert(length >= dist[1] - 1e-3f && length <= dist[1] * 1.02f);
        excess += length / dist[1] - 1.0f;
    }
    assert(excess / 100.0f < 0.002f); // under 0.2 % longer on average
}

#ifdef KIN_EXPERIMENTAL_3D
void test_mesh_data() {
    const kin::MeshData3D plane = kin::meshes::plane({10.0f, 10.0f}, 10);
    const kin::NavMesh3D mesh = kin::build_navmesh3d(plane, kin::Mat4f::translation({5.0f, 1.0f, 5.0f})).value_or({});
    assert(mesh.triangle_count() == 200 && mesh.region_count() == 1);
    assert(near(mesh.bounds_min, {0.0f, 1.0f, 0.0f}) && near(mesh.bounds_max, {10.0f, 1.0f, 10.0f}));
    // Mirrored, it still faces up.
    const kin::NavMesh3D mirrored = kin::build_navmesh3d(plane, kin::Mat4f::scaling({-1.0f, 1.0f, 1.0f})).value_or({});
    assert(mirrored.triangle_count() == 200 && mirrored.region_count() == 1);
    assert(kin::find_path(mirrored, {-4.5f, 0.0f, -4.5f}, {4.5f, 0.0f, 4.5f}).size() == 2);
}
#endif

void test_cost() {
    // 200 x 200 one-metre quads with a 4 x 4 pillar every 20 m.
    const auto pillars = [](int x, int z) { return x % 20 >= 8 && x % 20 < 12 && z % 20 >= 8 && z % 20 < 12; };
    const Soup soup = grid(200, 200, 1.0f, 0.0f, pillars);
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    const kin::NavMesh3D mesh = build(soup);
    const auto t1 = clock::now();
    assert(mesh.triangle_count() == 2 * (200 * 200 - 100 * 16) && mesh.region_count() == 1);

    std::vector<Vec3f> ends;
    kin::RngKey key = kin::make_key(42);
    for (int i = 0; i < 200; ++i) {
        const auto [use, next] = kin::split(key);
        key = next;
        ends.push_back(kin::random_point(mesh, use)->position);
    }
    std::size_t corners = 0;
    std::vector<std::vector<Vec3f>> paths;
    const auto t2 = clock::now();
    for (int i = 0; i < 100; ++i) {
        paths.push_back(kin::find_path(mesh, ends[2 * i], ends[2 * i + 1]));
        corners += paths.back().size();
    }
    const auto t3 = clock::now();
    for (int i = 0; i < 100; ++i) {
        const std::vector<Vec3f>& path = paths[static_cast<std::size_t>(i)];
        assert(path.size() >= 2);
        assert(near(path.front(), ends[2 * i]) && near(path.back(), ends[2 * i + 1]));
        // Pillars are small and far apart: never much longer than the straight line.
        const f32 straight = kin::length(ends[2 * i + 1] - ends[2 * i]);
        assert(path_length(path) >= straight - 1e-3f && path_length(path) <= straight * 1.02f + 8.0f);
    }
    assert(bytes_of(build(soup)) == bytes_of(mesh));
    assert(kin::find_path(mesh, ends[0], ends[1]) == paths[0]);

    const auto ms = [](clock::time_point a, clock::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    std::printf("navmesh3d: 200x200 grid, %zu triangles, built in %.1f ms (%.1f MiB); 100 paths in %.1f ms "
                "(%.0f us each, %.1f corners on average)\n",
                mesh.triangle_count(), ms(t0, t1), static_cast<double>(mesh.memory_bytes()) / (1024.0 * 1024.0),
                ms(t2, t3), ms(t2, t3) * 10.0, static_cast<double>(corners) / 100.0);
}

Soup stair_fixture(bool nose = false, f32 width = 2) {
    Soup soup = grid(20, int(std::round(width / .1f)), .1f, 0, {-2, 0, 0});
    for (int k = 0; k < 4; ++k) {
        const float cell = k % 2 ? .1f : .25f;
        soup.append(
            grid(int(std::round(1 / cell)), int(std::round(width / cell)), cell, (k + 1) * .2f, {float(k), 0, 0}));
        if (nose)
            soup.append({{{k - .03f, (k + 1) * .2f, 0},
                          {k - .03f, (k + 1) * .2f, width},
                          {float(k), (k + 1) * .2f, width},
                          {float(k), (k + 1) * .2f, 0}},
                         {0, 1, 2, 0, 2, 3}});
    }
    soup.append(grid(20, int(std::round(width / .1f)), .1f, .8f, {4, 0, 0}));
    return soup;
}
void test_step_seams_and_soffits() {
    // A nose extends a drop-off toward the lower tread. Radius erosion must
    // reach across the joined riser as well as along each individual tread.
    auto drop = grid(20, 40, .1f, 0, {-2, 0, 0});
    drop.append(grid(20, 20, .1f, .2f, {0, 0, 1}));
    drop.append({{{-.03f, .2f, 1}, {-.03f, .2f, 3}, {0, .2f, 3}, {0, .2f, 1}}, {0, 1, 2, 0, 2, 3}});
    const auto clearance = build(drop, {.agent_radius = .3f, .agent_height = 1.8f, .max_step_height = .25f});
    assert(!kin::nearest(clearance, {-.35f, 0, 1.05f}, {.002f, .002f, .002f}));
    assert(kin::nearest(clearance, {-.6f, 0, 1.05f}, {.002f, .002f, .002f}));
    // Crossings should follow the rerouted corridor, not aim every riser at
    // the final destination. This unobstructed route has a 6.025 m chord.
    const auto wide =
        build(stair_fixture(false, 4), {.agent_radius = .3f, .agent_height = 1.8f, .max_step_height = .25f});
    const auto across = kin::find_path(wide, {-.865620136f, 0, 2.50231123f}, {5.0229063f, .8f, 1.22923446f});
    assert(!across.empty());
    float horizontal = 0;
    for (std::size_t i = 1; i < across.size(); ++i) {
        const auto d = across[i] - across[i - 1];
        horizontal += std::hypot(d.x, d.z);
    }
    if (!(horizontal < 6.08f))
        std::fprintf(stderr, "stair route horizontal=%g\n", horizontal);
    assert(horizontal < 6.08f);

    for (bool nose : {false, true}) {
        for (float angle : {0.f, 5.f, 30.f, 45.f}) {
            auto soup = stair_fixture(nose);
            const auto turn = kin::Quatf::axis_angle({0, 1, 0}, angle);
            for (auto& point : soup.vertices)
                point = turn.rotate(point);
            for (float radius : {0.f, .1f, .3f, .4f}) {
                const auto mesh = build(soup, {.agent_radius = radius, .agent_height = 1.8f, .max_step_height = .25f});
                assert(mesh.region_count() == 1);
                const auto path = kin::find_path(mesh, turn.rotate({-1, 0, 1}), turn.rotate({5, .8f, 1}));
                assert(!path.empty());
                if (radius == 0) {
                    const auto inverse = kin::Quatf::axis_angle({0, 1, 0}, -angle);
                    for (const auto& tri : mesh.triangles)
                        for (u32 edge = 0; edge < 3; ++edge) {
                            const auto a = inverse.rotate(mesh.vertices[tri.vertices[edge]]);
                            const auto b = inverse.rotate(mesh.vertices[tri.vertices[(edge + 1) % 3]]);
                            const auto middle = (a + b) * .5f;
                            const bool seam = std::abs(middle.x - std::round(middle.x)) < 1e-4f ||
                                              (nose && std::abs(middle.x + .03f - std::round(middle.x + .03f)) < 1e-4f);
                            if (seam && middle.x > -.04f && middle.x < 4.01f && middle.z > .01f && middle.z < 1.99f &&
                                std::abs(a.x - b.x) < 1e-4f && std::abs(a.y - b.y) < 1e-4f)
                                assert(tri.neighbors[edge] != kin::nav_none);
                        }
                }
                for (const auto& tri : mesh.triangles)
                    for (u32 e = 0; e < 3; ++e)
                        if (tri.neighbors[e] != kin::nav_none) {
                            const auto& partner = mesh.triangles[tri.neighbors[e]];
                            assert(std::ranges::find(partner.neighbors, u32(&tri - mesh.triangles.data())) !=
                                   partner.neighbors.end());
                        }
            }
        }
    }
    // A single large roof triangle can extend below the current floor at
    // its far corner; classify its clipped overhead, not its lowest vertex.
    auto long_roof = stair_fixture();
    long_roof.append({{{-10, .05f, -1}, {-10, .05f, 3}, {20, 6.05f, 3}, {20, 6.05f, -1}}, {0, 2, 1, 0, 3, 2}});
    for (float radius : {.3f, .4f})
        assert(kin::find_path(build(long_roof, {.agent_radius = radius, .agent_height = 1.8f, .max_step_height = .25f}),
                              {-.5f, 0, 1}, {5, .8f, 1})
                   .empty());
    assert(!kin::find_path(build(long_roof, {.agent_radius = .1f, .agent_height = 1.8f, .max_step_height = .25f}),
                           {-.5f, 0, 1}, {5, .8f, 1})
                .empty());
    auto stairs = stair_fixture();
    auto roof = grid(16, 8, .5f, 0, {-2, 0, -1});
    for (auto& p : roof.vertices)
        p.y = 2.05f + .2f * std::clamp(p.x, -1.f, 4.f);
    for (std::size_t i = 0; i < roof.indices.size(); i += 3)
        std::swap(roof.indices[i + 1], roof.indices[i + 2]);
    stairs.append(roof);
    for (float radius : {0.f, .1f})
        assert(!kin::find_path(build(stairs, {.agent_radius = radius, .agent_height = 1.8f, .max_step_height = .25f}),
                               {-1, 0, 1}, {5, .8f, 1})
                    .empty());
    // The cylinder reaches the next tread before its centre crosses the riser.
    // Clearance measured only vertically over the centre incorrectly accepts it.
    for (float radius : {.3f, .4f})
        assert(kin::find_path(build(stairs, {.agent_radius = radius, .agent_height = 1.8f, .max_step_height = .25f}),
                              {-1, 0, 1}, {5, .8f, 1})
                   .empty());
    assert(!kin::find_path(build(stair_fixture(), {.agent_radius = .4f, .agent_height = 1.8f, .max_step_height = .25f}),
                           {-1, 0, 1}, {5, .8f, 1})
                .empty());
}

} // namespace

int main() {
    test_step_seams_and_soffits();
    test_grid_with_hole_adjacency();
    test_path_around_the_hole();
    test_slopes();
    test_islands();
    test_weld();
    test_nearest();
    test_raycast();
    test_up_axis();
    test_invalid_input();
    test_agent_clearance();
    test_random_point();
    test_deterministic();
    test_against_shortest_paths();
#ifdef KIN_EXPERIMENTAL_3D
    test_mesh_data();
#endif
    test_cost();
    std::printf("navmesh3d_tests: all passed\n");
    return 0;
}
