#include <kin/renderer/path.hpp>
#include <kin/renderer/shape.hpp>
#include <kin/renderer/svg.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <string>
#include <vector>

namespace {

constexpr kin::f32 pi = std::numbers::pi_v<kin::f32>;

bool near(kin::f32 a, kin::f32 b, kin::f32 eps) {
    return std::abs(a - b) <= eps;
}

bool near(kin::Vec2f a, kin::Vec2f b, kin::f32 eps = 1e-3f) {
    return near(a.x, b.x, eps) && near(a.y, b.y, eps);
}

// The area of the triangles wholly inside the outline (every corner's edge 0):
// the shape's own area, its soft edge left out. Overlaps count twice.
kin::f32 core_area(const kin::ShapeMesh& mesh) {
    double area = 0.0;
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const kin::ShapeVertex& a = mesh.vertices[mesh.indices[i]];
        const kin::ShapeVertex& b = mesh.vertices[mesh.indices[i + 1]];
        const kin::ShapeVertex& c = mesh.vertices[mesh.indices[i + 2]];
        if (a.edge == 0.0f && b.edge == 0.0f && c.edge == 0.0f) {
            area += std::abs((b.position.x - a.position.x) * (c.position.y - a.position.y) -
                             (c.position.x - a.position.x) * (b.position.y - a.position.y)) * 0.5;
        }
    }
    return static_cast<kin::f32>(area);
}

// The tessellator itself (Shape::mesh keeps circles and rectangles whole).
kin::ShapeMesh fill(const kin::Path& path, kin::FillRule rule = kin::FillRule::NonZero, kin::f32 fringe = 2.0f) {
    std::vector<kin::PathContour> contours;
    path.flatten(contours, 0.25f);
    kin::ShapeMesh mesh;
    kin::tessellate_fill(mesh, contours, rule, kin::colors::white, fringe);
    return mesh;
}

// Round joins and caps finely made, so their areas match a circle's.
kin::ShapeMesh stroke(const kin::Path& path, kin::StrokeStyle style) {
    std::vector<kin::PathContour> contours;
    path.flatten(contours, 0.001f);
    kin::ShapeMesh mesh;
    kin::tessellate_stroke(mesh, contours, style, kin::colors::white, 2.0f, 0.001f);
    return mesh;
}

void expect_area(const char* what, kin::f32 got, kin::f32 want, kin::f32 eps) {
    if (!near(got, want, eps)) {
        std::fprintf(stderr, "%s: area %.3f, expected %.3f (+-%.3f)\n", what, got, want, eps);
        assert(false);
    }
}

void test_path_flattening() {
    std::vector<kin::PathContour> contours;
    kin::Path::rect({10.0f, 20.0f, 30.0f, 40.0f}).flatten(contours, 0.25f);
    assert(contours.size() == 1 && contours[0].closed && contours[0].points.size() == 4);
    assert(near(contours[0].points[2], {40.0f, 60.0f}));

    // Every flattened point of a circle lies on it, and no chord strays past the tolerance.
    contours.clear();
    kin::Path::circle({0.0f, 0.0f}, 100.0f).flatten(contours, 0.25f);
    assert(contours.size() == 1 && contours[0].points.size() > 16);
    const auto& pts = contours[0].points;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        const kin::Vec2f a = pts[i], b = pts[(i + 1) % pts.size()];
        assert(near(std::hypot(a.x, a.y), 100.0f, 0.05f));
        const kin::Vec2f mid{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
        assert(100.0f - std::hypot(mid.x, mid.y) <= 0.3f);
    }

    // Mapped by a transform before flattening.
    contours.clear();
    kin::Path::line({0.0f, 0.0f}, {10.0f, 0.0f}).flatten(contours, 0.25f, kin::Affine2::translation({5.0f, 5.0f}));
    assert(!contours[0].closed && near(contours[0].points[1], {15.0f, 5.0f}));
}

