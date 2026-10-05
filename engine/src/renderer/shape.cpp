#include <kin/renderer/shape.hpp>

#include <mapbox/earcut.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <optional>
#include <span>

namespace mapbox::util {
template <>
struct nth<0, kin::Vec2f> {
    static auto get(const kin::Vec2f& p) { return p.x; }
};
template <>
struct nth<1, kin::Vec2f> {
    static auto get(const kin::Vec2f& p) { return p.y; }
};
} // namespace mapbox::util

namespace kin {

namespace {

constexpr f32 pi = std::numbers::pi_v<f32>;

Vec2f add(Vec2f a, Vec2f b) { return {a.x + b.x, a.y + b.y}; }
Vec2f sub(Vec2f a, Vec2f b) { return {a.x - b.x, a.y - b.y}; }
Vec2f mul(Vec2f a, f32 s) { return {a.x * s, a.y * s}; }
f32 dot(Vec2f a, Vec2f b) { return a.x * b.x + a.y * b.y; }
f32 cross(Vec2f a, Vec2f b) { return a.x * b.y - a.y * b.x; }
f32 length(Vec2f a) { return std::sqrt(dot(a, a)); }
// The normal on the travelling direction's left in the numbers' own terms; the
// outlines below are oriented so this side is outside what they bound.
Vec2f perp(Vec2f t) { return {-t.y, t.x}; }
Vec2f normalize(Vec2f a) {
    const f32 l = length(a);
    return l > 0.0f ? mul(a, 1.0f / l) : Vec2f{};
}

f32 signed_area(std::span<const Vec2f> pts) {
    f32 area = 0.0f;
    for (std::size_t i = 0, j = pts.size() - 1; i < pts.size(); j = i++) {
        area += pts[j].x * pts[i].y - pts[i].x * pts[j].y;
    }
    return area * 0.5f;
}

// How many times `ring` winds around `p` (+1 for a ring of positive area).
int winding_number(Vec2f p, std::span<const Vec2f> ring) {
    int wn = 0;
    for (std::size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
        const Vec2f a = ring[j], b = ring[i];
        const f32 left = cross(sub(b, a), sub(p, a));
        if (a.y <= p.y) {
            if (b.y > p.y && left > 0.0f) {
                ++wn;
            }
        } else if (b.y <= p.y && left < 0.0f) {
            --wn;
        }
    }
    return wn;
}

// Drops points that repeat the one before (and, closed, the first).
void clean(std::vector<Vec2f>& pts, bool closed) {
    constexpr f32 eps_sq = 1e-12f;
    std::size_t kept = 0;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        if (kept == 0 || dot(sub(pts[i], pts[kept - 1]), sub(pts[i], pts[kept - 1])) > eps_sq) {
            pts[kept++] = pts[i];
        }
    }
    pts.resize(kept);
    while (closed && pts.size() > 1 && dot(sub(pts.back(), pts.front()), sub(pts.back(), pts.front())) <= eps_sq) {
        pts.pop_back();
    }
}

// Segments in an arc of radius `r` so it strays at most `tolerance` from the circle.
f32 arc_step(f32 r, f32 tolerance) {
    if (r <= tolerance) {
        return pi * 0.5f;
    }
    return std::clamp(2.0f * std::acos(1.0f - tolerance / r), pi / 64.0f, pi * 0.5f);
}

struct Out {
    ShapeMesh& mesh;
    Color color;

    u32 vertex(Vec2f p, f32 edge = 0.0f, Vec2f outward = {}) {
        mesh.vertices.push_back({p, color, edge, outward});
        return static_cast<u32>(mesh.vertices.size() - 1);
    }
    Vec2f at(u32 i) const { return mesh.vertices[i].position; }
    void tri(u32 a, u32 b, u32 c) { mesh.indices.insert(mesh.indices.end(), {a, b, c}); }
    void quad(u32 a, u32 b, u32 c, u32 d) {
        tri(a, b, c);
        tri(a, c, d);
    }

