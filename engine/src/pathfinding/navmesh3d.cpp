#include <kin/pathfinding/navmesh3d.hpp>
#include <kin/platform/log.hpp>

#ifdef KIN_EXPERIMENTAL_3D
#include <kin/render3d/mesh3d.hpp>
#endif

#include <algorithm>
#include <cmath>
#include <compare>
#include <cstring>
#include <limits>
#include <numeric>
#include <queue>
#include <unordered_map>
#include <utility>

namespace kin {
namespace {

constexpr f32 inf = std::numeric_limits<f32>::infinity();
// The most triangles a mesh can have: the path search numbers three nodes a
// triangle and one more for the goal, all below nav_none.
constexpr std::size_t max_triangles = (static_cast<std::size_t>(nav_none) - 1) / 3;
constexpr u32 leaf_size = 4;

bool finite(Vec3f v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

Vec3f min3(Vec3f a, Vec3f b) {
    return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
}

Vec3f max3(Vec3f a, Vec3f b) {
    return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
}

f32 axis_of(Vec3f v, int axis) {
    return axis == 0 ? v.x : (axis == 1 ? v.y : v.z);
}

// Twice the area of a, b, c seen from above: positive when c is to the left
// of a -> b (counter-clockwise), negative to the right, 0 in line.
f32 orient(Vec3f a, Vec3f b, Vec3f c, Vec3f up) {
    return dot(cross(b - a, c - a), up);
}

// `v` seen from above: its part square to `up`.
Vec3f flat(Vec3f v, Vec3f up) {
    return v - up * dot(v, up);
}

// --- Building -----------------------------------------------------------------

// A weld cell: the position divided by the weld distance, or, when that is 0,
// the position's bits (equal positions, and only they, share one).
struct WeldKey {
    i64 x = 0;
    i64 y = 0;
    i64 z = 0;

    friend constexpr auto operator<=>(const WeldKey&, const WeldKey&) = default;
};

i64 weld_cell(f32 v, f64 inverse_cell) {
    // Clamped so that the neighbouring cells (one either side) never overflow.
    constexpr f64 limit = 4.0e18;
    return static_cast<i64>(std::clamp(std::floor(static_cast<f64>(v) * inverse_cell), -limit, limit));
}

i64 weld_bits(f32 v) {
    const f32 positive_zero = v + 0.0f; // -0 and +0 are one position
    u32 bits = 0;
    std::memcpy(&bits, &positive_zero, sizeof(bits));
    return bits;
}

WeldKey weld_key(Vec3f p, f32 distance) {
    if (distance > 0.0f) {
        const f64 inverse = 1.0 / static_cast<f64>(distance);
        return {weld_cell(p.x, inverse), weld_cell(p.y, inverse), weld_cell(p.z, inverse)};
    }
    return {weld_bits(p.x), weld_bits(p.y), weld_bits(p.z)};
}

// For each vertex, the vertex it is welded to: the lowest-numbered earlier
// vertex that is itself kept and lies within `distance`, or itself. Cells as
// wide as the distance put every candidate in the 27 cells around a vertex.
// A sorted array rather than a hash map, so that nothing depends on hashing.
std::vector<u32> weld(std::span<const Vec3f> positions, f32 distance) {
    struct Entry {
        WeldKey key;
        u32 index = 0;
    };
    const auto count = static_cast<u32>(positions.size());
    std::vector<Entry> entries(count);
    for (u32 i = 0; i < count; ++i) {
        entries[i] = {weld_key(positions[i], distance), i};
    }
    std::ranges::sort(entries, [](const Entry& a, const Entry& b) {
        if (a.key != b.key) {
            return a.key < b.key;
        }
        return a.index < b.index;
    });

    std::vector<u32> welded(count);
    const f32 distance_squared = distance * distance;
    const i64 reach = distance > 0.0f ? 1 : 0;
    for (u32 i = 0; i < count; ++i) {
        const WeldKey key = weld_key(positions[i], distance);
        u32 best = i;
        for (i64 dx = -reach; dx <= reach; ++dx) {
            for (i64 dy = -reach; dy <= reach; ++dy) {
                // The cells z - reach to z + reach of one column are adjacent in the sorted order.
                const WeldKey low{key.x + dx, key.y + dy, key.z - reach};
                const WeldKey high{key.x + dx, key.y + dy, key.z + reach};
                auto it = std::ranges::lower_bound(entries, low, {}, &Entry::key);
                for (; it != entries.end() && it->key <= high; ++it) {
                    const u32 j = it->index;
                    if (j < best && welded[j] == j && length_squared(positions[j] - positions[i]) <= distance_squared) {
                        best = j;
                    }
                }
            }
        }
        welded[i] = best;
    }
    return welded;
}

// For each vertex, the triangles that use it, in triangle order.
struct Incidence {
    std::vector<u32> start;
    std::vector<u32> triangles;

    std::span<const u32> of(u32 vertex) const {
        return {triangles.data() + start[vertex], triangles.data() + start[vertex + 1]};
    }
};

Incidence incidence(std::span<const std::array<u32, 3>> triangles, std::size_t vertex_count) {
    Incidence result;
    result.start.assign(vertex_count + 1, 0);
    for (const auto& t : triangles) {
        for (const u32 v : t) {
            ++result.start[v + 1];
        }
    }
    for (std::size_t v = 0; v < vertex_count; ++v) {
        result.start[v + 1] += result.start[v];
    }
    result.triangles.resize(triangles.size() * 3);
    std::vector<u32> fill(result.start.begin(), result.start.end() - 1);
    for (u32 t = 0; t < static_cast<u32>(triangles.size()); ++t) {
        for (const u32 v : triangles[t]) {
            result.triangles[fill[v]++] = t;
        }
    }
    return result;
}

std::array<u32, 3> sorted(std::array<u32, 3> t) {
    std::ranges::sort(t);
    return t;
}

std::string check_input(std::span<const Vec3f> vertices, std::span<const u32> indices,
                        const NavMeshBuildOptions3D& options) {
    if (!finite(options.up) || !(length_squared(options.up) > 0.0f) || !std::isfinite(length_squared(options.up))) {
        return "up must be finite and not zero";
    }
    if (!(options.max_slope_degrees >= 0.0f && options.max_slope_degrees <= 90.0f)) {
        return "max_slope_degrees must be 0 to 90";
    }
    if (!(options.weld_distance >= 0.0f) || !std::isfinite(options.weld_distance)) {
        return "weld_distance must be finite and not negative";
    }
    if (!(options.agent_radius >= 0.0f) || !std::isfinite(options.agent_radius) || !(options.agent_height >= 0.0f) ||
        !std::isfinite(options.agent_height)) {
        return "agent_radius and agent_height must be finite and not negative";
    }
    if (vertices.size() >= nav_none) {
        return "too many vertices";
    }
    const std::size_t corners = indices.empty() ? vertices.size() : indices.size();
    if (corners % 3 != 0) {
        return indices.empty() ? "an unindexed soup needs three vertices a triangle"
                               : "indices must come three a triangle";
    }
    if (corners / 3 > max_triangles) {
        return "too many triangles";
    }
    for (const u32 index : indices) {
        if (index >= vertices.size()) {
            return "index " + std::to_string(index) + " is out of range (" + std::to_string(vertices.size()) +
                   " vertices)";
        }
    }
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        if (!finite(vertices[i])) {
            return "vertex " + std::to_string(i) + " is not finite";
        }
    }
    return {};
}

NavMesh3D rejected(const std::string& why, std::string* error) {
    if (error) {
        *error = why;
    } else {
        KIN_LOG_WARN_F("pathfinding", "navigation mesh not built", (LogFields{{.name = "reason", .value = why}}));
    }
    return {};
}

// A bounding volume hierarchy over the triangles: median splits along the
// longest axis of the centroids. The comparison is a total order (centroid,
// then index), so every split, and so the tree, is the same with every
// standard library; each leaf is in index order.
void build_tree(NavMesh3D& mesh) {
    const auto count = static_cast<u32>(mesh.triangles.size());
    std::vector<Vec3f> low(count);
    std::vector<Vec3f> high(count);
    for (u32 t = 0; t < count; ++t) {
        const auto& v = mesh.triangles[t].vertices;
        low[t] = min3(min3(mesh.vertices[v[0]], mesh.vertices[v[1]]), mesh.vertices[v[2]]);
        high[t] = max3(max3(mesh.vertices[v[0]], mesh.vertices[v[1]]), mesh.vertices[v[2]]);
    }
    std::vector<u32> order(count);
    std::iota(order.begin(), order.end(), 0u);
    mesh.nodes.clear();
    mesh.nodes.reserve(2 * (count / 2 + 1));
    mesh.nodes.push_back({});

    struct Task {
        u32 node = 0;
        u32 begin = 0;
        u32 end = 0;
    };
    std::vector<Task> tasks{{0, 0, count}};
    while (!tasks.empty()) {
        const Task task = tasks.back();
        tasks.pop_back();
        Vec3f box_low = low[order[task.begin]];
        Vec3f box_high = high[order[task.begin]];
        Vec3f centre_low = mesh.triangles[order[task.begin]].centroid;
        Vec3f centre_high = centre_low;
        for (u32 i = task.begin + 1; i < task.end; ++i) {
            box_low = min3(box_low, low[order[i]]);
            box_high = max3(box_high, high[order[i]]);
            centre_low = min3(centre_low, mesh.triangles[order[i]].centroid);
            centre_high = max3(centre_high, mesh.triangles[order[i]].centroid);
        }
        mesh.nodes[task.node].min = box_low;
        mesh.nodes[task.node].max = box_high;
        if (task.end - task.begin <= leaf_size) {
            std::sort(order.begin() + task.begin, order.begin() + task.end);
            mesh.nodes[task.node].first = task.begin;
            mesh.nodes[task.node].count = task.end - task.begin;
            continue;
        }
        const Vec3f extent = centre_high - centre_low;
        const int axis = extent.x >= extent.y && extent.x >= extent.z ? 0 : (extent.y >= extent.z ? 1 : 2);
        const u32 middle = task.begin + (task.end - task.begin) / 2;
        std::nth_element(order.begin() + task.begin, order.begin() + middle, order.begin() + task.end,
                         [&](u32 a, u32 b) {
                             const f32 ca = axis_of(mesh.triangles[a].centroid, axis);
                             const f32 cb = axis_of(mesh.triangles[b].centroid, axis);
                             if (ca != cb) {
                                 return ca < cb;
                             }
                             return a < b;
                         });
        const auto children = static_cast<u32>(mesh.nodes.size());
        mesh.nodes.push_back({});
        mesh.nodes.push_back({});
        mesh.nodes[task.node].first = children;
        mesh.nodes[task.node].count = 0;
        tasks.push_back({children + 1, middle, task.end});
        tasks.push_back({children, task.begin, middle});
    }
    mesh.nodes.shrink_to_fit();
    mesh.node_triangles = std::move(order);
}

// Clearance deliberately discards whole triangles. It can remove extra floor on
// a coarse walk surface, but never advertises just the safe centre of a triangle
// while nearest(), random_point() or the funnel can still reach its unsafe edge.
template <class Predicate>
bool any_overlap(const NavMesh3D& tree, Vec3f low, Vec3f high, std::vector<u32>& stack, Predicate predicate) {
    if (tree.nodes.empty())
        return false;
    stack.clear();
    stack.push_back(0);
    while (!stack.empty()) {
        const auto& node = tree.nodes[stack.back()];
        stack.pop_back();
        if (node.max.x < low.x || node.min.x > high.x || node.max.y < low.y || node.min.y > high.y ||
            node.max.z < low.z || node.min.z > high.z)
            continue;
        if (node.count) {
            for (u32 i = 0; i < node.count; ++i)
                if (predicate(tree.node_triangles[node.first + i]))
                    return true;
        } else {
            stack.push_back(node.first + 1);
            stack.push_back(node.first);
        }
    }
    return false;
}

f64 dot64(Vec3f a, Vec3f b) {
    return f64(a.x) * b.x + f64(a.y) * b.y + f64(a.z) * b.z;
}

// Clip an obstacle against the vertical prism over a floor triangle. This tests
// the entire footprint, not a few upward samples (a small/slanted ceiling can
// fall between those). Obstacle winding does not matter.
bool blocks_headroom(const std::array<Vec3f, 3>& floor, const std::array<Vec3f, 3>& obstacle, Vec3f up, f32 height,
                     const std::array<u32, 3>& neighbors) {
    // Relative coordinates avoid cancellation of large world-space plane offsets.
    std::vector<Vec3f> polygon, clipped;
    for (const auto point : obstacle)
        polygon.push_back(point - floor[0]);
    clipped.reserve(10);
    const auto clip = [&](Vec3f normal, f64 offset) {
        clipped.clear();
        if (polygon.empty())
            return;
        auto previous = polygon.back();
        auto before = dot64(normal, previous) + offset;
        for (const auto point : polygon) {
            const auto after = dot64(normal, point) + offset;
            if ((before >= 0) != (after >= 0))
                clipped.push_back(previous + (point - previous) * f32(before / (before - after)));
            if (after >= 0)
                clipped.push_back(point);
            previous = point;
            before = after;
        }
        polygon.swap(clipped);
    };
    for (u32 edge = 0; edge < 3 && !polygon.empty(); ++edge) {
        const auto inward = normalize(cross(up, floor[(edge + 1) % 3] - floor[edge]));
        // Boundary-only contact (a stair riser or an adjacent uphill face)
        // does not occupy the floor interior. Keep a world-space tolerance.
        // Across an interior floor portal, a wall still blocks crossing.
        // Exposed tread edges have no portal to obstruct.
        clip(inward, -dot64(inward, floor[edge] - floor[0]) - (neighbors[edge] == nav_none ? 1e-5 : 0));
    }
    const auto normal = normalize(cross(floor[1] - floor[0], floor[2] - floor[0]));
    const auto vertical = dot64(normal, up);
    // Ignore the supporting surface itself. A 10 micrometre tolerance is in
    // world units; the upper bound is conservative about touching the head.
    clip(normal, -1e-5 * vertical);
    clip(-normal, (height + 1e-5) * vertical);
    return !polygon.empty();
}

f64 segment_distance_squared(Vec3f a, Vec3f b, Vec3f c, Vec3f d) {
    const auto u = b - a, v = d - c, w = a - c;
    const f64 aa = dot64(u, u), bb = dot64(u, v), cc = dot64(v, v), dd = dot64(u, w), ee = dot64(v, w);
    f64 s = 0, t = 0;
    if (aa > 0) {
        const f64 denominator = aa * cc - bb * bb;
        if (denominator > 0)
            s = std::clamp((bb * ee - cc * dd) / denominator, 0.0, 1.0);
    }
    if (cc > 0) {
        t = (bb * s + ee) / cc;
        if (t < 0) {
            t = 0;
            if (aa > 0)
                s = std::clamp(-dd / aa, 0.0, 1.0);
        } else if (t > 1) {
            t = 1;
            if (aa > 0)
                s = std::clamp((bb - dd) / aa, 0.0, 1.0);
        }
    } else if (aa > 0)
        s = std::clamp(-dd / aa, 0.0, 1.0);
    const auto delta = w + u * f32(s) - v * f32(t);
    return dot64(delta, delta);
}

f64 boundary_distance_squared(const std::array<Vec3f, 3>& triangle, Vec3f a, Vec3f b, Vec3f up) {
    // A border wholly inside a triangle need not cross any of its edges.
    for (const auto point : {a, b})
        if (orient(triangle[0], triangle[1], point, up) >= 0 && orient(triangle[1], triangle[2], point, up) >= 0 &&
            orient(triangle[2], triangle[0], point, up) >= 0)
            return 0;
    f64 distance = std::numeric_limits<f64>::infinity();
    for (u32 i = 0; i < 3; ++i)
        distance = std::min(distance, segment_distance_squared(triangle[i], triangle[(i + 1) % 3], a, b));
    return distance;
}

std::vector<u32> clear_triangles(const NavMesh3D& mesh, std::span<const Vec3f> vertices, std::span<const u32> indices) {
    const auto up = mesh.options.up;
    std::vector<bool> keep(mesh.triangles.size(), true);
    std::vector<u32> stack;
    stack.reserve(64);
    if (mesh.options.agent_height > 0) {
        // An uphill centre needs extra headroom above the downhill side of its
        // footprint. Bound that rise by the steepest actual slope per region.
        std::vector<f32> rise_margin(mesh.regions, 0);
        if (mesh.options.agent_radius > 0)
            for (const auto& triangle : mesh.triangles) {
                const auto& v = triangle.vertices;
                const auto normal = normalize(
                    cross(mesh.vertices[v[1]] - mesh.vertices[v[0]], mesh.vertices[v[2]] - mesh.vertices[v[0]]));
                const auto vertical = dot64(normal, up);
                const f64 slope = std::sqrt(std::max(0.0, dot64(normal, normal) - vertical * vertical)) / vertical;
                rise_margin[triangle.region] =
                    std::max(rise_margin[triangle.region], f32(slope * mesh.options.agent_radius));
            }
        NavMesh3D obstacles;
        const auto welded = weld(vertices, mesh.options.weld_distance);
        obstacles.vertices.reserve(vertices.size());
        for (const auto v : welded)
            obstacles.vertices.push_back(vertices[v]);
        const auto count = (indices.empty() ? vertices.size() : indices.size()) / 3;
        for (std::size_t t = 0; t < count; ++t) {
            NavTriangle3D triangle;
            for (u32 i = 0; i < 3; ++i)
                triangle.vertices[i] = indices.empty() ? u32(t * 3 + i) : indices[t * 3 + i];
            const auto a = obstacles.vertices[triangle.vertices[0]], b = obstacles.vertices[triangle.vertices[1]],
                       c = obstacles.vertices[triangle.vertices[2]];
            if (!(length_squared(cross(b - a, c - a)) > 0))
                continue;
            triangle.centroid = (a + b + c) / 3.f;
            obstacles.triangles.push_back(triangle);
        }
        if (!obstacles.triangles.empty())
            build_tree(obstacles);
        for (u32 t = 0; t < mesh.triangles.size(); ++t) {
            const auto& ids = mesh.triangles[t].vertices;
            const std::array<Vec3f, 3> floor{mesh.vertices[ids[0]], mesh.vertices[ids[1]], mesh.vertices[ids[2]]};
            auto low = min3(floor[0], min3(floor[1], floor[2]));
            auto high = max3(floor[0], max3(floor[1], floor[2]));
            const auto height = mesh.options.agent_height + rise_margin[mesh.triangles[t].region];
            const auto rise = up * (height + 1e-5f);
            low = min3(low, low + rise);
            high = max3(high, high + rise);
            keep[t] = !any_overlap(obstacles, low, high, stack, [&](u32 other) {
                const auto& v = obstacles.triangles[other].vertices;
                return blocks_headroom(floor,
                                       {obstacles.vertices[v[0]], obstacles.vertices[v[1]], obstacles.vertices[v[2]]},
                                       up, height, mesh.triangles[t].neighbors);
            });
        }
    }
    if (mesh.options.agent_radius > 0) {
        // Follow the local floor topology, rather than every border in a region:
        // a ramp can connect vertically overlapping floors in the same region.
        // Only traverse triangles within radius of this footprint. A distant
        // connection cannot bring a balcony's projected border down to the ground.
        const auto walkable = keep;
        std::vector<Vec3f> projected;
        projected.reserve(mesh.vertices.size());
        for (const auto vertex : mesh.vertices)
            projected.push_back(flat(vertex, up));
        std::vector<u32> visited(mesh.triangles.size(), nav_none);
        const f64 radius_squared = f64(mesh.options.agent_radius) * mesh.options.agent_radius;
        for (u32 t = 0; t < mesh.triangles.size(); ++t) {
            if (!walkable[t])
                continue;
            const auto& ids = mesh.triangles[t].vertices;
            const std::array<Vec3f, 3> floor{projected[ids[0]], projected[ids[1]], projected[ids[2]]};
            stack.clear();
            stack.push_back(t);
            visited[t] = t;
            while (!stack.empty() && keep[t]) {
                const auto current = stack.back();
                stack.pop_back();
                const auto& triangle = mesh.triangles[current];
                for (u32 i = 0; i < 3; ++i) {
                    const auto a = projected[triangle.vertices[i]], b = projected[triangle.vertices[(i + 1) % 3]];
                    if (boundary_distance_squared(floor, a, b, up) > radius_squared)
                        continue;
                    const auto next = triangle.neighbors[i];
                    if (next == nav_none || !walkable[next]) {
                        keep[t] = false;
                        break;
                    }
                    if (visited[next] != t) {
                        visited[next] = t;
                        stack.push_back(next);
                    }
                }
            }
        }
    }
    std::vector<u32> result;
    for (u32 t = 0; t < mesh.triangles.size(); ++t)
        if (keep[t])
            result.insert(result.end(), mesh.triangles[t].vertices.begin(), mesh.triangles[t].vertices.end());
    return result;
}

// --- Queries ------------------------------------------------------------------

// The point of triangle a, b, c closest to p (Ericson, Real-Time Collision
// Detection, 5.1.5).
Vec3f closest_on_triangle(Vec3f p, Vec3f a, Vec3f b, Vec3f c) {
    const Vec3f ab = b - a;
    const Vec3f ac = c - a;
    const Vec3f ap = p - a;
    const f32 d1 = dot(ab, ap);
    const f32 d2 = dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) {
        return a;
    }
    const Vec3f bp = p - b;
    const f32 d3 = dot(ab, bp);
    const f32 d4 = dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) {
        return b;
    }
    const f32 vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        return a + ab * (d1 / (d1 - d3));
    }
    const Vec3f cp = p - c;
    const f32 d5 = dot(ab, cp);
    const f32 d6 = dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) {
        return c;
    }
    const f32 vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        return a + ac * (d2 / (d2 - d6));
    }
    const f32 va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && d4 - d3 >= 0.0f && d5 - d6 >= 0.0f) {
        return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    }
    const f32 denominator = 1.0f / (va + vb + vc);
    return a + ab * (vb * denominator) + ac * (vc * denominator);
}