void test_svg_path_data() {
    std::vector<kin::PathContour> contours;
    const auto flat = [&](std::string_view d) {
        contours.clear();
        const std::optional<kin::Path> path = kin::Path::parse_svg(d);
        assert(path);
        path->flatten(contours, 0.1f);
        return contours;
    };
    // Relative commands, H and V, implicit repeats.
    auto c = flat("M10 10 h20 v20 h-20 z");
    assert(c.size() == 1 && c[0].closed && c[0].points.size() == 4 && near(c[0].points[2], {30.0f, 30.0f}));
    c = flat("m0,0 10,0 0,10z m20 0 l5 5");
    assert(c.size() == 2 && near(c[0].points[2], {10.0f, 10.0f}) && near(c[1].points[0], {20.0f, 0.0f}));
    // An arc sweeping the positive way (clockwise on screen) from (0,0) to (20,0) bulges up.
    c = flat("M0 0 A10 10 0 0 1 20 0");
    kin::f32 top = 0.0f;
    for (const kin::Vec2f p : c[0].points) top = std::min(top, p.y);
    assert(near(top, -10.0f, 0.1f) && near(c[0].points.back(), {20.0f, 0.0f}));
    // Arc flags run together, exponents, smooth curves.
    assert(kin::Path::parse_svg("M0 0a10 10 0 1110 10"));
    assert(kin::Path::parse_svg("M1e1-2.5L.5.5C1 2 3 4 5 6S7 8 9 10Q1 1 2 2T3 3"));
    // Malformed.
    std::string error;
    assert(!kin::Path::parse_svg("M10", &error) && !error.empty());
    assert(!kin::Path::parse_svg("M0 0 X1 1"));
    assert(!kin::Path::parse_svg("10 10"));

    // Written and read back, the same outline (path data does not say "circle").
    for (const kin::Path& path : {kin::Path::circle({3.5f, -2.25f}, 7.0f), kin::Path::star({0, 0}, 10, 4, 5, 12.5f),
                                  kin::Path::rounded_rect({0, 0, 20, 10}, 3), kin::Path::pie({0, 0}, 5, 30, 300)}) {
        const std::optional<kin::Path> back = kin::Path::parse_svg(path.to_svg());
        assert(back && std::ranges::equal(back->verbs(), path.verbs()) && std::ranges::equal(back->points(), path.points()));
    }
}

void test_fills() {
    expect_area("square", core_area(fill(kin::Path::rect({0, 0, 10, 10}))), 100.0f, 0.01f);
    expect_area("circle", core_area(fill(kin::Path::circle({0, 0}, 50))), pi * 2500.0f, pi * 2500.0f * 0.005f);
    // Concave: an L.
    const std::vector<kin::Vec2f> l{{0, 0}, {20, 0}, {20, 10}, {10, 10}, {10, 20}, {0, 20}};
    expect_area("L", core_area(fill(kin::Path::polygon(l))), 300.0f, 0.01f);
    // Holes: the same winding twice is a hole under even-odd, not under non-zero;
    // turned the other way it is a hole under both.
    kin::Path same = kin::Path::rect({0, 0, 100, 100});
    same.append(kin::Path::rect({25, 25, 50, 50}));
    expect_area("evenodd hole", core_area(fill(same, kin::FillRule::EvenOdd)), 7500.0f, 0.1f);
    expect_area("nonzero, same way", core_area(fill(same, kin::FillRule::NonZero)), 10000.0f, 0.1f);
    kin::Path opposite = kin::Path::rect({0, 0, 100, 100});
    opposite.append(kin::Path::polygon(std::vector<kin::Vec2f>{{25, 25}, {25, 75}, {75, 75}, {75, 25}}));
    expect_area("nonzero, turned", core_area(fill(opposite, kin::FillRule::NonZero)), 7500.0f, 0.1f);
    // An island in the hole is filled again.
    kin::Path island = same;
    island.append(kin::Path::rect({40, 40, 20, 20}));
    expect_area("island", core_area(fill(island, kin::FillRule::EvenOdd)), 7900.0f, 0.1f);
    // Separate outlines.
    kin::Path two = kin::Path::rect({0, 0, 10, 10});
    two.append(kin::Path::rect({20, 0, 10, 10}));
    expect_area("two squares", core_area(fill(two)), 200.0f, 0.01f);
    // A star (concave, every point an ear).
    const kin::ShapeMesh star = fill(kin::Path::star({0, 0}, 50, 20, 5));
    expect_area("star", core_area(star), 5.0f * 50.0f * 20.0f * std::sin(pi / 5.0f), 1.0f);

    // The soft edge: every vertex at 0 or -fringe, the rim outside the outline,
    // a fringe's width out (the corners along their miters).
    const kin::ShapeMesh mesh = fill(kin::Path::rect({0, 0, 10, 10}), kin::FillRule::NonZero, 3.0f);
    for (const kin::ShapeVertex& v : mesh.vertices) {
        assert(v.edge == 0.0f || v.edge == -3.0f);
        if (v.edge == -3.0f) {
            assert(near(std::abs(v.position.x - 5.0f), 8.0f, 1e-3f) && near(std::abs(v.position.y - 5.0f), 8.0f, 1e-3f));
        }
    }
    assert(near(mesh.bounds.x, -3.0f, 1e-3f) && near(mesh.bounds.w, 16.0f, 1e-3f));
    // Nothing for nothing.
    assert(fill(kin::Path::polygon(std::vector<kin::Vec2f>{{0, 0}, {10, 0}, {20, 0}})).empty());
}