    // The soft edge along a closed outline of existing vertices, whose outside
    // is on perp(travel): a rim `fringe` out (edge -fringe), along each corner's
    // miter so the strip is as wide beside both its edges.
    void fringe_ring(std::span<const u32> ring, f32 fringe) {
        const std::size_t n = ring.size();
        if (n < 2 || fringe <= 0.0f) {
            return;
        }
        const u32 first = static_cast<u32>(mesh.vertices.size());
        for (std::size_t i = 0; i < n; ++i) {
            const Vec2f p = at(ring[i]);
            // The nearest neighbours that are not on top of it.
            Vec2f e0{}, e1{};
            for (std::size_t k = 1; k < n && e0 == Vec2f{}; ++k) {
                e0 = normalize(sub(p, at(ring[(i + n - k) % n])));
            }
            for (std::size_t k = 1; k < n && e1 == Vec2f{}; ++k) {
                e1 = normalize(sub(at(ring[(i + k) % n]), p));
            }
            Vec2f dm = mul(add(perp(e0), perp(e1)), 0.5f);
            const f32 d2 = dot(dm, dm);
            if (d2 > 1e-6f) {
                dm = mul(dm, 1.0f / d2);
                const f32 l = length(dm);
                if (l > 4.0f) { // a spike: cut the miter short
                    dm = mul(dm, 4.0f / l);
                }
            } else {
                dm = perp(e0); // turning straight back
            }
            vertex(add(p, mul(dm, fringe)), -fringe, dm);
        }
        for (u32 i = 0; i < n; ++i) {
            const u32 j = (i + 1) % static_cast<u32>(n);
            quad(ring[i], ring[j], first + j, first + i);
        }
    }