// The squared distance from p to a box (0 inside it).
f32 box_distance_squared(Vec3f p, Vec3f low, Vec3f high) {
    const Vec3f d = max3(max3(low - p, p - high), {});
    return length_squared(d);
}

Vec3f corner(const NavMesh3D& mesh, u32 triangle, u32 i) {
    return mesh.vertices[mesh.triangles[triangle].vertices[i % 3]];
}

// The point of segment a-b with the shortest way from p through it to the
// target, seen from above, straight on either side: where the line from p to
// the target crosses the segment's line, the target first mirrored across that
// line when both are on one side (the shortest way that touches a line),
// clamped to the segment.
Vec3f crossing(Vec3f p, Vec3f a, Vec3f b, Vec3f target, Vec3f up) {
    const Vec3f edge = flat(b - a, up);
    const f32 edge_squared = dot(edge, edge);
    if (!(edge_squared > 0.0f)) {
        return (a + b) * 0.5f;
    }
    // (t along the segment, s across it) are coordinates of the plane seen
    // from above in which the segment's line is s = 0.
    const f32 tp = dot(flat(p - a, up), edge) / edge_squared;
    const f32 tg = dot(flat(target - a, up), edge) / edge_squared;
    const f32 sp = orient(a, b, p, up);
    f32 sg = orient(a, b, target, up);
    if ((sp > 0.0f && sg > 0.0f) || (sp < 0.0f && sg < 0.0f)) {
        sg = -sg;
    }
    const f32 t = sp == sg ? tg : tp + (tg - tp) * (sp / (sp - sg));
    return a + (b - a) * std::clamp(t, 0.0f, 1.0f);
}

