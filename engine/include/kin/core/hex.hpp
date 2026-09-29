#pragma once

#include <kin/core/types.hpp>

#include <array>
#include <vector>

namespace kin {

// Hex grids in axial coordinates (q, r); the third cube coordinate is s = -q - r.
// Screen conventions throughout: x grows right, y grows down.
struct Hex {
    i32 q = 0;
    i32 r = 0;

    constexpr i32 s() const { return -q - r; }

    friend constexpr bool operator==(Hex, Hex) = default;
    friend constexpr Hex operator+(Hex a, Hex b) { return {a.q + b.q, a.r + b.r}; }
    friend constexpr Hex operator-(Hex a, Hex b) { return {a.q - b.q, a.r - b.r}; }
    friend constexpr Hex operator*(Hex a, i32 k) { return {a.q * k, a.r * k}; }
};

// The six neighbour directions. Each index is 60 degrees counterclockwise (on
// screen) from the previous one; direction d + 3 is the opposite of d. For
// pointy-top hexes they are east, north-east, north-west, west, south-west and
// south-east; for flat-top hexes, south-east, north-east, north, north-west,
// south-west and south.
inline constexpr std::array<Hex, 6> HexDirections{{
    {1, 0}, {1, -1}, {0, -1}, {-1, 0}, {-1, 1}, {0, 1},
}};

// Wraps any integer to a direction index 0..5.
constexpr i32 hex_wrap_direction(i32 dir) {
    return ((dir % 6) + 6) % 6;
}

constexpr Hex hex_neighbor(Hex h, i32 dir) {
    return h + HexDirections[static_cast<std::size_t>(hex_wrap_direction(dir))];
}

constexpr std::array<Hex, 6> hex_neighbors(Hex h) {
    std::array<Hex, 6> out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = h + HexDirections[i];
    }
    return out;
}

// Number of steps between two hexes.
constexpr i32 hex_distance(Hex a, Hex b) {
    const Hex d = a - b;
    const i32 dq = d.q < 0 ? -d.q : d.q;
    const i32 dr = d.r < 0 ? -d.r : d.r;
    const i32 ds = d.s() < 0 ? -d.s() : d.s();
    return (dq + dr + ds) / 2;
}

// 60-degree rotations about the origin, clockwise and counterclockwise on screen.
// Rotate about another hex c with hex_rotate_cw(h - c) + c.
constexpr Hex hex_rotate_cw(Hex h) {
    return {-h.r, h.q + h.r};
}

constexpr Hex hex_rotate_ccw(Hex h) {
    return {h.q + h.r, -h.q};
}

// Nearest hex to a fractional axial coordinate.
Hex hex_round(f32 q, f32 r);

// The hexes exactly `radius` steps from `center`, walking counterclockwise.
// Radius 0 gives just the center; a negative radius gives nothing.
std::vector<Hex> hex_ring(Hex center, i32 radius);

// Every hex within `radius` steps of `center` (3 * radius * (radius + 1) + 1 of
// them), ordered by q, then r.
std::vector<Hex> hex_range(Hex center, i32 radius);

// The hexes a straight line from `a` to `b` passes through, both ends included.
std::vector<Hex> hex_line(Hex a, Hex b);

enum class HexOrientation : u8 {
    PointyTop, // a corner points up; hexes form horizontal rows
    FlatTop,   // an edge is on top; hexes form vertical columns
};

// Which rows (pointy-top) or columns (flat-top) sit half a hex further right or
// down in offset (col, row) coordinates.
enum class HexOffset : u8 {
    Odd,
    Even,
};

// Maps hexes to pixels and to rectangular offset cells.
struct HexLayout {
    HexOrientation orientation = HexOrientation::PointyTop;
    // Center-to-corner radius in pixels, per axis; unequal values squash the hexes.
    Vec2f size{16.0f, 16.0f};
    // Pixel position of hex (0, 0)'s center.
    Vec2f origin{};
    HexOffset offset = HexOffset::Odd;

    Vec2f to_pixel(Hex h) const;
    // The hex containing pixel `p`.
    Hex from_pixel(Vec2f p) const;

    // Corner `corner` (0..5, wrapped) relative to a hex's center. Corners go
    // clockwise on screen; corner 0 is at 30 degrees below the +x axis for
    // pointy-top hexes and on the +x axis for flat-top ones.
    Vec2f corner_offset(i32 corner) const;
    std::array<Vec2f, 6> corners(Hex h) const;

    // Width and height of one hex's bounding box, in pixels.
    Vec2f hex_extent() const;

    // Offset cell {col, row} of a hex, and back.
    Vec2i to_offset(Hex h) const;
    Hex from_offset(Vec2i cell) const;
};

} // namespace kin
