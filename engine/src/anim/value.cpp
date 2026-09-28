#include <kin/anim/value.hpp>

#include <algorithm>
#include <cmath>

namespace kin {
namespace {

f32 lerp(f32 a, f32 b, f32 t) {
    return a + (b - a) * t;
}

Vec2f lerp(Vec2f a, Vec2f b, f32 t) {
    return {
        .x = lerp(a.x, b.x, t),
        .y = lerp(a.y, b.y, t),
    };
}

u8 lerp_channel(u8 a, u8 b, f32 t) {
    return static_cast<u8>(std::clamp(std::lround(lerp(static_cast<f32>(a), static_cast<f32>(b), t)), 0l, 255l));
}

Color lerp(Color a, Color b, f32 t) {
    return {
        .r = lerp_channel(a.r, b.r, t),
        .g = lerp_channel(a.g, b.g, t),
        .b = lerp_channel(a.b, b.b, t),
        .a = lerp_channel(a.a, b.a, t),
    };
}

} // namespace

AnimValueKind value_kind(const AnimValue& value) {
    return static_cast<AnimValueKind>(value.index());
}

std::string_view anim_value_kind_name(AnimValueKind kind) {
    switch (kind) {
    case AnimValueKind::Float: return "float";
    case AnimValueKind::Vec2: return "vec2";
    case AnimValueKind::Color: return "color";
    case AnimValueKind::Int: return "int";
    case AnimValueKind::Bool: return "bool";
    case AnimValueKind::String: return "string";
    }
    return "unknown";
}

bool parse_anim_value_kind(std::string_view text, AnimValueKind& out) {
    if (text == "float") {
        out = AnimValueKind::Float;
    } else if (text == "vec2") {
        out = AnimValueKind::Vec2;
    } else if (text == "color") {
        out = AnimValueKind::Color;
    } else if (text == "int") {
        out = AnimValueKind::Int;
    } else if (text == "bool") {
        out = AnimValueKind::Bool;
    } else if (text == "string") {
        out = AnimValueKind::String;
    } else {
        return false;
    }
    return true;
}

std::string_view easing_name(Easing easing) {
    switch (easing) {
    case Easing::Step: return "step";
    case Easing::Linear: return "linear";
    case Easing::EaseIn: return "easein";
    case Easing::EaseOut: return "easeout";
    case Easing::EaseInOut: return "easeinout";
    }
    return "unknown";
}

bool parse_easing(std::string_view text, Easing& out) {
    if (text == "step") {
        out = Easing::Step;
    } else if (text == "linear") {
        out = Easing::Linear;
    } else if (text == "easein") {
        out = Easing::EaseIn;
    } else if (text == "easeout") {
        out = Easing::EaseOut;
    } else if (text == "easeinout") {
        out = Easing::EaseInOut;
    } else {
        return false;
    }
    return true;
}

f32 ease(Easing easing, f32 t) {
    t = std::clamp(t, 0.0f, 1.0f);
    switch (easing) {
    case Easing::Step:
        return t >= 1.0f ? 1.0f : 0.0f;
    case Easing::Linear:
        return t;
    case Easing::EaseIn:
        return t * t;
    case Easing::EaseOut:
        return 1.0f - (1.0f - t) * (1.0f - t);
    case Easing::EaseInOut:
        return t < 0.5f ? 2.0f * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 2.0f) * 0.5f;
    }
    return t;
}

AnimValue interpolate(const AnimValue& a, const AnimValue& b, f32 t, Easing easing) {
    if (a.index() != b.index()) {
        return a;
    }

    t = std::clamp(t, 0.0f, 1.0f);
    const f32 eased = ease(easing, t);

    switch (value_kind(a)) {
    case AnimValueKind::Float:
        return lerp(std::get<f32>(a), std::get<f32>(b), eased);
    case AnimValueKind::Vec2:
        return lerp(std::get<Vec2f>(a), std::get<Vec2f>(b), eased);
    case AnimValueKind::Color:
        return lerp(std::get<Color>(a), std::get<Color>(b), eased);
    case AnimValueKind::Int:
    case AnimValueKind::Bool:
    case AnimValueKind::String:
        return t >= 1.0f ? b : a;
    }
    return a;
}

} // namespace kin
