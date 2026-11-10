#include <kin/anim/value.hpp>

#include <array>
#include <utility>

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
    case AnimValueKind::Vec3: return "vec3";
    case AnimValueKind::Quat: return "quat";
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
    } else if (text == "vec3") {
        out = AnimValueKind::Vec3;
    } else if (text == "quat") {
        out = AnimValueKind::Quat;
    } else {
        return false;
    }
    return true;
}

namespace {

constexpr std::pair<Easing, std::string_view> easing_names[] = {
    {Easing::Step, "step"},           {Easing::Linear, "linear"},         {Easing::EaseIn, "easein"},
    {Easing::EaseOut, "easeout"},     {Easing::EaseInOut, "easeinout"},   {Easing::SineIn, "sinein"},
    {Easing::SineOut, "sineout"},     {Easing::SineInOut, "sineinout"},   {Easing::CubicIn, "cubicin"},
    {Easing::CubicOut, "cubicout"},   {Easing::CubicInOut, "cubicinout"}, {Easing::BackIn, "backin"},
    {Easing::BackOut, "backout"},     {Easing::BackInOut, "backinout"},   {Easing::ElasticIn, "elasticin"},
    {Easing::ElasticOut, "elasticout"}, {Easing::ElasticInOut, "elasticinout"}, {Easing::BounceIn, "bouncein"},
    {Easing::BounceOut, "bounceout"}, {Easing::BounceInOut, "bounceinout"},
};

constexpr auto all_easings = [] {
    std::array<Easing, std::size(easing_names)> all{};
    for (std::size_t i = 0; i < all.size(); ++i) {
        all[i] = easing_names[i].first;
    }
    return all;
}();

constexpr f32 pi = 3.14159265358979323846f;

f32 bounce_out(f32 t) {
    constexpr f32 n = 7.5625f;
    constexpr f32 d = 2.75f;
    if (t < 1.0f / d) {
        return n * t * t;
    }
    if (t < 2.0f / d) {
        t -= 1.5f / d;
        return n * t * t + 0.75f;
    }
    if (t < 2.5f / d) {
        t -= 2.25f / d;
        return n * t * t + 0.9375f;
    }
    t -= 2.625f / d;
    return n * t * t + 0.984375f;
}

} // namespace

std::span<const Easing> easings() {
    return all_easings;
}

std::string_view easing_name(Easing easing) {
    for (const auto& [value, name] : easing_names) {
        if (value == easing) {
            return name;
        }
    }
    return "unknown";
}

bool parse_easing(std::string_view text, Easing& out) {
    for (const auto& [value, name] : easing_names) {
        if (name == text) {
            out = value;
            return true;
        }
    }
    return false;
}

f32 ease(Easing easing, f32 t) {
    t = std::clamp(t, 0.0f, 1.0f);
    constexpr f32 back = 1.70158f;          // how far Back overshoots
    constexpr f32 back_both = back * 1.525f; // ...going both ways
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
    case Easing::SineIn:
        return 1.0f - std::cos(t * pi * 0.5f);
    case Easing::SineOut:
        return std::sin(t * pi * 0.5f);
    case Easing::SineInOut:
        return -(std::cos(pi * t) - 1.0f) * 0.5f;
    case Easing::CubicIn:
        return t * t * t;
    case Easing::CubicOut:
        return 1.0f - std::pow(1.0f - t, 3.0f);
    case Easing::CubicInOut:
        return t < 0.5f ? 4.0f * t * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) * 0.5f;
    case Easing::BackIn:
        return (back + 1.0f) * t * t * t - back * t * t;
    case Easing::BackOut:
        return 1.0f + (back + 1.0f) * std::pow(t - 1.0f, 3.0f) + back * std::pow(t - 1.0f, 2.0f);
    case Easing::BackInOut:
        return t < 0.5f ? std::pow(2.0f * t, 2.0f) * ((back_both + 1.0f) * 2.0f * t - back_both) * 0.5f
                        : (std::pow(2.0f * t - 2.0f, 2.0f) * ((back_both + 1.0f) * (2.0f * t - 2.0f) + back_both) + 2.0f) * 0.5f;
    case Easing::ElasticIn:
        return t <= 0.0f || t >= 1.0f ? t : -std::pow(2.0f, 10.0f * t - 10.0f) * std::sin((10.0f * t - 10.75f) * (2.0f * pi / 3.0f));
    case Easing::ElasticOut:
        return t <= 0.0f || t >= 1.0f ? t : std::pow(2.0f, -10.0f * t) * std::sin((10.0f * t - 0.75f) * (2.0f * pi / 3.0f)) + 1.0f;
    case Easing::ElasticInOut:
        if (t <= 0.0f || t >= 1.0f) {
            return t;
        }
        return t < 0.5f ? -(std::pow(2.0f, 20.0f * t - 10.0f) * std::sin((20.0f * t - 11.125f) * (2.0f * pi / 4.5f))) * 0.5f
                        : std::pow(2.0f, -20.0f * t + 10.0f) * std::sin((20.0f * t - 11.125f) * (2.0f * pi / 4.5f)) * 0.5f + 1.0f;
    case Easing::BounceIn:
        return 1.0f - bounce_out(1.0f - t);
    case Easing::BounceOut:
        return bounce_out(t);
    case Easing::BounceInOut:
        return t < 0.5f ? (1.0f - bounce_out(1.0f - 2.0f * t)) * 0.5f : (1.0f + bounce_out(2.0f * t - 1.0f)) * 0.5f;
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
    case AnimValueKind::Vec3: {
        const Vec3f from = std::get<Vec3f>(a);
        return from + (std::get<Vec3f>(b) - from) * eased;
    }
    case AnimValueKind::Quat:
        return slerp(std::get<Quatf>(a), std::get<Quatf>(b), eased);
    case AnimValueKind::Int:
    case AnimValueKind::Bool:
    case AnimValueKind::String:
        return t >= 1.0f ? b : a;
    }
    return a;
}

} // namespace kin
