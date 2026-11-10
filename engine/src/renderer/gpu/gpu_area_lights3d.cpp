// Area lights for global illumination (gpu_area_lights3d.hpp): the CPU's part.
#include "gpu_area_lights3d.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <meshoptimizer.h>
#include <tuple>
#include <unordered_map>

namespace kin::gpu {

namespace {

struct Box {
    Vec3f lo{std::numeric_limits<f32>::max(), std::numeric_limits<f32>::max(), std::numeric_limits<f32>::max()};
    Vec3f hi{-std::numeric_limits<f32>::max(), -std::numeric_limits<f32>::max(), -std::numeric_limits<f32>::max()};
    void grow(Vec3f p) {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    void grow(const Box& b) {
        if (b.empty())
            return; // empty SAH bins contribute no bounds
        grow(b.lo);
        grow(b.hi);
    }
    bool empty() const { return lo.x > hi.x; }
    f32 area() const {
        if (empty()) {
            return 0.0f;
        }
        const Vec3f d = hi - lo;
        return 2.0f * (d.x * d.y + d.y * d.z + d.z * d.x);
    }
};

f32 axis(Vec3f v, int k) {
    return k == 0 ? v.x : (k == 1 ? v.y : v.z);
}

void push4(std::vector<f32>& out, Vec3f v, f32 w) {
    out.insert(out.end(), {v.x, v.y, v.z, w});
}

// Binned surface-area heuristic, iteratively; children of a node are adjacent.
struct Builder {
    std::vector<Box> boxes; // filled by the caller
    u32 leaf_size = 4;
    std::vector<Vec3f> centres;
    std::vector<u32> order;
    struct Node {
        Box box;
        u32 first = 0, count = 0; // count 0: inside, children at first and first + 1
    };
    std::vector<Node> nodes;
    u32 depth = 0;

