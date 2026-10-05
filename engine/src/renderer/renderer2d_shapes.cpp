#include <kin/renderer/renderer2d.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <utility>

#ifdef KIN_ENABLE_RENDER_PROBE
#define KIN_TRACE_DRAW(...)          \
    do {                             \
        if (active_draw_trace()) {   \
            trace_draw(__VA_ARGS__); \
        }                            \
    } while (false)
#else
#define KIN_TRACE_DRAW(...) \
    do {                    \
    } while (false)
#endif

namespace kin {

namespace {

ShapePrimitive circle(Vec2f center, f32 radius) {
    return {.half_size = {radius, radius}, .radius = radius, .transform = Affine2::translation(center)};
}

ShapePrimitive ellipse(Vec2f center, Vec2f radii) {
    return {.kind = PathPrimitive::Kind::Ellipse, .half_size = radii, .transform = Affine2::translation(center)};
}

// What a primitive covers, before the renderer's transform.
Rectf reach(const ShapePrimitive& p) {
    const f32 r = p.stroke.a > 0 ? p.stroke_width * 0.5f : 0.0f;
    return transformed_bounds(p.transform, {-p.half_size.x - r, -p.half_size.y - r, 2.0f * (p.half_size.x + r),
                                            2.0f * (p.half_size.y + r)});
}

} // namespace

Renderer2D::ShapeDetail Renderer2D::shape_detail() const {
    // A quarter unit of curve error and one unit of soft edge, as seen after the
    // current transform: finer when it scales up, wider when it scales down.
    const Affine2& m = _transform;
    const f32 sx = std::sqrt(m.a * m.a + m.b * m.b), sy = std::sqrt(m.c * m.c + m.d * m.d);
    const f32 small = std::max(std::min(sx, sy), 1e-4f), large = std::max(std::max(sx, sy), 1e-4f);
    return {.tolerance = 0.25f / large, .fringe = 1.0f / small};
}

void Renderer2D::draw_shape_triangles(const ShapeMesh& mesh, const ShapeMesh::Run& run, Color tint) {
    if (run.count == 0 || std::size_t{run.first} + run.count > mesh.indices.size() ||
        std::size_t{run.first_vertex} + run.vertex_count > mesh.vertices.size()) {
        return; // a broken mesh
    }
    const std::span<const u32> indices{mesh.indices.data() + run.first, run.count};
    if (std::any_of(indices.begin(), indices.end(),
                    [&](u32 i) { return i < run.first_vertex || i - run.first_vertex >= run.vertex_count; })) {
        return; // an index past its run's vertices
    }
    _backend->draw_shape_mesh({mesh.vertices.data() + run.first_vertex, run.vertex_count}, indices, run.first_vertex,
                              tint);
}

void Renderer2D::draw_primitives(std::span<const ShapePrimitive> primitives, Color tint) {
    for (const ShapePrimitive& p : primitives) {
        KIN_TRACE_DRAW(p.fill.a > 0 ? DrawKind::Fill : DrawKind::Outline, reach(p), p.fill.a > 0 ? p.fill : p.stroke);
    }
    if (_backend->draw_shape_primitives(primitives, tint)) {
        return;
    }
    // Tessellated here, a soft edge a pixel wide under the current transform.
    const ShapeDetail detail = shape_detail();
    _primitive_scratch.clear();
    for (const ShapePrimitive& p : primitives) {
        tessellate_primitive(_primitive_scratch, p, detail.fringe, detail.tolerance);
    }
    for (const ShapeMesh::Run& run : _primitive_scratch.runs) {
        draw_shape_triangles(_primitive_scratch, run, tint);
    }
}

void Renderer2D::draw_shape(const ShapeMesh& mesh, const Affine2& transform, Color tint) {
    if (mesh.empty() || tint.a == 0) {
        return;
    }
    const bool moved = !transform.is_identity();
    if (moved) {
        push_transform(transform);
    }
    if (mesh.runs.empty()) { // made by hand: all one run of triangles
        KIN_TRACE_DRAW(DrawKind::Fill, mesh.bounds, tint);
        draw_shape_triangles(mesh,
                             {.count = static_cast<u32>(mesh.indices.size()),
                              .vertex_count = static_cast<u32>(mesh.vertices.size())},
                             tint);
    } else {
        bool traced = false; // the triangles once, as one draw
        for (const ShapeMesh::Run& run : mesh.runs) {
            if (run.primitives) {
                if (std::size_t{run.first} + run.count <= mesh.primitives.size()) {
                    draw_primitives({mesh.primitives.data() + run.first, run.count}, tint);
                }
                continue;
            }
            if (!std::exchange(traced, true)) {
                KIN_TRACE_DRAW(DrawKind::Fill, mesh.bounds, tint);
            }
            draw_shape_triangles(mesh, run, tint);
        }
    }
    if (moved) {
        pop_transform();
    }
}

void Renderer2D::fill_contours(FillRule rule, Color color) {
    _shape_scratch.clear();
    tessellate_fill(_shape_scratch, _contour_scratch, rule, color, shape_detail().fringe);
    draw_shape(_shape_scratch);
}

void Renderer2D::stroke_contours(const StrokeStyle& style, Color color) {
    _shape_scratch.clear();
    const ShapeDetail detail = shape_detail();
    tessellate_stroke(_shape_scratch, _contour_scratch, style, color, detail.fringe, detail.tolerance);
    draw_shape(_shape_scratch);
}

void Renderer2D::fill_circle(Vec2f center, f32 radius, Color color) {
    if (radius <= 0.0f || color.a == 0) {
        return;
    }
    ShapePrimitive p = circle(center, radius);
    p.fill = color;
    draw_primitives({&p, 1}, colors::white);
}

void Renderer2D::draw_circle(Vec2f center, f32 radius, Color color, f32 width) {
    if (radius <= 0.0f || color.a == 0 || width <= 0.0f) {
        return;
    }
    ShapePrimitive p = circle(center, radius);
    p.stroke = color;
    p.stroke_width = width;
    draw_primitives({&p, 1}, colors::white);
}

void Renderer2D::fill_ellipse(Vec2f center, Vec2f radii, Color color) {
    if (radii.x <= 0.0f || radii.y <= 0.0f || color.a == 0) {
        return;
    }
    ShapePrimitive p = radii.x == radii.y ? circle(center, radii.x) : ellipse(center, radii);
    p.fill = color;
    draw_primitives({&p, 1}, colors::white);
}

void Renderer2D::draw_ellipse(Vec2f center, Vec2f radii, Color color, f32 width) {
    if (radii.x <= 0.0f || radii.y <= 0.0f || color.a == 0 || width <= 0.0f) {
        return;
    }
    if (radii.x != radii.y && width > 0.5f * std::min(radii.x, radii.y)) {
        // Too thick for the ellipse's estimated distance: tessellated.
        _contour_scratch.clear();
        Path::ellipse(center, radii).flatten(_contour_scratch, shape_detail().tolerance);
        stroke_contours({.width = width}, color);
        return;
    }
    ShapePrimitive p = radii.x == radii.y ? circle(center, radii.x) : ellipse(center, radii);
    p.stroke = color;
    p.stroke_width = width;
    draw_primitives({&p, 1}, colors::white);
}

void Renderer2D::fill_polygon(std::span<const Vec2f> points, Color color) {
    if (points.size() < 3 || color.a == 0) {
        return;
    }
    _contour_scratch.assign(1, PathContour{.points = {points.begin(), points.end()}, .closed = true});
    fill_contours(FillRule::NonZero, color);
}

void Renderer2D::draw_polygon(std::span<const Vec2f> points, Color color, StrokeStyle style) {
    if (points.size() < 2 || color.a == 0) {
        return;
    }
    _contour_scratch.assign(1, PathContour{.points = {points.begin(), points.end()}, .closed = true});
    stroke_contours(style, color);
}

void Renderer2D::draw_polyline(std::span<const Vec2f> points, Color color, StrokeStyle style) {
    if (points.empty() || color.a == 0) {
        return;
    }
    _contour_scratch.assign(1, PathContour{.points = {points.begin(), points.end()}, .closed = false});
    stroke_contours(style, color);
}

void Renderer2D::draw_line(Vec2f a, Vec2f b, Color color, f32 width, LineCap cap) {
    if (color.a == 0 || width <= 0.0f) {
        return;
    }
    // A rectangle along the line (a capsule with round caps), drawn whole.
    const f32 hw = width * 0.5f;
    const Vec2f d{b.x - a.x, b.y - a.y};
    const f32 length = std::sqrt(d.x * d.x + d.y * d.y);
    if (length <= 1e-6f && cap == LineCap::Butt) {
        return;
    }
    const f32 run_on = cap == LineCap::Butt ? 0.0f : hw;
    const f32 degrees = length > 1e-6f ? std::atan2(d.y, d.x) * 180.0f / std::numbers::pi_v<f32> : 0.0f;
    const ShapePrimitive p{.half_size = {length * 0.5f + run_on, hw},
                           .radius = cap == LineCap::Round ? hw : 0.0f,
                           .fill = color,
                           .transform = Affine2::translation({(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f}) *
                                        Affine2::rotation(degrees)};
    draw_primitives({&p, 1}, colors::white);
}

void Renderer2D::draw_arc(Vec2f center, f32 radius, f32 start, f32 end, Color color, StrokeStyle style) {
    if (radius <= 0.0f || color.a == 0) {
        return;
    }
    _contour_scratch.clear();
    Path::arc(center, radius, start, end).flatten(_contour_scratch, shape_detail().tolerance);
    stroke_contours(style, color);
}

void Renderer2D::fill_pie(Vec2f center, f32 radius, f32 start, f32 end, Color color) {
    if (radius <= 0.0f || color.a == 0) {
        return;
    }
    _contour_scratch.clear();
    Path::pie(center, radius, start, end).flatten(_contour_scratch, shape_detail().tolerance);
    fill_contours(FillRule::NonZero, color);
}

void Renderer2D::fill_path(const Path& path, Color color, FillRule rule) {
    if (path.empty() || color.a == 0) {
        return;
    }
    _contour_scratch.clear();
    path.flatten(_contour_scratch, shape_detail().tolerance);
    fill_contours(rule, color);
}

void Renderer2D::stroke_path(const Path& path, Color color, StrokeStyle style) {
    if (path.empty() || color.a == 0) {
        return;
    }
    _contour_scratch.clear();
    path.flatten(_contour_scratch, shape_detail().tolerance);
    stroke_contours(style, color);
}

} // namespace kin
