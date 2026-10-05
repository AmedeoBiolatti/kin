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

// sRGB and linear light. Color holds sRGB values (as images and colour pickers
// do); light adds and blends in linear values. Per channel, 0 to 1 (IEC 61966-2-1).
f32 srgb_to_linear(f32 c);
f32 linear_to_srgb(f32 c);

// A colour in linear light, straight alpha. Channels may pass 1 (HDR: brighter
// than white, until tonemapped).
struct LinearColor {
    f32 r = 1.0f;
    f32 g = 1.0f;
    f32 b = 1.0f;
    f32 a = 1.0f;

    friend constexpr bool operator==(LinearColor, LinearColor) = default;
};

LinearColor to_linear(Color c);
Color to_srgb(LinearColor c); // clamped to 0..1 and rounded

// OKLab (Björn Ottosson): lightness `l` (0 black, 1 white) and two opponent
// axes, `a` green to red, `b` blue to yellow. Even steps in it look even.
struct Oklab {
    f32 l = 0.0f;
    f32 a = 0.0f;
    f32 b = 0.0f;
};

Oklab to_oklab(LinearColor c);
LinearColor from_oklab(Oklab c, f32 alpha = 1.0f);

// Where two colours are mixed: their sRGB values (what mix() above does; a
// muddy, dark middle between saturated colours), linear light (how light
// mixes: what blending does in a linear pipeline), or OKLab (even to the eye).
enum class ColorMix : u8 { Srgb, Linear, Oklab };

Color mix(Color a, Color b, f32 t, ColorMix space);

} // namespace kin