// A* over the edges between triangles, for the corridor a path runs through.
// A node is an edge crossed from one of its triangles (triangle * 3 + edge);
// the goal is one more node. A node's position is where the search crossed
// its edge (crossing(), toward the goal), its cost the length of the straight
// pieces from the start through those crossings: a real way through the
// corridor, close to its taut length. (Crossing at the edges' midpoints
// instead makes a regular grid's diagonals one way cost as much as going round
// two sides of a square, and the search then takes a corridor along a wall as
// readily as the straight one.) The open list's order is total (estimate, then
// the deeper node, then the node number), so equally good corridors are chosen
// the same way on every run. Returns the corridor's triangles, start to goal;
// empty when there is no way.
std::vector<u32> search_corridor(const NavMesh3D& mesh, const NavPoint3D& start, const NavPoint3D& goal) {
    struct Node {
        f32 cost = inf;
        u32 parent = nav_none;
        Vec3f position{};
    };
    struct Open {
        f32 estimate = 0.0f;
        f32 cost = 0.0f;
        u32 node = 0;
    };
    struct OpenGreater {
        bool operator()(const Open& a, const Open& b) const {
            if (a.estimate != b.estimate) {
                return a.estimate > b.estimate;
            }
            if (a.cost != b.cost) {
                return a.cost < b.cost;
            }
            return a.node > b.node;
        }
    };

    const Vec3f up = mesh.options.up;
    const Vec3f target = goal.position;
    const u32 goal_node = static_cast<u32>(mesh.triangles.size()) * 3;
    // Only the nodes reached are stored; lookups alone, so hashing never orders anything.
    std::unordered_map<u32, Node> nodes;
    nodes.reserve(1024);
    std::priority_queue<Open, std::vector<Open>, OpenGreater> open;
    const auto reach = [&](u32 node, u32 parent, Vec3f position, f32 cost) {
        Node& n = nodes.try_emplace(node).first->second;
        if (!(cost < n.cost)) {
            return;
        }
        n = {cost, parent, position};
        open.push({cost + length(target - position), cost, node});
    };

    for (u32 i = 0; i < 3; ++i) {
        if (mesh.triangles[start.triangle].neighbors[i] == nav_none) {
            continue;
        }
        const Vec3f at =
            crossing(start.position, corner(mesh, start.triangle, i), corner(mesh, start.triangle, i + 1), target, up);
        reach(start.triangle * 3 + i, nav_none, at, length(at - start.position));
    }

    while (!open.empty()) {
        const Open top = open.top();
        open.pop();
        const Node current = nodes[top.node];
        if (top.cost != current.cost) {
            continue; // superseded by a cheaper way to the same node
        }
        if (top.node == goal_node) {
            std::vector<u32> corridor{goal.triangle};
            for (u32 node = current.parent; node != nav_none; node = nodes[node].parent) {
                corridor.push_back(node / 3);
            }
            std::ranges::reverse(corridor);
            return corridor;
        }
        const u32 came_from = top.node / 3;
        const u32 triangle = mesh.triangles[came_from].neighbors[top.node % 3];
        if (triangle == goal.triangle) {
            // Never worth leaving the goal's triangle to come back: it is convex.
            reach(goal_node, top.node, target, current.cost + length(target - current.position));
            continue;
        }
        for (u32 j = 0; j < 3; ++j) {
            const u32 next = mesh.triangles[triangle].neighbors[j];
            if (next == nav_none || next == came_from) {
                continue;
            }
            const Vec3f at =
                crossing(current.position, corner(mesh, triangle, j), corner(mesh, triangle, j + 1), target, up);
            reach(triangle * 3 + j, top.node, at, current.cost + length(at - current.position));
        }
    }
    return {};
}

