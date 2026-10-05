#pragma once

// Vector shapes: paths filled and stroked, composed into reusable shapes, and
// tessellated into anti-aliased triangles that Renderer2D::draw_shape draws
// through any transform. kin/renderer/svg.hpp reads and writes them as SVG.
//
//   kin::Shape ship;
//   ship.fill(kin::Path::polygon(hull), kin::colors::white)
//       .stroke(kin::Path::circle({0, 0}, 12), kin::Color::rgb(255, 200, 0), {.width = 2});
//   kin::Shape fleet;
//   fleet.add(ship, kin::Affine2::translation({40, 0})).add(ship, kin::Affine2::translation({80, 0}));
//   const kin::ShapeMesh mesh = fleet.mesh();   // once
//   renderer.draw_shape(mesh, kin::Affine2::translation(pos));   // every frame

#include <kin/core/affine.hpp>
#include <kin/core/types.hpp>
#include <kin/renderer/color.hpp>
#include <kin/renderer/path.hpp>

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace kin {

// A vertex of a tessellated shape. `edge` is how far outside the outline it
// lies, in the mesh's units, negated: 0 on and inside the outline, -fringe at
// the soft edge's rim. Drawn, the shape fades out over one screen pixel past
// its outline, at any scale. `outward` is how the vertex moves per unit of
// edge given up (zero inside the outline): backends that cannot measure pixels
// in a shader pull the soft edge in to one pixel with it.
struct ShapeVertex {
    Vec2f position{};
    Color color = colors::white;
    f32 edge = 0.0f;
    Vec2f outward{};
};

// A rounded rectangle (a circle, a capsule, a sharp rectangle) or an ellipse,
// filled and/or stroked, drawn analytically: one quad, its outline computed
// per pixel (on backends that can; triangles elsewhere). Centred on the origin
// of `transform`, which may turn, scale and skew it.
struct ShapePrimitive {
    PathPrimitive::Kind kind = PathPrimitive::Kind::RoundedRect;
    Vec2f half_size{};
    f32 radius = 0.0f;                   // RoundedRect: corner radius, at most the smaller half size
    Color fill = colors::transparent;    // alpha 0: not filled
    Color stroke = colors::transparent;  // alpha 0, or no width: not stroked
    f32 stroke_width = 0.0f;             // centred on the outline
    bool round_join = false;             // a sharp rectangle's stroke: round outer corners, not mitred
    Affine2 transform{};
};

// Shapes ready to draw (Renderer2D::draw_shape): triangles, and primitives
// drawn whole, in the order they were added (`runs`).
struct ShapeMesh {
    std::vector<ShapeVertex> vertices;
    std::vector<u32> indices; // three a triangle
    std::vector<ShapePrimitive> primitives;
    // A stretch of triangles (vertices and the indices into them), or of
    // primitives. A mesh made by hand with no runs is all one stretch of triangles.
    struct Run {
        bool primitives = false;
        u32 first = 0;  // first index, or first primitive
        u32 count = 0;  // indices, or primitives
        u32 first_vertex = 0, vertex_count = 0; // the vertices a run of triangles uses
    };
    std::vector<Run> runs;
    Rectf bounds{}; // around everything, soft edges included

    bool empty() const { return indices.empty() && primitives.empty(); }
    void clear();
    void add(const ShapePrimitive& primitive);
    // Appends `other`, mapped by `transform` (edges scaled with it).
    void append(const ShapeMesh& other, const Affine2& transform = {});
};

struct ShapeBuildOptions {
    // How far a flattened curve may stray from the true one, in shape units.
    f32 tolerance = 0.25f;
    // The soft edge's width in shape units. It must be at least a pixel at the
    // smallest scale the mesh is drawn at: 2 covers down to half size.
    f32 fringe = 2.0f;
};

// What one SVG element draws: a path, filled and/or stroked, placed by a transform.
struct ShapeElement {
    Path path;
    std::optional<Color> fill;
    FillRule fill_rule = FillRule::NonZero;
    std::optional<Color> stroke;
    StrokeStyle stroke_style{};
    Affine2 transform{};
    std::string id{};
};

class Shape {
public:
    Shape& fill(Path path, Color color, FillRule rule = FillRule::NonZero);
    Shape& stroke(Path path, Color color, StrokeStyle style = {});
    Shape& fill_and_stroke(Path path, Color fill, Color stroke, StrokeStyle style = {});
    Shape& add(ShapeElement element);
    // `other`'s elements, placed by `transform`: shapes are made of shapes.
    Shape& add(const Shape& other, const Affine2& transform = {});

    bool empty() const { return elements.empty(); }
    // Around what the elements paint, strokes included.
    Rectf bounds() const;
    ShapeMesh mesh(ShapeBuildOptions options = {}) const;

    std::vector<ShapeElement> elements;
    // The area the shape is drawn for (SVG's viewBox); empty: its bounds.
    Rectf view_box{};
};

// The tessellator Shape::mesh and Renderer2D's shape drawing use, for building
// meshes piece by piece. Each call appends to `mesh` (and grows its bounds).
// Fills: contours in mesh units (closed whether or not they say so).
void tessellate_fill(ShapeMesh& mesh, std::span<const PathContour> contours, FillRule rule, Color color,
                     f32 fringe);
// Strokes: contours in the stroke's own units, mapped by `transform` once
// stroked, so a scaled stroke widens as SVG's does. `fringe` and `tolerance`
// are in mesh units.
void tessellate_stroke(ShapeMesh& mesh, std::span<const PathContour> contours, const StrokeStyle& style, Color color,
                       f32 fringe, f32 tolerance, const Affine2& transform = {});
// A primitive as triangles, for backends that cannot draw it whole.
void tessellate_primitive(ShapeMesh& mesh, const ShapePrimitive& primitive, f32 fringe, f32 tolerance);
// The primitive an element draws as, when its path is one and its paint and
// stroke can be drawn analytically (thick strokes on ellipses and bevelled
// rectangle corners cannot).
std::optional<ShapePrimitive> primitive_of(const ShapeElement& element);

} // namespace kin