    void build() {
        const u32 n = static_cast<u32>(boxes.size());
        centres.resize(n);
        order.resize(n);
        for (u32 i = 0; i < n; ++i) {
            centres[i] = (boxes[i].lo + boxes[i].hi) * 0.5f;
            order[i] = i;
        }
        nodes.push_back({});
        struct Task {
            u32 node, begin, end, level;
        };
        std::vector<Task> stack{{0, 0, n, 1}};
        while (!stack.empty()) {
            const Task t = stack.back();
            stack.pop_back();
            depth = std::max(depth, t.level);
            Box box, centre_box;
            for (u32 i = t.begin; i < t.end; ++i) {
                box.grow(boxes[order[i]]);
                centre_box.grow(centres[order[i]]);
            }
            nodes[t.node].box = box;
            const u32 count = t.end - t.begin;
            auto leaf = [&] {
                nodes[t.node].first = t.begin;
                nodes[t.node].count = count;
            };
            if (count <= leaf_size || t.level + 1 >= area_light_max_depth) {
                leaf();
                continue;
            }
            constexpr int bins = 16;
            int best_axis = -1;
            int best_split = 0;
            f32 best_cost = static_cast<f32>(count) * box.area(); // as a leaf (each triangle costs one box)
            for (int k = 0; k < 3; ++k) {
                const f32 lo = axis(centre_box.lo, k), hi = axis(centre_box.hi, k);
                if (hi - lo <= 1e-12f) {
                    continue;
                }
                std::array<Box, bins> bin_box{};
                std::array<u32, bins> bin_count{};
                const f32 scale = static_cast<f32>(bins) / (hi - lo);
                for (u32 i = t.begin; i < t.end; ++i) {
                    const int b = std::min(bins - 1, static_cast<int>((axis(centres[order[i]], k) - lo) * scale));
                    bin_box[b].grow(boxes[order[i]]);
                    ++bin_count[b];
                }
                std::array<f32, bins> right_area{};
                std::array<u32, bins> right_count{};
                Box acc;
                u32 c = 0;
                for (int b = bins - 1; b > 0; --b) {
                    acc.grow(bin_box[b]);
                    c += bin_count[b];
                    right_area[b] = acc.area();
                    right_count[b] = c;
                }
                acc = Box{};
                c = 0;
                for (int b = 0; b < bins - 1; ++b) {
                    acc.grow(bin_box[b]);
                    c += bin_count[b];
                    if (c == 0 || right_count[b + 1] == 0) {
                        continue;
                    }
                    const f32 cost = 0.125f * box.area() + static_cast<f32>(c) * acc.area() +
                                     static_cast<f32>(right_count[b + 1]) * right_area[b + 1];
                    if (cost < best_cost) {
                        best_cost = cost;
                        best_axis = k;
                        best_split = b;
                    }
                }
            }
            u32 mid = 0;
            if (best_axis < 0) {
                if (count <= 2 * leaf_size) {
                    leaf();
                    continue;
                }
                // No split pays (all centres alike): halve by index.
                mid = t.begin + count / 2;
            } else {
                const f32 lo = axis(centre_box.lo, best_axis), hi = axis(centre_box.hi, best_axis);
                const f32 scale = static_cast<f32>(bins) / (hi - lo);
                const auto split = std::partition(order.begin() + t.begin, order.begin() + t.end, [&](u32 i) {
                    return std::min(bins - 1, static_cast<int>((axis(centres[i], best_axis) - lo) * scale)) <=
                           best_split;
                });
                mid = static_cast<u32>(split - order.begin());
                if (mid == t.begin || mid == t.end) {
                    mid = t.begin + count / 2;
                }
            }
            const u32 left = static_cast<u32>(nodes.size());
            nodes.push_back({});
            nodes.push_back({});
            nodes[t.node].first = left;
            nodes[t.node].count = 0;
            stack.push_back({left + 1, mid, t.end, t.level + 1});
            stack.push_back({left, t.begin, mid, t.level + 1});
        }
    }
};

bool near(Vec3f a, Vec3f b, f32 tolerance) {
    return length_squared(a - b) <= tolerance * tolerance;
}

void push_nodes(std::vector<f32>& out, const Builder& b) {
    for (const Builder::Node& n : b.nodes) {
        push4(out, n.box.lo, std::bit_cast<f32>(n.first));
        push4(out, n.box.hi, std::bit_cast<f32>(n.count));
    }
}

} // namespace

namespace {

// A mesh's corners welded on a grid a millionth of its size: a seam's copies of a vertex often differ in the last bits
// (a sphere's sin(2 pi) against sin(0), -0 against 0). Each corner's vertex, and each vertex's position (its first
// corner's); empty when the corners span nothing.
struct Welded {
    std::vector<u32> id;
    std::vector<Vec3f> positions;
};

Welded weld(std::span<const Vec3f> corners) {
    Box box;
    for (const Vec3f& c : corners) {
        box.grow(c);
    }
    const f32 size = std::max({box.hi.x - box.lo.x, box.hi.y - box.lo.y, box.hi.z - box.lo.z});
    if (corners.empty() || !(size > 0.0f) || !std::isfinite(size)) {
        return {};
    }
    const f64 step = 1e-6 * size;
    const auto cell = [&](f32 v, f32 lo) { return static_cast<u64>(std::llround((v - lo) / step)) & 0x1fffffu; };
    Welded out;
    std::unordered_map<u64, u32> vertices;
    vertices.reserve(corners.size());
    out.id.resize(corners.size());
    for (std::size_t i = 0; i < corners.size(); ++i) {
        const Vec3f c = corners[i];
        const u64 key = cell(c.x, box.lo.x) | cell(c.y, box.lo.y) << 21 | cell(c.z, box.lo.z) << 42;
        const auto [it, fresh] = vertices.try_emplace(key, static_cast<u32>(out.positions.size()));
        if (fresh) {
            out.positions.push_back(c);
        }
        out.id[i] = it->second;
    }
    return out;
}

} // namespace

std::vector<Vec3f> simplify_area_light_occluder(std::span<const Vec3f> corners, std::size_t target) {
    const std::size_t n = corners.size() - corners.size() % 3;
    const std::vector<Vec3f> same(corners.begin(), corners.begin() + static_cast<std::ptrdiff_t>(n));
    const Welded w = weld(same);
    if (w.positions.empty() || target * 3 >= n) {
        return same;
    }
    std::vector<u32> kept(n);
    f32 reached = 0.0f;
    // At most a tenth of the mesh's size off (past that a shape is no longer its own: the budget is then not met), its
    // open edges where they are (a wall's outline).
    const std::size_t count =
        meshopt_simplify(kept.data(), w.id.data(), n, &w.positions[0].x, w.positions.size(), sizeof(Vec3f),
                         std::max<std::size_t>(target * 3, 3), 0.1f, meshopt_SimplifyLockBorder, &reached);
    if (count == 0 || count >= n) {
        return same;
    }
    std::vector<Vec3f> out(count);
    for (std::size_t i = 0; i < count; ++i) {
        out[i] = w.positions[kept[i]];
    }
    return out;
}

bool area_light_closed(std::span<const Vec3f> corners) {
    const std::size_t n = corners.size() - corners.size() % 3;
    if (n < 12) {
        return false; // fewer than four triangles close nothing
    }
    const Welded w = weld(corners.first(n));
    if (w.positions.empty()) {
        return false;
    }
    const std::vector<u32>& id = w.id;
    std::unordered_map<u64, u32> directed; // (from, to): how many triangles have that edge that way
    directed.reserve(n);
    for (std::size_t i = 0; i < n; i += 3) {
        for (std::size_t k = 0; k < 3; ++k) {
            const u32 from = id[i + k], to = id[i + (k + 1) % 3];
            if (from == to) {
                return false; // a degenerate triangle: not worth reasoning about
            }
            ++directed[(u64{from} << 32) | to];
        }
    }
    for (const auto& [key, count] : directed) {
        const auto back = directed.find((key << 32) | (key >> 32));
        if (count != 1 || back == directed.end() || back->second != 1) {
            return false;
        }
    }
    return true;
}

AreaLightMesh build_area_light_mesh(std::span<const AreaLightTriangle> triangles) {
    AreaLightMesh out;
    if (triangles.empty()) {
        return out;
    }
    Builder b;
    b.boxes.resize(triangles.size());
    for (std::size_t i = 0; i < triangles.size(); ++i) {
        b.boxes[i].grow(triangles[i].a);
        b.boxes[i].grow(triangles[i].b);
        b.boxes[i].grow(triangles[i].c);
    }
    b.build();
    out.depth = b.depth;
    out.triangle_count = static_cast<u32>(triangles.size());
    out.triangles.reserve(triangles.size() * area_light_floats_triangle);
    for (const u32 i : b.order) {
        const AreaLightTriangle& t = triangles[i];
        const Vec3f e0 = t.b - t.a, e1 = t.c - t.a, n = cross(e0, e1);
        push4(out.triangles, t.a, dot(t.a, n));
        push4(out.triangles, e0, 0.0f);
        push4(out.triangles, e1, 0.0f);
        push4(out.triangles, n, t.closed ? 1.0f : 0.0f);
    }
    out.node_count = static_cast<u32>(b.nodes.size());
    out.nodes.reserve(b.nodes.size() * area_light_floats_node);
    push_nodes(out.nodes, b);
    return out;
}

AreaLightCopies build_area_light_copies(std::span<const AreaLightCopy> copies) {
    AreaLightCopies out;
    if (copies.empty()) {
        return out;
    }
    Builder b;
    b.leaf_size = 2;
    b.boxes.resize(copies.size());
    for (std::size_t i = 0; i < copies.size(); ++i) {
        b.boxes[i].lo = copies[i].lo;
        b.boxes[i].hi = copies[i].hi;
    }
    b.build();
    out.depth = b.depth;
    out.node_count = static_cast<u32>(b.nodes.size());
    out.copy_count = static_cast<u32>(copies.size());
    out.data.reserve(b.nodes.size() * area_light_floats_node + copies.size() * area_light_floats_copy);
    push_nodes(out.data, b);
    for (const u32 i : b.order) {
        const AreaLightCopy& c = copies[i];
        const Mat4f& t = c.to_world;
        const Mat4f inv = t.inverse();
        for (int r = 0; r < 3; ++r) {
            push4(out.data, {inv.at(r, 0), inv.at(r, 1), inv.at(r, 2)}, inv.at(r, 3));
        }
        for (int col = 0; col < 3; ++col) {
            push4(out.data, {t.at(0, col), t.at(1, col), t.at(2, col)}, 0.0f);
        }
        out.data.insert(out.data.end(), {std::bit_cast<f32>(c.first_node), std::bit_cast<f32>(c.first_triangle),
                                         c.world ? 1.0f : 0.0f, 0.0f});
    }
    return out;
}

AreaLightEmitters build_area_light_emitters(std::span<const AreaLightEmitterTriangle> emitters, u32 max_emitters,
                                            f32 min_size) {
    AreaLightEmitters out;
    // Emitters, a GI emitter's triangles together: one that would not all fit stays with the probes.
    std::map<std::tuple<u32, u64>, std::vector<u32>> by_owner; // (GI emitter, group) -> triangles
    for (u32 i = 0; i < emitters.size(); ++i) {
        const AreaLightEmitterTriangle& e = emitters[i];
        const Vec3f n = cross(e.b - e.a, e.c - e.a);
        if (length_squared(n) <= 1e-20f || (e.radiance.x <= 0.0f && e.radiance.y <= 0.0f && e.radiance.z <= 0.0f)) {
            continue;
        }
        by_owner[{e.gi_emitter, e.group}].push_back(i);
    }
    struct Primitive {
        Vec3f p0, p1, p2, normal, radiance;
        f32 kind, jacobian;
        u32 gi_emitter;
        bool two_sided;
    };
    std::map<u32, std::vector<Primitive>> by_emitter; // GI emitter (0: baked strength) -> primitives
    for (const auto& [owner, list] : by_owner) {
        std::vector<bool> used(list.size(), false);
        for (std::size_t i = 0; i < list.size(); ++i) {
            if (used[i]) {
                continue;
            }
            const AreaLightEmitterTriangle& t = emitters[list[i]];
            const Vec3f n = cross(t.b - t.a, t.c - t.a);
            const f32 size =
                std::sqrt(std::max({length_squared(t.b - t.a), length_squared(t.c - t.b), length_squared(t.a - t.c)}));
            const f32 tolerance = 1e-5f * size;
            bool paired = false;
            for (std::size_t j = i + 1; j < list.size() && !paired; ++j) {
                if (used[j]) {
                    continue;
                }
                const AreaLightEmitterTriangle& u = emitters[list[j]];
                if (!(u.radiance == t.radiance) || u.two_sided != t.two_sided) {
                    continue;
                }
                // Two shared corners (the diagonal), the other two opposite it: a parallelogram when the diagonals
                // share their middle.
                const std::array<Vec3f, 3> p{t.a, t.b, t.c}, q{u.a, u.b, u.c};
                int shared_p[2], shared_q[2], k = 0;
                for (int x = 0; x < 3 && k < 3; ++x) {
                    for (int y = 0; y < 3; ++y) {
                        if (near(p[x], q[y], tolerance)) {
                            if (k < 2) {
                                shared_p[k] = x;
                                shared_q[k] = y;
                            }
                            ++k;
                            break;
                        }
                    }
                }
                if (k != 2) {
                    continue;
                }
                const Vec3f other_p = p[3 - shared_p[0] - shared_p[1]], other_q = q[3 - shared_q[0] - shared_q[1]];
                const Vec3f s0 = p[shared_p[0]], s1 = p[shared_p[1]];
                if (!near(other_p + other_q, s0 + s1, 2.0f * tolerance) ||
                    dot(cross(u.b - u.a, u.c - u.a), n) <= 0.0f) {
                    continue;
                }
                Vec3f side0 = s0 - other_p, side1 = s1 - other_p;
                if (length_squared(side0) > length_squared(side1)) {
                    std::swap(side0, side1); // side1: the longer, the lines' way
                }
                const Vec3f cr = cross(side0, side1);
                const f32 area = length(cr);
                Vec3f unit = cr / area;
                if (dot(unit, n) < 0.0f) {
                    unit = -unit;
                }
                by_emitter[t.gi_emitter].push_back(
                    {other_p, side0, side1, unit, t.radiance, 0.0f, area, t.gi_emitter, t.two_sided});
                used[i] = used[j] = paired = true;
            }
            if (!paired) {
                // A triangle: the apex opposite its longest side, whose lines run along that side.
                const std::array<Vec3f, 3> p{t.a, t.b, t.c};
                int apex = 0;
                f32 longest = -1.0f;
                for (int x = 0; x < 3; ++x) {
                    const f32 l = length_squared(p[(x + 2) % 3] - p[(x + 1) % 3]);
                    if (l > longest) {
                        longest = l;
                        apex = x;
                    }
                }
                const Vec3f a = p[apex], b = p[(apex + 1) % 3], c = p[(apex + 2) % 3];
                const f32 area2 = length(n);
                by_emitter[t.gi_emitter].push_back(
                    {a, b - a, c - b, n / area2, t.radiance, 1.0f, area2, t.gi_emitter, t.two_sided});
                used[i] = true;
            }
        }
    }
    std::vector<Primitive> chosen;
    for (auto& [gi, prims] : by_emitter) {
        // Emitters without a GI number keep their strength in their voxels, where the probes count it: they stay
        // there (lit here as well, they would count twice). Past the limit, a GI emitter stays with the probes whole,
        // and so does one all of whose pieces are smaller than min_size.
        f32 largest = 0.0f;
        for (const Primitive& p : prims) {
            largest =
                std::max(largest, p.kind > 0.5f ? 0.5f * p.jacobian : p.jacobian); // (a triangle's: twice its area)
        }
        if (gi == 0 || chosen.size() + prims.size() > max_emitters || largest < min_size * min_size) {
            continue;
        }
        chosen.insert(chosen.end(), prims.begin(), prims.end());
        out.explicit_emitters.push_back(gi);
    }
    out.count = static_cast<u32>(chosen.size());
    for (const Primitive& p : chosen) {
        push4(out.data, p.p0, p.kind);
        push4(out.data, p.p1, static_cast<f32>(p.gi_emitter));
        push4(out.data, p.p2, p.jacobian);
        push4(out.data, p.normal, p.two_sided ? 1.0f : 0.0f);
        push4(out.data, p.radiance, 0.0f);
    }
    return out;
}

AreaLightPieces area_light_pieces(std::span<const Vec3f> corners) {
    std::vector<AreaLightEmitterTriangle> triangles;
    triangles.reserve(corners.size() / 3);
    for (std::size_t i = 0; i + 2 < corners.size(); i += 3) {
        triangles.push_back({corners[i], corners[i + 1], corners[i + 2], {1.0f, 1.0f, 1.0f}, 1, false, 0});
    }
    const AreaLightEmitters pieces = build_area_light_emitters(triangles, std::numeric_limits<u32>::max());
    AreaLightPieces out{.count = pieces.count};
    for (u32 e = 0; e < pieces.count; ++e) {
        const f32* d = pieces.data.data() + e * area_light_floats_emitter;
        out.largest =
            std::max(out.largest, d[3] > 0.5f ? 0.5f * d[11] : d[11]); // a triangle's jacobian: twice its area
    }
    return out;
}

} // namespace kin::gpu