// An edge a corridor crosses, seen walking through it: leaving a triangle
// (counter-clockwise from above) through its edge from a to b, a is on the
// right and b on the left.
struct Portal {
    Vec3f left{};
    Vec3f right{};
    u32 left_vertex = nav_none;
    u32 right_vertex = nav_none;
};

// A point of a pulled string: the mesh vertex it is (nav_none for the ends)
// and the portal it was taken from.
struct Corner {
    Vec3f point{};
    u32 vertex = nav_none;
    std::size_t portal = 0;
};

std::vector<Portal> portals_of(const NavMesh3D& mesh, std::span<const u32> corridor, Vec3f from, Vec3f to) {
    std::vector<Portal> portals;
    portals.reserve(corridor.size() + 1);
    portals.push_back({from, from});
    for (std::size_t k = 1; k < corridor.size(); ++k) {
        const NavTriangle3D& t = mesh.triangles[corridor[k - 1]];
        const u32 e = t.neighbors[0] == corridor[k] ? 0u : (t.neighbors[1] == corridor[k] ? 1u : 2u);
        portals.push_back({
            .left = mesh.vertices[t.vertices[(e + 1) % 3]],
            .right = mesh.vertices[t.vertices[e]],
            .left_vertex = t.vertices[(e + 1) % 3],
            .right_vertex = t.vertices[e],
        });
    }
    portals.push_back({to, to});
    return portals;
}