void test_strokes() {
    const kin::Path line = kin::Path::line({0, 0}, {100, 0});
    expect_area("butt", core_area(stroke(line, {.width = 10})), 1000.0f, 0.01f);
    expect_area("square cap", core_area(stroke(line, {.width = 10, .cap = kin::LineCap::Square})), 1100.0f, 0.01f);
    expect_area("round cap", core_area(stroke(line, {.width = 10, .cap = kin::LineCap::Round})), 1000.0f + pi * 25.0f,
                1.0f);
    // A right-angled corner, nothing drawn twice: two 100 x 10 legs meeting at
    // the miter; a bevel cuts its tip off; a round join rounds the corner.
    const std::vector<kin::Vec2f> corner{{0, 0}, {100, 0}, {100, 100}};
    const kin::Path bend = kin::Path::polyline(corner);
    expect_area("miter", core_area(stroke(bend, {.width = 10})), 2000.0f, 0.1f);
    expect_area("bevel", core_area(stroke(bend, {.width = 10, .join = kin::LineJoin::Bevel})), 2000.0f - 12.5f, 0.1f);
    expect_area("round join", core_area(stroke(bend, {.width = 10, .join = kin::LineJoin::Round})),
                2000.0f - 25.0f + pi * 25.0f / 4.0f, 0.1f);
    // Closed: the ring between a 110 and a 90 square.
    expect_area("closed", core_area(stroke(kin::Path::rect({0, 0, 100, 100}), {.width = 10})), 4000.0f, 0.1f);
    // Short segments with a sharp turn: each keeps its own inner corner.
    const std::vector<kin::Vec2f> zig{{0, 0}, {3, 0}, {0, 2}};
    const kin::ShapeMesh tight = stroke(kin::Path::polyline(zig), {.width = 10, .join = kin::LineJoin::Round});
    assert(!tight.empty() && tight.bounds.w < 30.0f);
    // A spike past the miter limit is bevelled: no far-flung point.
    const std::vector<kin::Vec2f> spike{{0, 0}, {100, 0}, {0, 5}};
    const kin::ShapeMesh sharp = stroke(kin::Path::polyline(spike), {.width = 10});
    assert(sharp.bounds.x + sharp.bounds.w < 112.0f);
    const kin::ShapeMesh unlimited = stroke(kin::Path::polyline(spike), {.width = 10, .miter_limit = 100.0f});
    assert(unlimited.bounds.x + unlimited.bounds.w > 150.0f);
    // A lone point: a dot under a round cap, nothing under a butt.
    kin::Path dot;
    dot.move_to({5, 5}).line_to({5, 5});
    expect_area("dot", core_area(stroke(dot, {.width = 10, .cap = kin::LineCap::Round})), pi * 25.0f, 1.0f);
    assert(stroke(dot, {.width = 10}).empty());
    // Scaled with its element, as SVG does.
    kin::Shape small;
    small.stroke(line, kin::colors::white, {.width = 2});
    kin::Shape big;
    big.add(small, kin::Affine2::scaling({3.0f, 3.0f}));
    expect_area("scaled stroke", core_area(big.mesh()), 300.0f * 6.0f, 0.1f);
    // Its soft edge is a fringe wide in the mesh's units, not three.
    kin::f32 rim = 0.0f;
    for (const kin::ShapeVertex& v : big.mesh().vertices) rim = std::min(rim, v.edge);
    assert(near(rim, -2.0f, 1e-4f));
}