    // A convex polygon, filled, with its soft edge.
    void convex(std::vector<Vec2f> pts, f32 fringe) {
        if (pts.size() < 3) {
            return;
        }
        if (signed_area(pts) > 0.0f) {
            std::reverse(pts.begin(), pts.end()); // the inside on the right, as fringe_ring wants
        }
        std::vector<u32> ring;
        for (const Vec2f& p : pts) {
            ring.push_back(vertex(p));
        }
        for (std::size_t i = 1; i + 1 < ring.size(); ++i) {
            tri(ring[0], ring[i], ring[i + 1]);
        }
        fringe_ring(ring, fringe);
    }
};

// Triangles appended since (first_vertex, first_index) join the last run of
// triangles, or start one.
void note_triangles(ShapeMesh& mesh, std::size_t first_vertex, std::size_t first_index) {
    const u32 count = static_cast<u32>(mesh.indices.size() - first_index);
    if (count == 0) {
        return;
    }
    if (!mesh.runs.empty() && !mesh.runs.back().primitives) {
        ShapeMesh::Run& run = mesh.runs.back();
        run.count += count;
        run.vertex_count = static_cast<u32>(mesh.vertices.size()) - run.first_vertex;
        return;
    }
    mesh.runs.push_back({.primitives = false,
                         .first = static_cast<u32>(first_index),
                         .count = count,
                         .first_vertex = static_cast<u32>(first_vertex),
                         .vertex_count = static_cast<u32>(mesh.vertices.size() - first_vertex)});
}

void grow_bounds_by(ShapeMesh& mesh, Rectf r) {
    const bool empty = mesh.bounds.w <= 0.0f && mesh.bounds.h <= 0.0f && mesh.vertices.empty() &&
                       mesh.primitives.size() <= 1;
    if (empty) {
        mesh.bounds = r;
        return;
    }
    const f32 x0 = std::min(mesh.bounds.x, r.x), y0 = std::min(mesh.bounds.y, r.y);
    const f32 x1 = std::max(mesh.bounds.x + mesh.bounds.w, r.x + r.w), y1 = std::max(mesh.bounds.y + mesh.bounds.h, r.y + r.h);
    mesh.bounds = {x0, y0, x1 - x0, y1 - y0};
}

Rectf primitive_bounds(const ShapePrimitive& p) {
    const f32 reach = p.stroke.a > 0 ? p.stroke_width * 0.5f : 0.0f;
    const Vec2f e{p.half_size.x + reach, p.half_size.y + reach};
    return transformed_bounds(p.transform, {-e.x, -e.y, 2.0f * e.x, 2.0f * e.y});
}

void grow_bounds(ShapeMesh& mesh, std::size_t first) {
    if (first >= mesh.vertices.size()) {
        return;
    }
    f32 x0 = mesh.vertices[first].position.x, y0 = mesh.vertices[first].position.y, x1 = x0, y1 = y0;
    if (first > 0 || mesh.bounds.w > 0.0f || mesh.bounds.h > 0.0f || !mesh.primitives.empty()) {
        x0 = std::min(x0, mesh.bounds.x);
        y0 = std::min(y0, mesh.bounds.y);
        x1 = std::max(x1, mesh.bounds.x + mesh.bounds.w);
        y1 = std::max(y1, mesh.bounds.y + mesh.bounds.h);
    }
    for (std::size_t i = first; i < mesh.vertices.size(); ++i) {
        const Vec2f p = mesh.vertices[i].position;
        x0 = std::min(x0, p.x);
        y0 = std::min(y0, p.y);
        x1 = std::max(x1, p.x);
        y1 = std::max(y1, p.y);
    }
    mesh.bounds = {x0, y0, x1 - x0, y1 - y0};
}

// The smaller and larger of how much `m` stretches lengths (close enough for
// widths and tolerances: its columns' lengths).
std::pair<f32, f32> scales(const Affine2& m) {
    const f32 sx = std::sqrt(m.a * m.a + m.b * m.b), sy = std::sqrt(m.c * m.c + m.d * m.d);
    return {std::min(sx, sy), std::max(sx, sy)};
}

struct Ring {
    std::vector<Vec2f> pts;
    f32 area = 0.0f;
    Vec2f probe{}; // just inside the ring, beside its first edge
    bool outer = false;
};

// A stroke as one strip: two vertices a point where the path bends gently
// (the miters, shared by both segments), more where a join or cap needs them.
void stroke_contour(Out& out, std::vector<Vec2f> pts, bool closed, const StrokeStyle& style, f32 fringe,
                    f32 tolerance) {
    clean(pts, closed);
    const f32 hw = style.width * 0.5f;
    if (pts.empty() || hw <= 0.0f) {
        return;
    }
    if (pts.size() == 1) {
        // A lone point: a dot under a round or square cap, nothing under a butt.
        const Vec2f p = pts[0];
        std::vector<Vec2f> dot_pts;
        if (style.cap == LineCap::Round) {
            const int n = std::max(8, static_cast<int>(std::ceil(2.0f * pi / arc_step(hw, tolerance))));
            for (int i = 0; i < n; ++i) {
                const f32 a = 2.0f * pi * static_cast<f32>(i) / static_cast<f32>(n);
                dot_pts.push_back({p.x + hw * std::cos(a), p.y + hw * std::sin(a)});
            }
        } else if (style.cap == LineCap::Square) {
            dot_pts = {{p.x - hw, p.y - hw}, {p.x + hw, p.y - hw}, {p.x + hw, p.y + hw}, {p.x - hw, p.y + hw}};
        }
        out.convex(std::move(dot_pts), fringe);
        return;
    }
    if (closed && pts.size() == 2) {
        closed = false; // there and back: the same as the open line
    }

    const std::size_t n = pts.size();
    const std::size_t segments = closed ? n : n - 1;
    thread_local std::vector<Vec2f> dir, nrm;
    thread_local std::vector<f32> len;
    dir.resize(segments);
    nrm.resize(segments);
    len.resize(segments);
    for (std::size_t k = 0; k < segments; ++k) {
        const Vec2f d = sub(pts[(k + 1) % n], pts[k]);
        len[k] = length(d);
        dir[k] = mul(d, 1.0f / len[k]);
        nrm[k] = perp(dir[k]);
    }
    if (!closed && style.cap == LineCap::Square) { // a square cap is the line run on by half its width
        pts[0] = sub(pts[0], mul(dir[0], hw));
        pts[n - 1] = add(pts[n - 1], mul(dir[segments - 1], hw));
    }
    const f32 step = arc_step(hw, tolerance);
    // Points on a circle round `p` from direction `u` towards `w` (unit,
    // perpendicular) through `angle` radians; the ends left out.
    const auto arc = [&](Vec2f p, Vec2f u, Vec2f w, f32 angle) {
        std::vector<u32> between;
        const int count = std::max(1, static_cast<int>(std::ceil(angle / step)));
        for (int k = 1; k < count; ++k) {
            const f32 t = angle * static_cast<f32>(k) / static_cast<f32>(count);
            between.push_back(out.vertex(add(p, add(mul(u, hw * std::cos(t)), mul(w, hw * std::sin(t))))));
        }
        return between;
    };

    // Each point's vertices on the left (+normal) and right, in travelling
    // order: spans of one pool, reused from call to call on this thread.
    struct Side {
        u32 first = 0, count = 0;
    };
    thread_local std::vector<u32> pool;
    thread_local std::vector<Side> left_spans, right_spans;
    pool.clear();
    left_spans.assign(n, {});
    right_spans.assign(n, {});
    const auto keep = [&](std::vector<Side>& spans, std::size_t i, std::initializer_list<u32> vertices) {
        spans[i] = {static_cast<u32>(pool.size()), static_cast<u32>(vertices.size())};
        pool.insert(pool.end(), vertices);
    };
    const auto keep_list = [&](std::vector<Side>& spans, std::size_t i, const std::vector<u32>& vertices) {
        spans[i] = {static_cast<u32>(pool.size()), static_cast<u32>(vertices.size())};
        pool.insert(pool.end(), vertices.begin(), vertices.end());
    };
    const auto front = [&](const Side& side) { return pool[side.first]; };
    const auto back = [&](const Side& side) { return pool[side.first + side.count - 1]; };
    for (std::size_t i = 0; i < n; ++i) {
        const Vec2f p = pts[i];
        if (!closed && (i == 0 || i == n - 1)) {
            const Vec2f o = mul(nrm[i == 0 ? 0 : segments - 1], hw);
            keep(left_spans, i, {out.vertex(add(p, o))});
            keep(right_spans, i, {out.vertex(sub(p, o))});
            continue;
        }
        const std::size_t in = closed ? (i + n - 1) % n : i - 1, onward = i;
        const Vec2f n0 = nrm[in], n1 = nrm[onward];
        const f32 turn = cross(dir[in], dir[onward]);
        if (std::abs(turn) < 1e-6f && dot(dir[in], dir[onward]) > 0.0f) { // straight on
            keep(left_spans, i, {out.vertex(add(p, mul(n0, hw)))});
            keep(right_spans, i, {out.vertex(sub(p, mul(n0, hw)))});
            continue;
        }
        const f32 s = turn > 0.0f ? -1.0f : 1.0f; // the outer side: +1 left
        const Vec2f u0 = mul(n0, s), u1 = mul(n1, s);
        const Vec2f miter_dir = normalize(add(u0, u1));
        const f32 cos_half = dot(miter_dir, u0);
        const f32 miter = cos_half > 1e-4f ? hw / cos_half : 1e30f;
        // On the inside both segments meet at the inner miter, while it stays
        // within them; past that each keeps its own corner.
        const f32 reach = cos_half > 1e-4f ? miter * std::sqrt(std::max(0.0f, 1.0f - cos_half * cos_half)) : 1e30f;
        const bool inner_meets = reach <= std::min(len[in], len[onward]);
        // Outside: one shared miter where it is allowed, or where a round or
        // bevel join would differ from it by less than the tolerance.
        const bool smooth = (style.join == LineJoin::Miter && cos_half > 1e-4f && 1.0f / cos_half <= style.miter_limit) ||
                            (style.join != LineJoin::Miter && miter - hw <= tolerance);
        thread_local std::vector<u32> outer, inner;
        outer.clear();
        inner.clear();
        if (smooth) {
            outer.push_back(out.vertex(add(p, mul(miter_dir, miter))));
        } else {
            outer.push_back(out.vertex(add(p, mul(u0, hw))));
            if (style.join == LineJoin::Round) {
                Vec2f w = sub(u1, mul(u0, dot(u0, u1)));
                w = length(w) > 1e-6f ? normalize(w) : dir[in]; // straight back: round the front
                const std::vector<u32> between = arc(p, u0, w, std::atan2(dot(u1, w), dot(u1, u0)));
                outer.insert(outer.end(), between.begin(), between.end());
            }
            outer.push_back(out.vertex(add(p, mul(u1, hw))));
        }
        if (inner_meets) {
            inner.push_back(out.vertex(sub(p, mul(miter_dir, miter))));
        } else {
            inner.push_back(out.vertex(sub(p, mul(u0, hw))));
            inner.push_back(out.vertex(sub(p, mul(u1, hw))));
        }
        if (outer.size() > 1) {
            const u32 hub = inner.size() == 1 ? inner[0] : out.vertex(p);
            for (std::size_t k = 0; k + 1 < outer.size(); ++k) {
                out.tri(hub, outer[k], outer[k + 1]);
            }
        }
        keep_list(s > 0.0f ? left_spans : right_spans, i, outer);
        keep_list(s > 0.0f ? right_spans : left_spans, i, inner);
    }
    // The strip: each segment from its start's last vertices to its end's first.
    for (std::size_t k = 0; k < segments; ++k) {
        const std::size_t i = k, j = (k + 1) % n;
        out.quad(back(left_spans[i]), front(left_spans[j]), front(right_spans[j]), back(right_spans[i]));
    }
    // A side's vertices forwards, or backwards, onto `outline`.
    thread_local std::vector<u32> outline;
    outline.clear();
    const auto forwards = [&](const Side& side) {
        outline.insert(outline.end(), pool.begin() + side.first, pool.begin() + side.first + side.count);
    };
    const auto backwards = [&](const Side& side) {
        for (u32 k = side.count; k > 0; --k) {
            outline.push_back(pool[side.first + k - 1]);
        }
    };

    if (closed) {
        // Two outlines: the left forwards, the right backwards (its outside on
        // the left of travel too).
        for (const Side& side : left_spans) {
            forwards(side);
        }
        out.fringe_ring(outline, fringe);
        outline.clear();
        for (auto side = right_spans.rbegin(); side != right_spans.rend(); ++side) {
            backwards(*side);
        }
        out.fringe_ring(outline, fringe);
        return;
    }
    // Round caps: half circles fanned from the ends.
    std::vector<u32> end_cap, start_cap;
    if (style.cap == LineCap::Round) {
        const Vec2f pe = pts[n - 1], p0 = pts[0];
        end_cap = arc(pe, nrm[segments - 1], dir[segments - 1], pi);
        start_cap = arc(p0, mul(nrm[0], -1.0f), mul(dir[0], -1.0f), pi);
        const auto fan = [&](Vec2f center, u32 from, const std::vector<u32>& between, u32 to) {
            const u32 hub = out.vertex(center);
            u32 previous = from;
            for (const u32 v : between) {
                out.tri(hub, previous, v);
                previous = v;
            }
            out.tri(hub, previous, to);
        };
        fan(pe, front(left_spans[n - 1]), end_cap, front(right_spans[n - 1]));
        fan(p0, front(right_spans[0]), start_cap, front(left_spans[0]));
    }
    // One outline round it all: forwards along the left, round the end, back
    // along the right, round the start.
    for (const Side& side : left_spans) {
        forwards(side);
    }
    outline.insert(outline.end(), end_cap.begin(), end_cap.end());
    for (auto side = right_spans.rbegin(); side != right_spans.rend(); ++side) {
        backwards(*side);
    }
    outline.insert(outline.end(), start_cap.begin(), start_cap.end());
    out.fringe_ring(outline, fringe);
}

} // namespace