// The simple stupid funnel algorithm (Mikko Mononen, 2010), seen from above:
// the taut string through the portals, from the first (a point) to the last.
std::vector<Corner> pull_string(std::span<const Portal> portals, Vec3f up) {
    std::vector<Corner> path;
    Corner apex{portals[0].left, nav_none, 0};
    Corner left = apex;
    Corner right = apex;
    path.push_back(apex);

    for (std::size_t i = 1; i < portals.size(); ++i) {
        const Corner l{portals[i].left, portals[i].left_vertex, i};
        const Corner r{portals[i].right, portals[i].right_vertex, i};

        // The right side moves in (or stays): tighten it, unless it crosses the left one.
        if (orient(apex.point, right.point, r.point, up) >= 0.0f) {
            if (apex.point == right.point || orient(apex.point, left.point, r.point, up) < 0.0f) {
                right = r;
            } else {
                // The left side is a corner: restart from it.
                apex = left;
                path.push_back(apex);
                right = apex;
                i = apex.portal;
                continue;
            }
        }

        // The same for the left side.
        if (orient(apex.point, left.point, l.point, up) <= 0.0f) {
            if (apex.point == left.point || orient(apex.point, right.point, l.point, up) > 0.0f) {
                left = l;
            } else {
                apex = right;
                path.push_back(apex);
                left = apex;
                i = apex.portal;
                continue;
            }
        }
    }
    path.push_back({portals.back().left, nav_none, portals.size() - 1});
    return path;
}

// Whether triangle t holds p, seen from above, to within rounding.
bool holds(const NavMesh3D& mesh, u32 t, Vec3f p) {
    const Vec3f up = mesh.options.up;
    for (u32 i = 0; i < 3; ++i) {
        const Vec3f a = corner(mesh, t, i);
        const Vec3f b = corner(mesh, t, i + 1);
        if (orient(a, b, p, up) < -1e-5f * length_squared(b - a)) {
            return false;
        }
    }
    return true;
}

// Cuts the corridor to begin at the last triangle that holds `from` and end at
// the first after it that holds `to`: an end on an edge or a vertex is in more
// than one triangle, and the funnel needs it off the portals it pulls through.
void trim(const NavMesh3D& mesh, std::vector<u32>& corridor, Vec3f from, Vec3f to) {
    std::size_t first = 0;
    for (std::size_t k = 1; k < corridor.size(); ++k) {
        if (holds(mesh, corridor[k], from)) {
            first = k;
        }
    }
    std::size_t last = corridor.size() - 1;
    for (std::size_t k = last; k > first; --k) {
        if (holds(mesh, corridor[k - 1], to)) {
            last = k - 1;
        }
    }
    corridor.erase(corridor.begin() + static_cast<std::ptrdiff_t>(last) + 1, corridor.end());
    corridor.erase(corridor.begin(), corridor.begin() + static_cast<std::ptrdiff_t>(first));
}

bool has_vertex(const NavMesh3D& mesh, u32 triangle, u32 v) {
    const auto& vs = mesh.triangles[triangle].vertices;
    return vs[0] == v || vs[1] == v || vs[2] == v;
}

// The neighbour of `triangle` across its edge at vertex v that does not lead
// to `not_to` (each triangle has two edges at each of its vertices).
u32 round_vertex(const NavMesh3D& mesh, u32 triangle, u32 v, u32 not_to) {
    const NavTriangle3D& t = mesh.triangles[triangle];
    const u32 k = t.vertices[0] == v ? 0u : (t.vertices[1] == v ? 1u : 2u);
    const u32 leaving = t.neighbors[k];
    const u32 arriving = t.neighbors[(k + 2) % 3];
    return leaving == not_to ? arriving : leaving;
}

// Whether walkable ground goes all the way round vertex v of `triangle`:
// then a shortest path never turns at it.
bool surrounded(const NavMesh3D& mesh, u32 triangle, u32 v) {
    u32 previous = nav_none;
    u32 at = triangle;
    for (std::size_t step = 0; step < mesh.triangles.size(); ++step) {
        const u32 next = round_vertex(mesh, at, v, previous);
        if (next == nav_none) {
            return false;
        }
        if (next == triangle) {
            return true;
        }
        previous = at;
        at = next;
    }
    return false;
}

// The string turned at a vertex walkable all the way round: the corridor went
// round it on one side. Sends it round the other side instead (the other way
// round the vertex's triangles), so that the string can pull straight past.
// False when it cannot.
bool reroute(const NavMesh3D& mesh, std::vector<u32>& corridor, const Corner& turn) {
    const u32 v = turn.vertex;
    // The run of the corridor's triangles round v, about the portal the turn came from.
    std::size_t i = turn.portal - 1;
    std::size_t j = turn.portal;
    while (i > 0 && has_vertex(mesh, corridor[i - 1], v)) {
        --i;
    }
    while (j + 1 < corridor.size() && has_vertex(mesh, corridor[j + 1], v)) {
        ++j;
    }
    std::vector<u32> other_way;
    if (corridor[i] != corridor[j]) {
        u32 previous = corridor[i + 1];
        u32 at = corridor[i];
        while (true) {
            const u32 next = round_vertex(mesh, at, v, previous);
            if (next == nav_none || other_way.size() > mesh.triangles.size()) {
                return false;
            }
            if (next == corridor[j]) {
                break;
            }
            other_way.push_back(next);
            previous = at;
            at = next;
        }
    }
    std::vector<u32> rerouted(corridor.begin(), corridor.begin() + static_cast<std::ptrdiff_t>(i) + 1);
    rerouted.insert(rerouted.end(), other_way.begin(), other_way.end());
    rerouted.insert(rerouted.end(), corridor.begin() + static_cast<std::ptrdiff_t>(j) + (corridor[i] == corridor[j]),
                    corridor.end());
    corridor = std::move(rerouted);
    return true;
}

f32 length_of(std::span<const Corner> path) {
    f32 total = 0.0f;
    for (std::size_t i = 1; i < path.size(); ++i) {
        total += length(path[i].point - path[i - 1].point);
    }
    return total;
}

