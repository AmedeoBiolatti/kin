#pragma once

#include <cstdint>

namespace kin {

using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

using f32 = float;
using f64 = double;

struct Vec2i {
    i32 x = 0;
    i32 y = 0;

    friend constexpr bool operator==(Vec2i, Vec2i) = default;
};

struct Vec2f {
    f32 x = 0.0f;
    f32 y = 0.0f;

    friend constexpr bool operator==(Vec2f, Vec2f) = default;
};

struct Rectf {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 w = 0.0f;
    f32 h = 0.0f;

    friend constexpr bool operator==(Rectf, Rectf) = default;
};

} // namespace kin