void ShapeMesh::clear() {
    vertices.clear();
    indices.clear();
    primitives.clear();
    runs.clear();
    bounds = {};
}

void ShapeMesh::add(const ShapePrimitive& primitive) {
    if (!runs.empty() && runs.back().primitives) {
        ++runs.back().count;
    } else {
        runs.push_back({.primitives = true, .first = static_cast<u32>(primitives.size()), .count = 1});
    }
    primitives.push_back(primitive);
    grow_bounds_by(*this, primitive_bounds(primitive));
}

void ShapeMesh::append(const ShapeMesh& other, const Affine2& transform) {
    // A mesh made by hand has no runs: all of it is one of triangles.
    std::vector<Run> other_runs = other.runs;
    if (other_runs.empty() && !other.indices.empty()) {
        other_runs.push_back({.count = static_cast<u32>(other.indices.size()),
                              .vertex_count = static_cast<u32>(other.vertices.size())});
    }
    for (Run run : other_runs) {
        if (run.primitives) {
            for (u32 i = run.first; i < run.first + run.count; ++i) {
                ShapePrimitive p = other.primitives[i];
                p.transform = transform * p.transform;
                add(p);
            }
            continue;
        }
        run.first += static_cast<u32>(indices.size());
        run.first_vertex += static_cast<u32>(vertices.size());
        runs.push_back(run);
    }
    const std::size_t first = vertices.size();
    const u32 offset = static_cast<u32>(first);
    vertices.reserve(first + other.vertices.size());
    const f32 edge_scale = std::max(scales(transform).first, 1e-6f);
    for (ShapeVertex v : other.vertices) {
        v.position = transform.apply(v.position);
        v.edge *= edge_scale;
        v.outward = mul(transform.apply_vector(v.outward), 1.0f / edge_scale);
        vertices.push_back(v);
    }
    indices.reserve(indices.size() + other.indices.size());
    for (const u32 i : other.indices) {
        indices.push_back(i + offset);
    }
    grow_bounds(*this, first);
}