// Circles, ellipses and rounded rectangles are kept as primitives, to be drawn
// whole: in paths while they are nothing else, in meshes where their paint
// allows, in paint order with the triangles.
void test_primitives() {
    using Kind = kin::PathPrimitive::Kind;
    const auto prim = [](const kin::Path& p) { return p.primitive(); };
    assert(prim(kin::Path::circle({1, 2}, 3)) && prim(kin::Path::circle({1, 2}, 3))->kind == Kind::RoundedRect &&
           prim(kin::Path::circle({1, 2}, 3))->radius == 3.0f);
    assert(prim(kin::Path::ellipse({0, 0}, {4, 2}))->kind == Kind::Ellipse);
    assert(prim(kin::Path::rect({0, 0, 10, 4}))->radius == 0.0f);
    assert((prim(kin::Path::rect({0, 0, 10, 4}))->transform == kin::Affine2::translation({5, 2})));
    assert(prim(kin::Path::rounded_rect({0, 0, 10, 4}, 9))->radius == 2.0f); // at most half the shorter side
    kin::Path edited = kin::Path::circle({0, 0}, 3);
    edited.line_to({9, 9});
    assert(!edited.primitive() && !kin::Path::star({0, 0}, 5, 2, 5).primitive());
    kin::Path moved;
    moved.append(kin::Path::circle({0, 0}, 3), kin::Affine2::translation({7, 0}));
    assert(moved.primitive() && near(moved.primitive()->transform.apply({0, 0}), {7, 0}));
    kin::Path two = kin::Path::circle({0, 0}, 3);
    two.append(kin::Path::circle({9, 0}, 3));
    assert(!two.primitive());

    // A mesh: the star as triangles between two primitives, in order.
    kin::Shape shape;
    shape.fill_and_stroke(kin::Path::circle({0, 0}, 10), kin::colors::white, kin::colors::black, {.width = 2})
        .fill(kin::Path::star({0, 0}, 8, 3, 5), kin::Color::rgb(255, 0, 0))
        .stroke(kin::Path::rect({-20, -20, 40, 40}), kin::colors::black, {.width = 1});
    const kin::ShapeMesh mesh = shape.mesh();
    assert(mesh.primitives.size() == 2 && mesh.runs.size() == 3);
    assert(mesh.runs[0].primitives && !mesh.runs[1].primitives && mesh.runs[2].primitives);
    assert(mesh.primitives[0].fill.a == 255 && mesh.primitives[0].stroke_width == 2.0f);
    assert(mesh.primitives[1].fill.a == 0 && mesh.primitives[1].radius == 0.0f && !mesh.primitives[1].round_join);
    assert(near(mesh.bounds.x, -20.5f, 1e-3f) && near(mesh.bounds.w, 41.0f, 1e-3f));
    // What cannot be drawn whole is tessellated: bevelled corners, thick
    // strokes on ellipses.
    kin::Shape bevel;
    bevel.stroke(kin::Path::rect({0, 0, 10, 10}), kin::colors::white, {.width = 2, .join = kin::LineJoin::Bevel});
    assert(bevel.mesh().primitives.empty() && !bevel.mesh().indices.empty());
    kin::Shape thick;
    thick.stroke(kin::Path::ellipse({0, 0}, {10, 4}), kin::colors::white, {.width = 3});
    assert(thick.mesh().primitives.empty());
    // Appended, with the transform.
    kin::ShapeMesh two_meshes = mesh;
    two_meshes.append(mesh, kin::Affine2::translation({100, 0}));
    // (Primitives after primitives join one run: 5 runs, not 6.)
    assert(two_meshes.primitives.size() == 4 && two_meshes.runs.size() == 5);
    assert(two_meshes.runs[2].primitives && two_meshes.runs[2].count == 2);
    assert(near(two_meshes.primitives[2].transform.apply({0, 0}), {100, 0}));
    assert(two_meshes.runs[3].first == static_cast<kin::u32>(mesh.indices.size()) &&
           two_meshes.runs[3].first_vertex == static_cast<kin::u32>(mesh.vertices.size()));
    // As triangles (backends without them), a circle is a circle.
    kin::ShapeMesh tessellated;
    kin::tessellate_primitive(tessellated, mesh.primitives[0], 1.0f, 0.001f);
    expect_area("tessellated circle fill and stroke", core_area(tessellated), pi * 100.0f + 2.0f * pi * 10.0f * 2.0f, 1.0f);
}

