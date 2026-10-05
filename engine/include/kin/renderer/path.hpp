#pragma once

// Vector paths: lines and curves, flattened to polylines for shapes
// (kin/renderer/shape.hpp). Coordinates are y down like the rest of 2D, angles
// in degrees, clockwise. SVG path data ("M0 0 L10 0 Z") reads and writes.

#include <kin/core/affine.hpp>
#include <kin/core/types.hpp>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kin {

// Which parts of overlapping and nested outlines are inside (SVG's fill-rule).
enum class FillRule : u8 { NonZero, EvenOdd };
enum class LineJoin : u8 { Miter, Round, Bevel };
enum class LineCap : u8 { Butt, Round, Square };

struct StrokeStyle {
    f32 width = 1.0f;
    LineJoin join = LineJoin::Miter;
    LineCap cap = LineCap::Butt;
    // A miter longer than this many widths is drawn as a bevel (SVG's default).
    f32 miter_limit = 4.0f;

    friend bool operator==(const StrokeStyle&, const StrokeStyle&) = default;
};

// A flattened subpath: its points, and whether it closes back to the first.
struct PathContour {
    std::vector<Vec2f> points;
    bool closed = false;
};

class Path {
public:
    enum class Verb : u8 { Move, Line, Quad, Cubic, Close };

    // A line, quad or cubic without a move first starts from the end of the
    // last subpath (or the origin).
    Path& move_to(Vec2f p);
    Path& line_to(Vec2f p);
    Path& quad_to(Vec2f control, Vec2f p);
    Path& cubic_to(Vec2f control1, Vec2f control2, Vec2f p);
    // SVG's elliptical arc to `p`: radii, the ellipse's rotation in degrees, and
    // which of the four possible arcs. Stored as cubics.
    Path& arc_to(Vec2f radii, f32 rotation, bool large_arc, bool sweep, Vec2f p);
    Path& close();
    // `other`'s subpaths, mapped by `transform`.
    Path& append(const Path& other, const Affine2& transform = {});

    static Path rect(Rectf rect);
    static Path rounded_rect(Rectf rect, f32 radius); // radius at most half the shorter side
    static Path circle(Vec2f center, f32 radius);
    static Path ellipse(Vec2f center, Vec2f radii);
    static Path line(Vec2f a, Vec2f b);
    static Path polyline(std::span<const Vec2f> points); // open
    static Path polygon(std::span<const Vec2f> points);  // closed
    // Part of a circle from `start` to `end` degrees (0 is +x, clockwise), open;
    // a pie closes it through the centre.
    static Path arc(Vec2f center, f32 radius, f32 start, f32 end);
    static Path pie(Vec2f center, f32 radius, f32 start, f32 end);
    // A first point straight up from the centre, turned by `rotation` degrees.
    static Path regular_polygon(Vec2f center, f32 radius, i32 sides, f32 rotation = 0.0f);
    static Path star(Vec2f center, f32 outer_radius, f32 inner_radius, i32 points, f32 rotation = 0.0f);

    bool empty() const { return _verbs.empty(); }
    std::span<const Verb> verbs() const { return _verbs; }
    std::span<const Vec2f> points() const { return _points; }
    // The box around the points, control points included: the path is inside.
    Rectf bounds() const;
    Path transformed(const Affine2& transform) const;

    // The path as polylines, mapped by `transform`, with curves split until no
    // point is more than `tolerance` from the curve (in the mapped units).
    void flatten(std::vector<PathContour>& out, f32 tolerance, const Affine2& transform = {}) const;

    // SVG path data: every command, absolute and relative. Null when malformed.
    static std::optional<Path> parse_svg(std::string_view data, std::string* error = nullptr);
    std::string to_svg() const;

    friend bool operator==(const Path&, const Path&) = default;

private:
    void ensure_start();
    std::vector<Verb> _verbs;
    std::vector<Vec2f> _points;
    Vec2f _subpath_start{};
};

} // namespace kin