void tessellate_fill(ShapeMesh& mesh, std::span<const PathContour> contours, FillRule rule, Color color, f32 fringe) {
    const std::size_t first = mesh.vertices.size();
    const std::size_t first_index = mesh.indices.size();
    std::vector<Ring> rings;
    for (const PathContour& contour : contours) {
        Ring ring{.pts = contour.points};
        clean(ring.pts, true);
        if (ring.pts.size() < 3) {
            continue;
        }
        ring.area = signed_area(ring.pts);
        if (std::abs(ring.area) < 1e-8f) {
            continue;
        }
        // Inside is on perp(travel) for a ring of positive area.
        for (std::size_t i = 0; i < ring.pts.size(); ++i) {
            const Vec2f a = ring.pts[i], b = ring.pts[(i + 1) % ring.pts.size()];
            const f32 len = length(sub(b, a));
            if (len > 1e-5f) {
                const f32 eps = std::min(len * 0.01f, 1e-3f);
                ring.probe = add(mul(add(a, b), 0.5f), mul(perp(mul(sub(b, a), 1.0f / len)), ring.area > 0.0f ? eps : -eps));
                break;
            }
        }
        rings.push_back(std::move(ring));
    }
    const auto filled = [rule](int w) { return rule == FillRule::NonZero ? w != 0 : (std::abs(w) % 2) == 1; };
    // A ring bounds the fill where the rule says one side of it is in and the
    // other out: an outline if its inside is in, a hole if its outside is.
    // (Rings are taken not to cross; crossing ones are each filled whole.)
    std::vector<std::size_t> boundaries;
    for (std::size_t i = 0; i < rings.size(); ++i) {
        int around = 0;
        for (std::size_t j = 0; j < rings.size(); ++j) {
            if (j != i) {
                around += winding_number(rings[i].probe, rings[j].pts);
            }
        }
        const bool in = filled(around + (rings[i].area > 0.0f ? 1 : -1));
        if (in != filled(around)) {
            rings[i].outer = in;
            boundaries.push_back(i);
        }
    }

    Out out{mesh, color};
    std::vector<std::vector<std::size_t>> holes(rings.size());
    for (const std::size_t h : boundaries) {
        if (rings[h].outer) {
            continue;
        }
        // The smallest outline around it.
        std::size_t parent = rings.size();
        for (const std::size_t o : boundaries) {
            if (rings[o].outer && winding_number(rings[h].probe, rings[o].pts) != 0 &&
                (parent == rings.size() || std::abs(rings[o].area) < std::abs(rings[parent].area))) {
                parent = o;
            }
        }
        if (parent != rings.size()) {
            holes[parent].push_back(h);
        }
    }
    // Each ring's vertices once: the fill's corners and its soft edge's inside.
    std::vector<std::vector<u32>> ring_vertices(rings.size());
    for (const std::size_t b : boundaries) {
        for (const Vec2f& p : rings[b].pts) {
            ring_vertices[b].push_back(out.vertex(p));
        }
    }
    std::vector<std::vector<Vec2f>> polygon;
    std::vector<u32> polygon_vertices;
    for (const std::size_t o : boundaries) {
        if (!rings[o].outer) {
            continue;
        }
        polygon.clear();
        polygon_vertices.clear();
        polygon.push_back(rings[o].pts);
        polygon_vertices.insert(polygon_vertices.end(), ring_vertices[o].begin(), ring_vertices[o].end());
        for (const std::size_t h : holes[o]) {
            polygon.push_back(rings[h].pts);
            polygon_vertices.insert(polygon_vertices.end(), ring_vertices[h].begin(), ring_vertices[h].end());
        }
        for (const u32 i : mapbox::earcut<u32>(polygon)) {
            mesh.indices.push_back(polygon_vertices[i]);
        }
    }
    // The soft edge outside each boundary, the ring turned so the fill is on
    // its right (fringe_ring's outside on the left).
    for (const std::size_t b : boundaries) {
        std::vector<u32>& ring = ring_vertices[b];
        if ((rings[b].outer && rings[b].area > 0.0f) || (!rings[b].outer && rings[b].area < 0.0f)) {
            std::reverse(ring.begin(), ring.end());
        }
        out.fringe_ring(ring, fringe);
    }
    note_triangles(mesh, first, first_index);
    grow_bounds(mesh, first);
}