// Walks the mesh in a straight line seen from above, from `origin` on
// `triangle` toward origin + delta, following the surface: where it first
// crosses a border, or nothing when it gets `reach` of the way (1: all of it).
std::optional<NavRayHit3D> walk(const NavMesh3D& mesh, u32 triangle, Vec3f origin, Vec3f delta, f32 reach) {
    const Vec3f up = mesh.options.up;
    f32 t = 0.0f;
    // A walk crosses each triangle at most once; more steps than triangles
    // means rounding has it circling a vertex, and it stops there.
    for (std::size_t step = 0; step <= mesh.triangles.size(); ++step) {
        // The edge the line leaves this triangle by, and when: along the walk,
        // the side of edge a -> b changes as n + t d (inside when not negative).
        f32 exit_t = inf;
        u32 exit_edge = nav_none;
        for (u32 i = 0; i < 3; ++i) {
            const Vec3f a = corner(mesh, triangle, i);
            const Vec3f b = corner(mesh, triangle, i + 1);
            const f32 d = dot(cross(b - a, delta), up);
            if (d < 0.0f) {
                const f32 when = -orient(a, b, origin, up) / d;
                if (when < exit_t) {
                    exit_t = when;
                    exit_edge = i;
                }
            }
        }
        if (exit_edge == nav_none || exit_t >= reach) {
            return std::nullopt;
        }
        t = std::max(t, exit_t);
        const u32 next = mesh.triangles[triangle].neighbors[exit_edge];
        if (next == nav_none) {
            const Vec3f a = corner(mesh, triangle, exit_edge);
            const Vec3f b = corner(mesh, triangle, exit_edge + 1);
            const Vec3f edge = flat(b - a, up);
            const f32 along = std::clamp(dot(flat(origin + delta * t - a, up), edge) / dot(edge, edge), 0.0f, 1.0f);
            return NavRayHit3D{
                .position = a + (b - a) * along,
                .fraction = t,
                .normal = normalize(cross(up, edge)),
                .triangle = triangle,
            };
        }
        triangle = next;
    }
    return NavRayHit3D{.position = origin + delta * t, .fraction = t, .triangle = triangle};
}

// Of the triangles round vertex v (from `triangle`, across edges), the one
// whose corner at v holds the direction `toward`, seen from above; nav_none
// when walkable ground does not go that way from v.
u32 triangle_toward(const NavMesh3D& mesh, u32 triangle, u32 v, Vec3f toward) {
    const Vec3f up = mesh.options.up;
    const Vec3f at = mesh.vertices[v];
    const auto holds_direction = [&](u32 t) {
        const auto& vs = mesh.triangles[t].vertices;
        const u32 k = vs[0] == v ? 0u : (vs[1] == v ? 1u : 2u);
        return orient(at, mesh.vertices[vs[(k + 1) % 3]], at + toward, up) >= 0.0f &&
               orient(at, mesh.vertices[vs[(k + 2) % 3]], at + toward, up) <= 0.0f;
    };
    if (holds_direction(triangle)) {
        return triangle;
    }
    for (const bool leaving : {true, false}) {
        u32 previous = nav_none;
        u32 t = triangle;
        for (std::size_t step = 0; step < mesh.triangles.size(); ++step) {
            const NavTriangle3D& tri = mesh.triangles[t];
            const u32 k = tri.vertices[0] == v ? 0u : (tri.vertices[1] == v ? 1u : 2u);
            const u32 next = step == 0 ? tri.neighbors[leaving ? k : (k + 2) % 3] : round_vertex(mesh, t, v, previous);
            if (next == nav_none || next == triangle) {
                break;
            }
            if (holds_direction(next)) {
                return next;
            }
            previous = t;
            t = next;
        }
    }
    return nav_none;
}

// Drops a turn wherever a straight walk on the mesh joins the points either
// side of it (the funnel's string is taut in its corridor, but the corridor
// need not be the best one round the vertices it turned at).
void shortcut(const NavMesh3D& mesh, std::vector<Corner>& path, std::span<const u32> corridor) {
    if (path.size() < 3) {
        return;
    }
    const auto clear = [&](const Corner& a, const Corner& b) {
        u32 t = corridor[std::min(a.portal, corridor.size() - 1)];
        if (a.vertex != nav_none) {
            t = triangle_toward(mesh, t, a.vertex, b.point - a.point);
            if (t == nav_none) {
                return false;
            }
        }
        // Reaching b to within rounding is reaching it: b is often a vertex on a border.
        return !walk(mesh, t, a.point, b.point - a.point, 1.0f - 1e-5f);
    };
    std::vector<Corner> out{path.front()};
    std::size_t anchor = 0;
    for (std::size_t i = 2; i < path.size(); ++i) {
        if (!clear(path[anchor], path[i])) {
            anchor = i - 1;
            out.push_back(path[anchor]);
        }
    }
    out.push_back(path.back());
    path = std::move(out);
}

// The corridor's string pulled taut, then rerouted round every vertex it turns
// at that has walkable ground all the way round (the corridor A* found need not
// hold the taut path: it can pass a vertex on the side that makes the string
// bend), until every turn is at a vertex on the mesh's border. Each reroute
// shortens the string; a few dozen at most are tried, keeping the shortest,
// and then the turns a straight walk can cut are dropped (shortcut()).
std::vector<Vec3f> taut_path(const NavMesh3D& mesh, std::vector<u32> corridor, Vec3f from, Vec3f to) {
    constexpr int max_reroutes = 64;
    std::vector<Corner> best;
    std::vector<u32> best_corridor;
    f32 best_length = inf;
    for (int attempt = 0; attempt <= max_reroutes; ++attempt) {
        trim(mesh, corridor, from, to);
        const std::vector<Portal> portals = portals_of(mesh, corridor, from, to);
        std::vector<Corner> path = pull_string(portals, mesh.options.up);
        const f32 path_length = length_of(path);
        if (path_length < best_length) {
            best_length = path_length;
            best = path;
            best_corridor = corridor;
        }
        bool rerouted = false;
        for (std::size_t c = 1; c + 1 < path.size() && !rerouted; ++c) {
            const Corner& turn = path[c];
            if (turn.vertex != nav_none && turn.portal > 0 && turn.portal < corridor.size() &&
                surrounded(mesh, corridor[turn.portal], turn.vertex)) {
                rerouted = reroute(mesh, corridor, turn);
            }
        }
        if (!rerouted) {
            break;
        }
    }
    shortcut(mesh, best, best_corridor);
    std::vector<Vec3f> points;
    points.reserve(best.size());
    for (const Corner& c : best) {
        points.push_back(c.point);
    }
    return points;
}

