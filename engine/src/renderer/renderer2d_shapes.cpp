#include <kin/renderer/renderer2d.hpp>

#include <algorithm>
#include <cmath>

namespace kin {

Renderer2D::ShapeDetail Renderer2D::shape_detail() const {
    // A quarter unit of curve error and one unit of soft edge, as seen after the
    // current transform: finer when it scales up, wider when it scales down.
    const Affine2& m = _transform;
    const f32 sx = std::sqrt(m.a * m.a + m.b * m.b), sy = std::sqrt(m.c * m.c + m.d * m.d);
    const f32 small = std::max(std::min(sx, sy), 1e-4f), large = std::max(std::max(sx, sy), 1e-4f);
    return {.tolerance = 0.25f / large, .fringe = 1.0f / small};
}

void Renderer2D::draw_shape(const ShapeMesh& mesh, const Affine2& transform, Color tint) {
    if (mesh.empty() || tint.a == 0) {
        return;
    }
    const std::size_t count = mesh.vertices.size();
    if (std::any_of(mesh.indices.begin(), mesh.indices.end(), [count](u32 i) { return i >= count; })) {
        return; // a broken mesh: an index past its vertices
    }
    const bool moved = !transform.is_identity();
    if (moved) {
        push_transform(transform);
    }
    _backend->draw_shape_mesh(mesh.vertices, mesh.indices, tint);
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
    fill_ellipse(center, {radius, radius}, color);
}

void Renderer2D::draw_circle(Vec2f center, f32 radius, Color color, f32 width) {
    draw_ellipse(center, {radius, radius}, color, width);
}

void Renderer2D::fill_ellipse(Vec2f center, Vec2f radii, Color color) {
    if (radii.x <= 0.0f || radii.y <= 0.0f || color.a == 0) {
        return;
    }
    _contour_scratch.clear();
    Path::ellipse(center, radii).flatten(_contour_scratch, shape_detail().tolerance);
    fill_contours(FillRule::NonZero, color);
}

void Renderer2D::draw_ellipse(Vec2f center, Vec2f radii, Color color, f32 width) {
    if (radii.x <= 0.0f || radii.y <= 0.0f || color.a == 0 || width <= 0.0f) {
        return;
    }
    _contour_scratch.clear();
    Path::ellipse(center, radii).flatten(_contour_scratch, shape_detail().tolerance);
    stroke_contours({.width = width}, color);
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
    const std::array<Vec2f, 2> points{a, b};
    draw_polyline(points, color, {.width = width, .cap = cap});
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