void tessellate_stroke(ShapeMesh& mesh, std::span<const PathContour> contours, const StrokeStyle& style, Color color,
                       f32 fringe, f32 tolerance, const Affine2& transform) {
    const auto [min_scale, max_scale] = scales(transform);
    if (style.width <= 0.0f || min_scale < 1e-6f) {
        return;
    }
    const std::size_t first = mesh.vertices.size();
    const std::size_t first_index = mesh.indices.size();
    Out out{mesh, color};
    for (const PathContour& contour : contours) {
        stroke_contour(out, contour.points, contour.closed, style, fringe / min_scale, tolerance / max_scale);
    }
    if (!transform.is_identity()) {
        // Stroked in its own units: the edges, made `fringe / min_scale` wide,
        // become `fringe` in the mesh's.
        for (ShapeVertex& v : std::span{mesh.vertices}.subspan(first)) {
            v.position = transform.apply(v.position);
            v.edge *= min_scale;
            v.outward = mul(transform.apply_vector(v.outward), 1.0f / min_scale);
        }
    }
    note_triangles(mesh, first, first_index);
    grow_bounds(mesh, first);
}

void tessellate_primitive(ShapeMesh& mesh, const ShapePrimitive& p, f32 fringe, f32 tolerance) {
    const Rectf box{-p.half_size.x, -p.half_size.y, 2.0f * p.half_size.x, 2.0f * p.half_size.y};
    const Path path = p.kind == PathPrimitive::Kind::Ellipse ? Path::ellipse({}, p.half_size)
                                                             : Path::rounded_rect(box, p.radius);
    const auto [min_scale, max_scale] = scales(p.transform);
    std::vector<PathContour> contours;
    if (p.fill.a > 0) {
        path.flatten(contours, tolerance, p.transform);
        tessellate_fill(mesh, contours, FillRule::NonZero, p.fill, fringe);
    }
    if (p.stroke.a > 0 && p.stroke_width > 0.0f) {
        contours.clear();
        path.flatten(contours, tolerance / std::max(max_scale, 1e-6f));
        tessellate_stroke(mesh, contours,
                          {.width = p.stroke_width, .join = p.round_join ? LineJoin::Round : LineJoin::Miter},
                          p.stroke, fringe, tolerance, p.transform);
    }
}