void test_composition() {
    kin::Shape dot;
    dot.fill(kin::Path::circle({0, 0}, 5), kin::Color::rgb(255, 0, 0));
    kin::Shape pair;
    pair.add(dot, kin::Affine2::translation({10, 0})).add(dot, kin::Affine2::translation({30, 0}));
    kin::Shape row;
    row.add(pair).add(pair, kin::Affine2::translation({0, 20}));
    assert(row.elements.size() == 4);
    assert((row.elements[3].transform == kin::Affine2::translation({30, 20})));
    const kin::Rectf b = row.bounds();
    assert(near(b.x, 5.0f, 1e-3f) && near(b.y, -5.0f, 1e-3f) && near(b.w, 30.0f, 1e-3f) && near(b.h, 30.0f, 1e-3f));
    // Four dots, each a circle drawn whole, where they were placed.
    const kin::ShapeMesh dots = row.mesh();
    assert(dots.primitives.size() == 4 && dots.indices.empty());
    assert(near(dots.primitives[3].transform.apply({0, 0}), {30, 20}) && dots.primitives[3].radius == 5.0f);
    // Meshes compose too.
    kin::ShapeMesh twice = dot.mesh();
    twice.append(dot.mesh(), kin::Affine2::translation({100, 0}));
    assert(twice.primitives.size() == 2 && twice.bounds.x + twice.bounds.w > 104.0f);
}

void test_svg_reading() {
    const char* svg = R"svg(<?xml version="1.0" encoding="UTF-8"?>
<!-- an editor's export -->
<!DOCTYPE svg PUBLIC "-//W3C//DTD SVG 1.1//EN" "http://www.w3.org/Graphics/SVG/1.1/DTD/svg11.dtd">
<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" viewBox="0 0 200 100" width="400px" height="200px">
  <defs>
    <linearGradient id="sky"><stop offset="0" stop-color="#fff"/></linearGradient>
    <circle id="dot" r="4" fill="#ff0000"/>
  </defs>
  <title>Test &amp; things</title>
  <g id="body" transform="translate(10,20) scale(2)" fill="#00ff00" stroke="blue" stroke-width="3">
    <rect id="box" x="0" y="0" width="10" height="5" rx="1"/>
    <circle cx="20" cy="0" r="3" style="fill:rgba(0,0,255,0.5); stroke:none"/>
    <polygon points="0,0 5,0 5,5" fill-opacity="0.5" stroke-linejoin="round"/>
  </g>
  <line x1="0" y1="0" x2="10" y2="10" stroke="#123" stroke-linecap="square"/>
  <polyline points="0 0 10 0 10 10" fill="none" stroke="currentColor" color="orange"/>
  <ellipse cx="50" cy="50" rx="10" ry="5" fill="url(#sky)"/>
  <use xlink:href="#dot" x="100" y="50"/>
  <path d="M0 0 L10 0 L10 10 Z" fill-rule="evenodd" opacity="0.5" transform="rotate(90 5 5)"/>
  <text x="0" y="0">ignored</text>
  <g display="none"><rect width="10" height="10"/></g>
</svg>)svg";
    std::vector<std::string> warnings;
    std::string error;
    const std::optional<kin::Shape> shape = kin::read_svg(svg, &error, &warnings);
    assert(shape);
    assert((shape->view_box == kin::Rectf{0, 0, 200, 100}));
    const auto& e = shape->elements;
    // box, circle, polygon, line, polyline, (ellipse unpainted: dropped), use'd dot, path
    assert(e.size() == 7);
    // Inherited fill and stroke, the group's transform.
    assert(e[0].id == "box" && e[0].fill && e[0].fill->g == 255 && e[0].stroke && e[0].stroke->b == 255);
    assert(e[0].stroke_style.width == 3.0f);
    assert((e[0].transform.apply({1, 1}) == kin::Vec2f{12, 22}));
    // style="" wins over the group's attributes.
    assert(e[1].fill && e[1].fill->b == 255 && e[1].fill->a == 128 && !e[1].stroke);
    assert(e[2].fill && e[2].fill->a == 128 && e[2].stroke_style.join == kin::LineJoin::Round);
    // A line is never filled.
    assert(!e[3].fill && e[3].stroke && e[3].stroke->r == 0x11 && e[3].stroke_style.cap == kin::LineCap::Square);
    // currentColor is the color property.
    assert(!e[4].fill && e[4].stroke && e[4].stroke->r == 255 && e[4].stroke->g == 165);
    // <use> places the referenced element.
    assert(e[5].fill && e[5].fill->r == 255);
    assert(near(e[5].transform.apply({0, 0}), {100, 50}));
    // Opacity, fill-rule, rotate about a point.
    assert(e[6].fill->a == 128 && e[6].fill_rule == kin::FillRule::EvenOdd);
    assert(near(e[6].transform.apply({0, 0}), {10, 0}));
    // What was left out is said.
    const auto said = [&](std::string_view part) {
        for (const std::string& w : warnings) {
            if (w.find(part) != std::string::npos) return true;
        }
        return false;
    };
    assert(said("gradient") && said("<text>"));

    // Not SVG.
    assert(!kin::read_svg("<html></html>", &error) && error.find("<html>") != std::string::npos);
    assert(!kin::read_svg("<svg><g></svg>", &error));
    assert(!kin::read_svg("plain text", &error));
}

