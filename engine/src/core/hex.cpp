#include <kin/core/hex.hpp>

#include <algorithm>
#include <cmath>

namespace kin {
namespace {

constexpr f64 sqrt3 = 1.7320508075688772;
constexpr f64 pi = 3.14159265358979323846;

Hex hex_round_f64(f64 q, f64 r) {
    const f64 s = -q - r;
    f64 rq = std::round(q);
    f64 rr = std::round(r);
    const f64 rs = std::round(s);
    const f64 dq = std::abs(rq - q);
    const f64 dr = std::abs(rr - r);
    const f64 ds = std::abs(rs - s);
    // Rounding each coordinate can break q + r + s == 0; recompute the one that
    // moved furthest from the others.
    if (dq > dr && dq > ds) {
        rq = -rr - rs;
    } else if (dr > ds) {
        rr = -rq - rs;
    }
    return {static_cast<i32>(rq), static_cast<i32>(rr)};
}

} // namespace

Hex hex_round(f32 q, f32 r) {
    return hex_round_f64(q, r);
}

std::vector<Hex> hex_ring(Hex center, i32 radius) {
    if (radius < 0) {
        return {};
    }
    if (radius == 0) {
        return {center};
    }
    std::vector<Hex> out;
    out.reserve(static_cast<std::size_t>(6 * radius));
    // Start `radius` steps along direction 4 and walk each side of the ring.
    Hex h = center + HexDirections[4] * radius;
    for (const Hex& dir : HexDirections) {
        for (i32 step = 0; step < radius; ++step) {
            out.push_back(h);
            h = h + dir;
        }
    }
    return out;
}

std::vector<Hex> hex_range(Hex center, i32 radius) {
    if (radius < 0) {
        return {};
    }
    std::vector<Hex> out;
    out.reserve(static_cast<std::size_t>(3 * radius * (radius + 1) + 1));
    for (i32 dq = -radius; dq <= radius; ++dq) {
        const i32 lo = std::max(-radius, -dq - radius);
        const i32 hi = std::min(radius, -dq + radius);
        for (i32 dr = lo; dr <= hi; ++dr) {
            out.push_back(center + Hex{dq, dr});
        }
    }
    return out;
}

std::vector<Hex> hex_line(Hex a, Hex b) {
    const i32 n = hex_distance(a, b);
    std::vector<Hex> out;
    out.reserve(static_cast<std::size_t>(n) + 1);
    if (n == 0) {
        out.push_back(a);
        return out;
    }
    // Nudge the endpoints off hex edges so points exactly between two hexes
    // round the same way every time.
    constexpr f64 eps = 1e-6;
    const f64 aq = a.q + eps, ar = a.r + eps;
    const f64 bq = b.q + eps, br = b.r + eps;
    for (i32 i = 0; i <= n; ++i) {
        const f64 t = static_cast<f64>(i) / static_cast<f64>(n);
        out.push_back(hex_round_f64(aq + (bq - aq) * t, ar + (br - ar) * t));
    }
    return out;
}

Vec2f HexLayout::to_pixel(Hex h) const {
    const f64 q = h.q;
    const f64 r = h.r;
    f64 x = 0.0;
    f64 y = 0.0;
    if (orientation == HexOrientation::PointyTop) {
        x = sqrt3 * q + sqrt3 / 2.0 * r;
        y = 1.5 * r;
    } else {
        x = 1.5 * q;
        y = sqrt3 / 2.0 * q + sqrt3 * r;
    }
    return {static_cast<f32>(x * size.x + origin.x), static_cast<f32>(y * size.y + origin.y)};
}

Hex HexLayout::from_pixel(Vec2f p) const {
    if (size.x == 0.0f || size.y == 0.0f) {
        return {};
    }
    const f64 x = (static_cast<f64>(p.x) - origin.x) / size.x;
    const f64 y = (static_cast<f64>(p.y) - origin.y) / size.y;
    if (orientation == HexOrientation::PointyTop) {
        return hex_round_f64(sqrt3 / 3.0 * x - y / 3.0, 2.0 / 3.0 * y);
    }
    return hex_round_f64(2.0 / 3.0 * x, -x / 3.0 + sqrt3 / 3.0 * y);
}

Vec2f HexLayout::corner_offset(i32 corner) const {
    const f64 start = orientation == HexOrientation::PointyTop ? 30.0 : 0.0;
    const f64 angle = (start + 60.0 * hex_wrap_direction(corner)) * pi / 180.0;
    return {static_cast<f32>(size.x * std::cos(angle)), static_cast<f32>(size.y * std::sin(angle))};
}

std::array<Vec2f, 6> HexLayout::corners(Hex h) const {
    const Vec2f center = to_pixel(h);
    std::array<Vec2f, 6> out{};
    for (i32 i = 0; i < 6; ++i) {
        const Vec2f offset = corner_offset(i);
        out[static_cast<std::size_t>(i)] = {center.x + offset.x, center.y + offset.y};
    }
    return out;
}

Vec2f HexLayout::hex_extent() const {
    if (orientation == HexOrientation::PointyTop) {
        return {static_cast<f32>(sqrt3 * size.x), 2.0f * size.y};
    }
    return {2.0f * size.x, static_cast<f32>(sqrt3 * size.y)};
}

// Offset conversions follow https://www.redblobgames.com/grids/hexagons/.
// `x & 1` is the parity of x for negative values too (two's complement).
Vec2i HexLayout::to_offset(Hex h) const {
    const bool odd = offset == HexOffset::Odd;
    if (orientation == HexOrientation::PointyTop) {
        const i32 shift = odd ? (h.r - (h.r & 1)) / 2 : (h.r + (h.r & 1)) / 2;
        return {h.q + shift, h.r};
    }
    const i32 shift = odd ? (h.q - (h.q & 1)) / 2 : (h.q + (h.q & 1)) / 2;
    return {h.q, h.r + shift};
}

Hex HexLayout::from_offset(Vec2i cell) const {
    const bool odd = offset == HexOffset::Odd;
    if (orientation == HexOrientation::PointyTop) {
        const i32 shift = odd ? (cell.y - (cell.y & 1)) / 2 : (cell.y + (cell.y & 1)) / 2;
        return {cell.x - shift, cell.y};
    }
    const i32 shift = odd ? (cell.x - (cell.x & 1)) / 2 : (cell.x + (cell.x & 1)) / 2;
    return {cell.x, cell.y - shift};
}

} // namespace kin