std::optional<ShapePrimitive> primitive_of(const ShapeElement& element) {
    const std::optional<PathPrimitive>& shape = element.path.primitive();
    if (!shape) {
        return std::nullopt;
    }
    ShapePrimitive p{.kind = shape->kind,
                     .half_size = shape->half_size,
                     .radius = std::min(shape->radius, std::min(shape->half_size.x, shape->half_size.y)),
                     .transform = element.transform * shape->transform};
    if (element.fill) {
        p.fill = *element.fill;
    }
    if (element.stroke && element.stroke_style.width > 0.0f) {
        const StrokeStyle& s = element.stroke_style;
        const bool sharp = shape->kind == PathPrimitive::Kind::RoundedRect && p.radius <= 0.0f;
        // An ellipse's distance is estimated: good near its outline, not a
        // thick stroke's width away. A sharp corner's bevel is not drawn.
        if (shape->kind == PathPrimitive::Kind::Ellipse &&
            s.width > 0.5f * std::min(shape->half_size.x, shape->half_size.y)) {
            return std::nullopt;
        }
        if (sharp && (s.join == LineJoin::Bevel || (s.join == LineJoin::Miter && s.miter_limit < 1.4143f))) {
            return std::nullopt;
        }
        p.stroke = *element.stroke;
        p.stroke_width = s.width;
        p.round_join = s.join == LineJoin::Round;
    }
    return p;
}

