#pragma once

#include <kin/core/types.hpp>

namespace kin {

// Mirroring a sprite within its rectangle, left-right (X) and/or upside
// down (Y), before it turns about its pivot.
enum class Flip : u8 { None = 0, X = 1, Y = 2, XY = 3 };
constexpr Flip operator|(Flip a, Flip b) {
    return static_cast<Flip>(static_cast<u8>(a) | static_cast<u8>(b));
}
constexpr Flip operator^(Flip a, Flip b) {
    return static_cast<Flip>(static_cast<u8>(a) ^ static_cast<u8>(b));
}
constexpr bool flips_x(Flip f) {
    return (static_cast<u8>(f) & 1u) != 0;
}
constexpr bool flips_y(Flip f) {
    return (static_cast<u8>(f) & 2u) != 0;
}
constexpr Flip flip_of(bool x, bool y) {
    return static_cast<Flip>((x ? 1u : 0u) | (y ? 2u : 0u));
}
// `source` (texture pixels) read the other way round: what backends are given
// for a flipped draw (a negative width or height reads right to left or bottom
// to top).
constexpr Rectf mirrored(Rectf source, Flip flip) {
    if (flips_x(flip)) {
        source = {source.x + source.w, source.y, -source.w, source.h};
    }
    if (flips_y(flip)) {
        source = {source.x, source.y + source.h, source.w, -source.h};
    }
    return source;
}

} // namespace kin
