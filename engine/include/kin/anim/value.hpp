#pragma once

#include <kin/core/types.hpp>
#include <kin/renderer/color.hpp>

#include <string>
#include <string_view>
#include <variant>

namespace kin {

enum class AnimValueKind : u8 {
    Float,
    Vec2,
    Color,
    Int,
    Bool,
    String,
};

using AnimValue = std::variant<f32, Vec2f, Color, i32, bool, std::string>;

AnimValueKind value_kind(const AnimValue& value);
std::string_view anim_value_kind_name(AnimValueKind kind);
bool parse_anim_value_kind(std::string_view text, AnimValueKind& out);

enum class Easing : u8 {
    Step,
    Linear,
    EaseIn,
    EaseOut,
    EaseInOut,
};

std::string_view easing_name(Easing easing);
bool parse_easing(std::string_view text, Easing& out);
f32 ease(Easing easing, f32 t);
AnimValue interpolate(const AnimValue& a, const AnimValue& b, f32 t, Easing easing);

} // namespace kin