Shape& Shape::fill(Path path, Color color, FillRule rule) {
    return add(ShapeElement{.path = std::move(path), .fill = color, .fill_rule = rule});
}

Shape& Shape::stroke(Path path, Color color, StrokeStyle style) {
    return add(ShapeElement{.path = std::move(path), .stroke = color, .stroke_style = style});
}

Shape& Shape::fill_and_stroke(Path path, Color fill, Color stroke, StrokeStyle style) {
    return add(ShapeElement{.path = std::move(path), .fill = fill, .stroke = stroke, .stroke_style = style});
}

Shape& Shape::add(ShapeElement element) {
    elements.push_back(std::move(element));
    return *this;
}

Shape& Shape::add(const Shape& other, const Affine2& transform) {
    for (ShapeElement element : other.elements) {
        element.transform = transform * element.transform;
        elements.push_back(std::move(element));
    }
    return *this;
}

Rectf Shape::bounds() const {
    bool any = false;
    f32 x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
    for (const ShapeElement& element : elements) {
        if (element.path.empty() || (!element.fill && !element.stroke)) {
            continue;
        }
        Rectf r = element.path.bounds();
        if (element.stroke) {
            const StrokeStyle& s = element.stroke_style;
            const f32 reach = s.width * 0.5f *
                              (s.join == LineJoin::Miter ? std::max(1.0f, s.miter_limit)
                               : s.cap == LineCap::Square ? 1.4143f
                                                          : 1.0f);
            r = {r.x - reach, r.y - reach, r.w + 2.0f * reach, r.h + 2.0f * reach};
        }
        r = transformed_bounds(element.transform, r);
        if (!any) {
            x0 = r.x, y0 = r.y, x1 = r.x + r.w, y1 = r.y + r.h;
            any = true;
        } else {
            x0 = std::min(x0, r.x), y0 = std::min(y0, r.y);
            x1 = std::max(x1, r.x + r.w), y1 = std::max(y1, r.y + r.h);
        }
    }
    return {x0, y0, x1 - x0, y1 - y0};
}

ShapeMesh Shape::mesh(ShapeBuildOptions options) const {
    ShapeMesh mesh;
    std::vector<PathContour> contours;
    for (const ShapeElement& element : elements) {
        if (const std::optional<ShapePrimitive> primitive = primitive_of(element)) {
            mesh.add(*primitive);
            continue;
        }
        if (element.fill) {
            contours.clear();
            element.path.flatten(contours, options.tolerance, element.transform);
            tessellate_fill(mesh, contours, element.fill_rule, *element.fill, options.fringe);
        }
        if (element.stroke && element.stroke_style.width > 0.0f) {
            contours.clear();
            element.path.flatten(contours, options.tolerance / std::max(scales(element.transform).second, 1e-6f));
            tessellate_stroke(mesh, contours, element.stroke_style, *element.stroke, options.fringe, options.tolerance,
                              element.transform);
        }
    }
    return mesh;
}

} // namespace kin
