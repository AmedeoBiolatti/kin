#pragma once

#include <kin/core/types.hpp>

namespace kin {

struct Color {
    u8 r = 255;
    u8 g = 255;
    u8 b = 255;
    u8 a = 255;

    friend constexpr bool operator==(Color, Color) = default;

    static constexpr Color rgb(u8 red, u8 green, u8 blue) {
        return {red, green, blue, 255};
    }

    static constexpr Color rgba(u8 red, u8 green, u8 blue, u8 alpha) {
        return {red, green, blue, alpha};
    }
};

namespace colors {

inline constexpr Color black{0, 0, 0, 255};
inline constexpr Color white{255, 255, 255, 255};
inline constexpr Color transparent{0, 0, 0, 0};

} // namespace colors

namespace detail {

constexpr u8 mix_channel(u8 a, u8 b, f32 t) {
    const f32 value = static_cast<f32>(a) + (static_cast<f32>(b) - static_cast<f32>(a)) * t;
    return static_cast<u8>(value + 0.5f); // t in [0,1] → value is non-negative
}

} // namespace detail

// Component-wise blend, t in [0,1]: t=0 → a, t=1 → b. Named `mix` (GLSL) rather
// than `lerp` to avoid colliding with the file-local lerp in anim/value.cpp.
constexpr Color mix(Color a, Color b, f32 t) {
    return Color{
        detail::mix_channel(a.r, b.r, t),
        detail::mix_channel(a.g, b.g, t),
        detail::mix_channel(a.b, b.b, t),
        detail::mix_channel(a.a, b.a, t),
    };
}

} // namespace kin
