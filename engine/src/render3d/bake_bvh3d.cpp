#include "bake_bvh3d.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace kin::bake {

namespace {

constexpr f32 inf = std::numeric_limits<f32>::infinity();
constexpr u32 bins = 16;
constexpr u32 leaf_size = 4;
constexpr u32 max_depth = 64;

struct Box {
    Vec3f lo{inf, inf, inf};
    Vec3f hi{-inf, -inf, -inf};
    void grow(Vec3f p) {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    void grow(const Box& b) {
        grow(b.lo);
        grow(b.hi);
    }
    bool empty() const { return !(lo.x <= hi.x); }
    f32 area() const {
        if (empty())
            return 0.0f;
        const Vec3f d = hi - lo;
        return 2.0f * (d.x * d.y + d.y * d.z + d.z * d.x);
    }
};

f32 axis(Vec3f v, u32 a) {
    return a == 0 ? v.x : a == 1 ? v.y : v.z;
}

struct Builder {
    std::vector<Box> boxes;
    std::vector<Vec3f> centres;
    std::vector<u32> ids;
    struct Out {
        Box box;
        u32 first = 0, count = 0;
    };
    std::vector<Out> nodes; // siblings side by side: an inner node's children are at first and first + 1

    void make(u32 index, u32 first, u32 count, u32 depth) {
        Box box, centre_box;
        for (u32 i = first; i < first + count; ++i) {
            box.grow(boxes[ids[i]]);
            centre_box.grow(centres[ids[i]]);
        }
        nodes[index] = {box, first, count};
        if (count <= leaf_size || depth >= max_depth) {
            return;
        }
        // Binned SAH over the widest axis of the centres.
        const Vec3f extent = centre_box.hi - centre_box.lo;
        const u32 a = extent.x >= extent.y && extent.x >= extent.z ? 0 : extent.y >= extent.z ? 1 : 2;
        const f32 lo = axis(centre_box.lo, a), width = axis(extent, a);
        u32 middle = first + count / 2; // every centre in one place: halves by order
        if (width > 0.0f) {
            std::array<Box, bins> bin_box{};
            std::array<u32, bins> bin_count{};
            const f32 scale = static_cast<f32>(bins) / width;
            const auto bin_of = [&](u32 id) {
                return std::min(bins - 1, static_cast<u32>((axis(centres[id], a) - lo) * scale));
            };
            for (u32 i = first; i < first + count; ++i) {
                const u32 b = bin_of(ids[i]);
                bin_box[b].grow(boxes[ids[i]]);
                ++bin_count[b];
            }
            std::array<f32, bins> right_cost{};
            Box right;
            u32 right_n = 0;
            for (u32 b = bins - 1; b > 0; --b) {
                right.grow(bin_box[b]);
                right_n += bin_count[b];
                right_cost[b] = right.area() * static_cast<f32>(right_n);
            }
            Box left;
            u32 left_n = 0, best = 0;
            f32 best_cost = inf;
            for (u32 b = 0; b + 1 < bins; ++b) {
                left.grow(bin_box[b]);
                left_n += bin_count[b];
                const f32 cost = left.area() * static_cast<f32>(left_n) + right_cost[b + 1];
                if (left_n > 0 && left_n < count && cost < best_cost) {
                    best_cost = cost;
                    best = b;
                }
            }
            // A small node that no split makes cheaper stays a leaf.
            if (!(best_cost < box.area() * static_cast<f32>(count)) && count <= 4 * leaf_size) {
                return;
            }
            if (best_cost < inf) {
                const auto split = std::stable_partition(ids.begin() + first, ids.begin() + first + count,
                                                         [&](u32 id) { return bin_of(id) <= best; });
                middle = static_cast<u32>(split - ids.begin());
            }
        }
        if (middle == first || middle == first + count) {
            middle = first + count / 2;
        }
        const u32 child = static_cast<u32>(nodes.size());
        nodes.resize(nodes.size() + 2);
        nodes[index].first = child;
        nodes[index].count = 0;
        make(child, first, middle - first, depth + 1);
        make(child + 1, middle, first + count - middle, depth + 1);
    }
};

} // namespace

void TriangleBvh::build(std::span<const Vec3f> corners) {
    _nodes.clear();
    _tris.clear();
    _order.clear();
    const u32 n = static_cast<u32>(corners.size() / 3);
    if (n == 0) {
        return;
    }
    Builder b;
    b.boxes.resize(n);
    b.centres.resize(n);
    b.ids.resize(n);
    for (u32 i = 0; i < n; ++i) {
        Box box;
        box.grow(corners[3 * i]);
        box.grow(corners[3 * i + 1]);
        box.grow(corners[3 * i + 2]);
        b.boxes[i] = box;
        b.centres[i] = (box.lo + box.hi) * 0.5f;
        b.ids[i] = i;
    }
    b.nodes.reserve(2 * n / leaf_size + 2);
    b.nodes.resize(1);
    b.make(0, 0, n, 0);
    _nodes.resize(b.nodes.size());
    for (std::size_t i = 0; i < b.nodes.size(); ++i) {
        _nodes[i] = {b.nodes[i].box.lo, b.nodes[i].first, b.nodes[i].box.hi, b.nodes[i].count};
    }
    // Leaves index ids directly: the triangles are laid out in that order.
    _tris.resize(n);
    _order = b.ids;
    for (u32 i = 0; i < n; ++i) {
        const u32 id = b.ids[i];
        const Vec3f a = corners[3 * id];
        _tris[i] = {a, corners[3 * id + 1] - a, corners[3 * id + 2] - a};
    }
}

namespace {

// Möller-Trumbore, both sides; `back` when the ray meets the side the counter-clockwise winding faces away from.
inline bool hit_triangle(Vec3f o, Vec3f d, Vec3f a, Vec3f e1, Vec3f e2, f32 t_max, f32& t, f32& u, f32& v, bool& back) {
    const Vec3f p = cross(d, e2);
    const f32 det = dot(e1, p);
    if (std::fabs(det) < 1e-12f) {
        return false;
    }
    const f32 inv = 1.0f / det;
    const Vec3f s = o - a;
    u = dot(s, p) * inv;
    if (u < 0.0f || u > 1.0f) {
        return false;
    }
    const Vec3f q = cross(s, e1);
    v = dot(d, q) * inv;
    if (v < 0.0f || u + v > 1.0f) {
        return false;
    }
    t = dot(e2, q) * inv;
    if (!(t > 0.0f && t < t_max)) {
        return false;
    }
    back = det < 0.0f;
    return true;
}

struct RayBox {
    Vec3f o, inv;
    // Entry distance into the box, or inf.
    f32 enter(Vec3f lo, Vec3f hi, f32 t_max) const {
        const f32 tx0 = (lo.x - o.x) * inv.x, tx1 = (hi.x - o.x) * inv.x;
        const f32 ty0 = (lo.y - o.y) * inv.y, ty1 = (hi.y - o.y) * inv.y;
        const f32 tz0 = (lo.z - o.z) * inv.z, tz1 = (hi.z - o.z) * inv.z;
        const f32 t0 = std::max({std::min(tx0, tx1), std::min(ty0, ty1), std::min(tz0, tz1), 0.0f});
        const f32 t1 = std::min({std::max(tx0, tx1), std::max(ty0, ty1), std::max(tz0, tz1), t_max});
        return t0 <= t1 ? t0 : inf;
    }
};

RayBox ray_box(Vec3f o, Vec3f d) {
    const auto rcp = [](f32 x) { return std::fabs(x) > 1e-30f ? 1.0f / x : (x >= 0.0f ? 1e30f : -1e30f); };
    return {o, {rcp(d.x), rcp(d.y), rcp(d.z)}};
}

} // namespace

bool TriangleBvh::nearest(Vec3f origin, Vec3f direction, f32 t_max, RayHit& hit) const {
    if (_nodes.empty()) {
        return false;
    }
    const RayBox rb = ray_box(origin, direction);
    if (rb.enter(_nodes[0].lo, _nodes[0].hi, t_max) == inf) {
        return false;
    }
    std::array<u32, 128> stack;
    u32 top = 0;
    stack[top++] = 0;
    bool found = false;
    f32 best = t_max;
    while (top > 0) {
        const Node& node = _nodes[stack[--top]];
        if (node.count > 0) {
            for (u32 i = node.first; i < node.first + node.count; ++i) {
                const Tri& tri = _tris[i];
                f32 t, u, v;
                bool back;
                if (hit_triangle(origin, direction, tri.a, tri.e1, tri.e2, best, t, u, v, back)) {
                    best = t;
                    found = true;
                    hit = {t, _order[i], u, v, back};
                }
            }
            continue;
        }
        const Node& l = _nodes[node.first];
        const Node& r = _nodes[node.first + 1];
        const f32 tl = rb.enter(l.lo, l.hi, best), tr = rb.enter(r.lo, r.hi, best);
        // The nearer child is walked first (pushed last).
        if (tl <= tr) {
            if (tr != inf)
                stack[top++] = node.first + 1;
            if (tl != inf)
                stack[top++] = node.first;
        } else {
            if (tl != inf)
                stack[top++] = node.first;
            if (tr != inf)
                stack[top++] = node.first + 1;
        }
    }
    return found;
}

bool TriangleBvh::any(Vec3f origin, Vec3f direction, f32 t_max) const {
    if (_nodes.empty()) {
        return false;
    }
    const RayBox rb = ray_box(origin, direction);
    std::array<u32, 128> stack;
    u32 top = 0;
    stack[top++] = 0;
    while (top > 0) {
        const Node& node = _nodes[stack[--top]];
        if (rb.enter(node.lo, node.hi, t_max) == inf) {
            continue;
        }
        if (node.count > 0) {
            for (u32 i = node.first; i < node.first + node.count; ++i) {
                const Tri& tri = _tris[i];
                f32 t, u, v;
                bool back;
                if (hit_triangle(origin, direction, tri.a, tri.e1, tri.e2, t_max, t, u, v, back)) {
                    return true;
                }
            }
            continue;
        }
        stack[top++] = node.first + 1;
        stack[top++] = node.first;
    }
    return false;
}

} // namespace kin::bake