void test_svg_round_trip() {
    kin::Shape shape;
    shape.fill(kin::Path::star({0, 0}, 10, 4, 5), kin::Color::rgba(255, 128, 0, 200), kin::FillRule::EvenOdd)
        .stroke(kin::Path::circle({20, 0}, 6), kin::Color::rgb(10, 20, 30),
                {.width = 2.5f, .join = kin::LineJoin::Bevel, .cap = kin::LineCap::Round, .miter_limit = 7.0f})
        .fill_and_stroke(kin::Path::rounded_rect({-5, -5, 10, 10}, 2), kin::colors::white, kin::colors::black);
    shape.elements.back().transform = kin::Affine2::rotation(30.0f) * kin::Affine2::scaling({2, 0.5f});
    shape.elements.back().id = "a \"quoted\" & <odd> id";
    const std::string svg = kin::write_svg(shape);
    std::vector<std::string> warnings;
    const std::optional<kin::Shape> back = kin::read_svg(svg, nullptr, &warnings);
    assert(back && warnings.empty() && back->elements.size() == shape.elements.size());
    // A circle and a rectangle write as <circle> and <rect> and read back as primitives.
    assert(svg.find("<circle") != std::string::npos && svg.find("<rect") != std::string::npos);
    for (std::size_t i = 0; i < shape.elements.size(); ++i) {
        const kin::ShapeElement& a = shape.elements[i];
        const kin::ShapeElement& b = back->elements[i];
        assert(a.path == b.path);
        assert(a.fill.has_value() == b.fill.has_value() && a.stroke.has_value() == b.stroke.has_value());
        if (a.fill) {
            assert(a.fill->r == b.fill->r && a.fill->g == b.fill->g && a.fill->b == b.fill->b && a.fill->a == b.fill->a);
        }
        if (a.stroke) {
            assert(a.stroke->r == b.stroke->r && a.stroke->a == b.stroke->a && a.stroke_style == b.stroke_style);
        }
        assert(a.fill_rule == b.fill_rule && a.transform == b.transform && a.id == b.id);
    }
    assert(back->mesh().vertices.size() == shape.mesh().vertices.size());
}

} // namespace

int main() {
    test_path_flattening();
    test_svg_path_data();
    test_fills();
    test_strokes();
    test_primitives();
    test_composition();
    test_svg_reading();
    test_svg_round_trip();
    return 0;
}