// Drops repeated points, and corners the path goes straight through (in line
// in 3D: a corner where only the slope changes stays).
void drop_straight_corners(std::vector<Vec3f>& path) {
    std::vector<Vec3f> out;
    out.reserve(path.size());
    for (const Vec3f p : path) {
        if (!out.empty() && out.back() == p) {
            continue;
        }
        while (out.size() >= 2) {
            const Vec3f before = out[out.size() - 1] - out[out.size() - 2];
            const Vec3f after = p - out.back();
            const f32 bend = length_squared(cross(before, after));
            if (dot(before, after) > 0.0f && bend <= 1e-10f * length_squared(before) * length_squared(after)) {
                out.pop_back();
            } else {
                break;
            }
        }
        out.push_back(p);
    }
    path = std::move(out);
}

} // namespace

std::size_t NavMesh3D::memory_bytes() const {
    return sizeof(NavMesh3D) + vertices.capacity() * sizeof(Vec3f) + triangles.capacity() * sizeof(NavTriangle3D) +
           nodes.capacity() * sizeof(NavMeshNode3D) + node_triangles.capacity() * sizeof(u32);
}

static NavMesh3D build_navmesh3d_impl(std::span<const Vec3f> vertices, std::span<const u32> indices,
                                      const NavMeshBuildOptions3D& options, std::string* error) {
    if (const std::string why = check_input(vertices, indices, options); !why.empty()) {
        return rejected(why, error);
    }
    if (error) {
        error->clear();
    }

    NavMesh3D mesh;
    mesh.options = options;
    mesh.options.up = normalize(options.up);
    const Vec3f up = mesh.options.up;

    // Weld, then keep the triangles that are whole after welding, have a
    // direction, face up and are not too steep.
    const std::vector<u32> welded = weld(vertices, options.weld_distance);
    const std::size_t input_triangles = (indices.empty() ? vertices.size() : indices.size()) / 3;
    const auto corner_of = [&](std::size_t i) { return welded[indices.empty() ? i : indices[i]]; };
    const f64 min_cos = std::cos(static_cast<f64>(options.max_slope_degrees) * 3.14159265358979323846 / 180.0);
    std::vector<std::array<u32, 3>> candidates;
    candidates.reserve(input_triangles);
    for (std::size_t t = 0; t < input_triangles; ++t) {
        const std::array<u32, 3> v{corner_of(t * 3), corner_of(t * 3 + 1), corner_of(t * 3 + 2)};
        if (v[0] == v[1] || v[1] == v[2] || v[0] == v[2]) {
            continue;
        }
        const Vec3f e1 = vertices[v[1]] - vertices[v[0]];
        const Vec3f e2 = vertices[v[2]] - vertices[v[0]];
        const Vec3f n = cross(e1, e2);
        const f64 n_length = std::sqrt(static_cast<f64>(length_squared(n)));
        // Collinear corners (to about a hundred-thousandth of a degree) give no direction.
        const f64 corners =
            std::sqrt(static_cast<f64>(length_squared(e1))) * std::sqrt(static_cast<f64>(length_squared(e2)));
        if (!(n_length > 1e-6 * corners)) {
            continue;
        }
        const f64 cos_slope =
            (static_cast<f64>(n.x) * up.x + static_cast<f64>(n.y) * up.y + static_cast<f64>(n.z) * up.z) / n_length;
        if (cos_slope < min_cos) {
            continue;
        }
        candidates.push_back(v);
    }

    // Drop a triangle that repeats an earlier one's three vertices.
    std::vector<bool> keep(candidates.size(), true);
    {
        const Incidence uses = incidence(candidates, vertices.size());
        for (u32 t = 0; t < static_cast<u32>(candidates.size()); ++t) {
            const auto key = sorted(candidates[t]);
            for (const u32 other : uses.of(candidates[t][0])) {
                if (other >= t) {
                    break;
                }
                if (keep[other] && sorted(candidates[other]) == key) {
                    keep[t] = false;
                    break;
                }
            }
        }
    }

    // Number the vertices the kept triangles use, in input order.
    std::vector<bool> used(vertices.size(), false);
    std::vector<std::array<u32, 3>> kept;
    kept.reserve(candidates.size());
    for (std::size_t t = 0; t < candidates.size(); ++t) {
        if (keep[t]) {
            kept.push_back(candidates[t]);
            for (const u32 v : candidates[t]) {
                used[v] = true;
            }
        }
    }
    std::vector<u32> renumber(vertices.size(), nav_none);
    u32 vertex_count = 0;
    for (std::size_t v = 0; v < vertices.size(); ++v) {
        if (used[v]) {
            renumber[v] = vertex_count++;
        }
    }
    mesh.vertices.resize(vertex_count);
    for (std::size_t v = 0; v < vertices.size(); ++v) {
        if (renumber[v] != nav_none) {
            mesh.vertices[renumber[v]] = vertices[v];
        }
    }
    for (auto& t : kept) {
        for (u32& v : t) {
            v = renumber[v];
        }
    }
    if (kept.empty()) {
        return mesh;
    }

    // Join triangles across edges. Two walkable triangles on either side of
    // an edge go round it in opposite directions; an edge with more than two,
    // or two on one side (folded over each other), joins nothing.
    const Incidence uses = incidence(kept, vertex_count);
    mesh.triangles.resize(kept.size());
    for (u32 t = 0; t < static_cast<u32>(kept.size()); ++t) {
        NavTriangle3D& tri = mesh.triangles[t];
        tri.vertices = kept[t];
        tri.centroid = (mesh.vertices[kept[t][0]] + mesh.vertices[kept[t][1]] + mesh.vertices[kept[t][2]]) / 3.0f;
        for (u32 i = 0; i < 3; ++i) {
            const u32 a = kept[t][i];
            const u32 b = kept[t][(i + 1) % 3];
            u32 sharing = 0;
            u32 other = nav_none;
            bool opposite = false;
            for (const u32 u : uses.of(a)) {
                if (u == t) {
                    continue;
                }
                for (u32 k = 0; k < 3; ++k) {
                    if (kept[u][k] == b) {
                        ++sharing;
                        other = u;
                        opposite = kept[u][(k + 1) % 3] == a;
                    }
                }
            }
            if (sharing == 1 && opposite) {
                tri.neighbors[i] = other;
            }
        }
    }

    // Regions: flood fills in triangle order, so region numbers follow the
    // lowest triangle of each.
    std::vector<u32> queue;
    std::vector<bool> labelled(kept.size(), false);
    for (u32 t = 0; t < static_cast<u32>(kept.size()); ++t) {
        if (labelled[t]) {
            continue;
        }
        const u32 region = mesh.regions++;
        queue.assign(1, t);
        labelled[t] = true;
        for (std::size_t q = 0; q < queue.size(); ++q) {
            mesh.triangles[queue[q]].region = region;
            for (const u32 n : mesh.triangles[queue[q]].neighbors) {
                if (n != nav_none && !labelled[n]) {
                    labelled[n] = true;
                    queue.push_back(n);
                }
            }
        }
    }

    if (options.agent_height > 0 || options.agent_radius > 0) {
        const auto cleared = clear_triangles(mesh, vertices, indices);
        auto point_options = mesh.options;
        point_options.agent_height = point_options.agent_radius = 0;
        NavMesh3D result;
        if (!cleared.empty())
            result = build_navmesh3d_impl(mesh.vertices, cleared, point_options, error);
        result.options = mesh.options;
        return result;
    }

    mesh.bounds_min = mesh.vertices[0];
    mesh.bounds_max = mesh.vertices[0];
    for (const Vec3f v : mesh.vertices) {
        mesh.bounds_min = min3(mesh.bounds_min, v);
        mesh.bounds_max = max3(mesh.bounds_max, v);
    }
    build_tree(mesh);
    return mesh;
}
Result3D<NavMesh3D> build_navmesh3d(std::span<const Vec3f> vertices, std::span<const u32> indices,
                                    const NavMeshBuildOptions3D& options) {
    std::string why;
    auto value = build_navmesh3d_impl(vertices, indices, options, &why);
    if (!why.empty())
        return {std::nullopt, {"build_navmesh3d: " + why}};
    return result3d(std::move(value));
}

