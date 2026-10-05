#include <kin/renderer/shape.hpp>

#include <mapbox/earcut.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
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
    void tri(u32 a, u32 b, u32 c) { mesh.indices.insert(mesh.indices.end(), {a, b, c}); }
    void quad(u32 a, u32 b, u32 c, u32 d) {
        tri(a, b, c);
        tri(a, c, d);
    }
    // Triangles from `center` to each pair of neighbouring `points`.
    void fan(Vec2f center, std::span<const Vec2f> points) {
        if (points.size() < 2) {
            return;
        }
        const u32 c = vertex(center);
        u32 previous = vertex(points[0]);
        for (std::size_t i = 1; i < points.size(); ++i) {
            const u32 next = vertex(points[i]);
            tri(c, previous, next);
            previous = next;
        }
    }

    // The soft edge along a closed outline whose outside is on perp(travel):
    // from the outline (edge 0) out by `fringe` (edge -fringe), along each
    // corner's miter so the strip has the same width beside both of its edges.
    void fringe_ring(std::span<const Vec2f> pts, f32 fringe) {
        const std::size_t n = pts.size();
        if (n < 2 || fringe <= 0.0f) {
            return;
        }
        const u32 first = static_cast<u32>(mesh.vertices.size());
        for (std::size_t i = 0; i < n; ++i) {
            const Vec2f p = pts[i];
            Vec2f e0 = normalize(sub(p, pts[(i + n - 1) % n]));
            Vec2f e1 = normalize(sub(pts[(i + 1) % n], p));
            if (e0 == Vec2f{}) {
                e0 = e1;
            }
            if (e1 == Vec2f{}) {
                e1 = e0;
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
            vertex(p, 0.0f);
            vertex(add(p, mul(dm, fringe)), -fringe, dm);
        }
        for (u32 i = 0; i < n; ++i) {
            const u32 j = (i + 1) % static_cast<u32>(n);
            quad(first + 2 * i, first + 2 * j, first + 2 * j + 1, first + 2 * i + 1);
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
        const u32 first = static_cast<u32>(mesh.vertices.size());
        for (const Vec2f& p : pts) {
            vertex(p);
        }
        for (u32 i = 1; i + 1 < pts.size(); ++i) {
            tri(first, first + i, first + i + 1);
        }
        fringe_ring(pts, fringe);
    }
};

void grow_bounds(ShapeMesh& mesh, std::size_t first) {
    if (first >= mesh.vertices.size()) {
        return;
    }
    f32 x0 = mesh.vertices[first].position.x, y0 = mesh.vertices[first].position.y, x1 = x0, y1 = y0;
    if (first > 0 || mesh.bounds.w > 0.0f || mesh.bounds.h > 0.0f) {
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

void stroke_contour(Out& out, std::vector<Vec2f> pts, bool closed, const StrokeStyle& style, f32 fringe,
                    f32 tolerance) {
    clean(pts, closed);
    const f32 hw = style.width * 0.5f;
    if (pts.empty()) {
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

    const std::size_t n = pts.size();
    const std::size_t segments = closed ? n : n - 1;
    std::vector<Vec2f> dir(segments), nrm(segments);
    for (std::size_t k = 0; k < segments; ++k) {
        dir[k] = normalize(sub(pts[(k + 1) % n], pts[k]));
        nrm[k] = perp(dir[k]);
    }
    // Each segment's own rectangle.
    for (std::size_t k = 0; k < segments; ++k) {
        const Vec2f a = pts[k], b = pts[(k + 1) % n], o = mul(nrm[k], hw);
        out.quad(out.vertex(add(a, o)), out.vertex(add(b, o)), out.vertex(sub(b, o)), out.vertex(sub(a, o)));
    }

    // Each corner: the wedge on its outer side, and the outline's points on
    // both sides (in travelling order).
    std::vector<std::vector<Vec2f>> left(n), right(n);
    const f32 step = arc_step(hw, tolerance);
    const auto join = [&](std::size_t i, std::size_t in, std::size_t outgoing) {
        const Vec2f p = pts[i];
        const f32 turn = cross(dir[in], dir[outgoing]);
        if (std::abs(turn) < 1e-6f && dot(dir[in], dir[outgoing]) > 0.0f) { // straight on
            left[i] = {add(p, mul(nrm[in], hw))};
            right[i] = {sub(p, mul(nrm[in], hw))};
            return;
        }
        const f32 s = turn > 0.0f ? -1.0f : 1.0f; // the outer side
        const Vec2f a = add(p, mul(nrm[in], s * hw)), b = add(p, mul(nrm[outgoing], s * hw));
        std::vector<Vec2f> outer;
        LineJoin kind = style.join;
        Vec2f miter{};
        if (kind == LineJoin::Miter) {
            const Vec2f m = normalize(mul(add(nrm[in], nrm[outgoing]), s));
            const f32 cos_half = dot(m, mul(nrm[in], s));
            if (std::abs(turn) < 1e-6f || cos_half <= 0.0f || 1.0f / cos_half > style.miter_limit) {
                kind = LineJoin::Bevel;
            } else {
                miter = add(p, mul(m, hw / cos_half));
            }
        }
        if (kind == LineJoin::Miter) {
            outer = {a, miter, b};
        } else if (kind == LineJoin::Round) {
            const f32 from = std::atan2(a.y - p.y, a.x - p.x);
            f32 delta = std::atan2(b.y - p.y, b.x - p.x) - from;
            while (delta > pi) delta -= 2.0f * pi;
            while (delta < -pi) delta += 2.0f * pi;
            const int count = std::max(1, static_cast<int>(std::ceil(std::abs(delta) / step)));
            outer.push_back(a);
            for (int k = 1; k < count; ++k) {
                const f32 t = from + delta * static_cast<f32>(k) / static_cast<f32>(count);
                outer.push_back({p.x + hw * std::cos(t), p.y + hw * std::sin(t)});
            }
            outer.push_back(b);
        } else {
            outer = {a, b};
        }
        out.fan(p, outer);
        std::vector<Vec2f> inner{sub(p, mul(nrm[in], s * hw)), sub(p, mul(nrm[outgoing], s * hw))};
        (s > 0.0f ? left[i] : right[i]) = std::move(outer);
        (s > 0.0f ? right[i] : left[i]) = std::move(inner);
    };

    if (closed) {
        for (std::size_t i = 0; i < n; ++i) {
            join(i, (i + n - 1) % n, i);
        }
        std::vector<Vec2f> loop;
        for (const auto& side : left) {
            loop.insert(loop.end(), side.begin(), side.end());
        }
        out.fringe_ring(loop, fringe);
        loop.clear();
        for (const auto& side : right) {
            loop.insert(loop.end(), side.begin(), side.end());
        }
        std::reverse(loop.begin(), loop.end()); // travelled backwards, its outside is on the left too
        out.fringe_ring(loop, fringe);
        return;
    }

    for (std::size_t i = 1; i + 1 < n; ++i) {
        join(i, i - 1, i);
    }
    // One outline around the whole stroke: forward along the left, round the
    // end, back along the right, round the start.
    const Vec2f p0 = pts[0], pe = pts[n - 1];
    const Vec2f t0 = dir[0], n0 = nrm[0], te = dir[segments - 1], ne = nrm[segments - 1];
    const auto cap = [&](Vec2f p, Vec2f u, Vec2f t) {
        // From p + u*hw round to p - u*hw, ahead along t; returns the points between.
        std::vector<Vec2f> between;
        if (style.cap == LineCap::Square) {
            const Vec2f a = add(add(p, mul(u, hw)), mul(t, hw)), b = add(sub(p, mul(u, hw)), mul(t, hw));
            out.quad(out.vertex(add(p, mul(u, hw))), out.vertex(a), out.vertex(b), out.vertex(sub(p, mul(u, hw))));
            between = {a, b};
        } else if (style.cap == LineCap::Round) {
            const int count = std::max(2, static_cast<int>(std::ceil(pi / step)));
            std::vector<Vec2f> arc{add(p, mul(u, hw))};
            for (int k = 1; k < count; ++k) {
                const f32 a = pi * static_cast<f32>(k) / static_cast<f32>(count);
                const Vec2f q = add(p, add(mul(u, hw * std::cos(a)), mul(t, hw * std::sin(a))));
                arc.push_back(q);
                between.push_back(q);
            }
            arc.push_back(sub(p, mul(u, hw)));
            out.fan(p, arc);
        }
        return between;
    };
    std::vector<Vec2f> outline{add(p0, mul(n0, hw))};
    for (std::size_t i = 1; i + 1 < n; ++i) {
        outline.insert(outline.end(), left[i].begin(), left[i].end());
    }
    outline.push_back(add(pe, mul(ne, hw)));
    const std::vector<Vec2f> end_cap = cap(pe, ne, te);
    outline.insert(outline.end(), end_cap.begin(), end_cap.end());
    outline.push_back(sub(pe, mul(ne, hw)));
    for (std::size_t i = n - 2; i >= 1; --i) {
        outline.insert(outline.end(), right[i].rbegin(), right[i].rend());
    }
    outline.push_back(sub(p0, mul(n0, hw)));
    const std::vector<Vec2f> start_cap = cap(p0, mul(n0, -1.0f), mul(t0, -1.0f));
    outline.insert(outline.end(), start_cap.begin(), start_cap.end());
    clean(outline, true);
    out.fringe_ring(outline, fringe);
}

} // namespace

void ShapeMesh::clear() {
    vertices.clear();
    indices.clear();
    bounds = {};
}

void ShapeMesh::append(const ShapeMesh& other, const Affine2& transform) {
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
    std::vector<std::vector<Vec2f>> polygon;
    for (const std::size_t o : boundaries) {
        if (!rings[o].outer) {
            continue;
        }
        polygon.clear();
        polygon.push_back(rings[o].pts);
        for (const std::size_t h : holes[o]) {
            polygon.push_back(rings[h].pts);
        }
        const u32 base = static_cast<u32>(mesh.vertices.size());
        for (const auto& ring : polygon) {
            for (const Vec2f& p : ring) {
                out.vertex(p);
            }
        }
        for (const u32 i : mapbox::earcut<u32>(polygon)) {
            mesh.indices.push_back(base + i);
        }
    }
    // The soft edge outside each boundary: rings turned so the fill is on
    // their right (fringe_ring's outside on the left).
    for (const std::size_t b : boundaries) {
        Ring& ring = rings[b];
        if ((ring.outer && ring.area > 0.0f) || (!ring.outer && ring.area < 0.0f)) {
            std::reverse(ring.pts.begin(), ring.pts.end());
        }
        out.fringe_ring(ring.pts, fringe);
    }
    grow_bounds(mesh, first);
}

void tessellate_stroke(ShapeMesh& mesh, std::span<const PathContour> contours, const StrokeStyle& style, Color color,
                       f32 fringe, f32 tolerance, const Affine2& transform) {
    const auto [min_scale, max_scale] = scales(transform);
    if (style.width <= 0.0f || min_scale < 1e-6f) {
        return;
    }
    const std::size_t first = mesh.vertices.size();
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
    grow_bounds(mesh, first);
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