#ifdef KIN_EXPERIMENTAL_3D
static NavMesh3D build_navmesh3d_impl(const MeshData3D& mesh, const Mat4f& transform,
                                      const NavMeshBuildOptions3D& options, std::string* error) {
    std::vector<Vec3f> positions;
    positions.reserve(mesh.vertices.size());
    for (const MeshVertex3D& v : mesh.vertices) {
        positions.push_back(transform.apply(v.position));
    }
    if (transform.determinant() >= 0.0f || (mesh.indices.empty() && positions.size() % 3 != 0)) {
        return build_navmesh3d_impl(positions, mesh.indices, options, error);
    }
    // A mirroring transform turns the triangles over: wind them back.
    std::vector<u32> indices = mesh.indices;
    if (indices.empty()) {
        indices.resize(positions.size());
        std::iota(indices.begin(), indices.end(), 0u);
    }
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        std::swap(indices[i + 1], indices[i + 2]);
    }
    return build_navmesh3d_impl(positions, indices, options, error);
}
Result3D<NavMesh3D> build_navmesh3d(const MeshData3D& mesh, const Mat4f& transform,
                                    const NavMeshBuildOptions3D& options) {
    std::string why;
    auto value = build_navmesh3d_impl(mesh, transform, options, &why);
    if (!why.empty())
        return {std::nullopt, {"build_navmesh3d: " + why}};
    return result3d(std::move(value));
}
#endif

std::optional<NavPoint3D> nearest(const NavMesh3D& mesh, Vec3f point, Vec3f extents) {
    if (!mesh.valid() || mesh.nodes.empty() || !finite(point) || !finite(extents) || extents.x < 0.0f ||
        extents.y < 0.0f || extents.z < 0.0f) {
        return std::nullopt;
    }
    const Vec3f low = point - extents;
    const Vec3f high = point + extents;
    std::optional<NavPoint3D> best;
    f32 best_squared = inf;
    std::vector<u32> stack;
    stack.reserve(64);
    stack.push_back(0);
    while (!stack.empty()) {
        const NavMeshNode3D& node = mesh.nodes[stack.back()];
        stack.pop_back();
        if (node.min.x > high.x || node.max.x < low.x || node.min.y > high.y || node.max.y < low.y ||
            node.min.z > high.z || node.max.z < low.z ||
            box_distance_squared(point, node.min, node.max) > best_squared) {
            continue;
        }
        if (node.count == 0) {
            stack.push_back(node.first + 1);
            stack.push_back(node.first);
            continue;
        }
        for (u32 i = node.first; i < node.first + node.count; ++i) {
            const u32 t = mesh.node_triangles[i];
            const Vec3f c = closest_on_triangle(point, corner(mesh, t, 0), corner(mesh, t, 1), corner(mesh, t, 2));
            const Vec3f d = c - point;
            if (std::abs(d.x) > extents.x || std::abs(d.y) > extents.y || std::abs(d.z) > extents.z) {
                continue;
            }
            const f32 squared = length_squared(d);
            if (squared < best_squared || (squared == best_squared && best && t < best->triangle)) {
                best_squared = squared;
                best = NavPoint3D{c, t};
            }
        }
    }
    return best;
}

std::vector<Vec3f> find_path(const NavMesh3D& mesh, Vec3f from, Vec3f to, NavPathOptions3D options) {
    const std::optional<NavPoint3D> start = nearest(mesh, from, options.extents);
    const std::optional<NavPoint3D> goal = nearest(mesh, to, options.extents);
    if (!start || !goal || mesh.triangles[start->triangle].region != mesh.triangles[goal->triangle].region) {
        return {};
    }
    if (start->triangle == goal->triangle) {
        if (start->position == goal->position) {
            return {start->position};
        }
        return {start->position, goal->position};
    }
    std::vector<u32> corridor = search_corridor(mesh, *start, *goal);
    if (corridor.empty()) {
        return {};
    }
    std::vector<Vec3f> path = taut_path(mesh, std::move(corridor), start->position, goal->position);
    drop_straight_corners(path);
    return path;
}

std::optional<NavRayHit3D> raycast(const NavMesh3D& mesh, Vec3f from, Vec3f to, Vec3f extents) {
    const std::optional<NavPoint3D> start = nearest(mesh, from, extents);
    if (!start || !finite(to)) {
        return NavRayHit3D{.position = from};
    }
    return walk(mesh, start->triangle, start->position, to - start->position, 1.0f);
}

u32 region_of(const NavMesh3D& mesh, u32 triangle) {
    return triangle < mesh.triangles.size() ? mesh.triangles[triangle].region : nav_none;
}

std::optional<NavPoint3D> random_point(const NavMesh3D& mesh, RngKey key) {
    if (!mesh.valid()) {
        return std::nullopt;
    }
    const auto [pick_key, rest] = split(key);
    const auto [u_key, v_key] = split(rest);
    const auto area = [&](u32 t) {
        const Vec3f n = cross(corner(mesh, t, 1) - corner(mesh, t, 0), corner(mesh, t, 2) - corner(mesh, t, 0));
        return std::sqrt(static_cast<f64>(length_squared(n)));
    };
    f64 total = 0.0;
    for (u32 t = 0; t < static_cast<u32>(mesh.triangles.size()); ++t) {
        total += area(t);
    }
    const f64 pick = static_cast<f64>(rng_u32(pick_key)) / 4294967296.0 * total;
    u32 triangle = static_cast<u32>(mesh.triangles.size()) - 1;
    f64 sum = 0.0;
    for (u32 t = 0; t < static_cast<u32>(mesh.triangles.size()); ++t) {
        sum += area(t);
        if (pick < sum) {
            triangle = t;
            break;
        }
    }
    // Uniform over the triangle: the square root spreads the first number by area.
    const f32 s = std::sqrt(rng_f32(u_key, 0.0f, 1.0f));
    const f32 r = rng_f32(v_key, 0.0f, 1.0f);
    const Vec3f position = corner(mesh, triangle, 0) * (1.0f - s) + corner(mesh, triangle, 1) * (s * (1.0f - r)) +
                           corner(mesh, triangle, 2) * (s * r);
    return NavPoint3D{position, triangle};
}

} // namespace kin
