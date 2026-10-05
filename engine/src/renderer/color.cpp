#include <kin/renderer/color.hpp>

#include <algorithm>
#include <cmath>

namespace kin {

f32 srgb_to_linear(f32 c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

f32 linear_to_srgb(f32 c) {
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

LinearColor to_linear(Color c) {
    constexpr f32 inv = 1.0f / 255.0f;
    return {srgb_to_linear(c.r * inv), srgb_to_linear(c.g * inv), srgb_to_linear(c.b * inv), c.a * inv};
}

Color to_srgb(LinearColor c) {
    const auto byte = [](f32 v) { return static_cast<u8>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return {byte(linear_to_srgb(std::clamp(c.r, 0.0f, 1.0f))), byte(linear_to_srgb(std::clamp(c.g, 0.0f, 1.0f))),
            byte(linear_to_srgb(std::clamp(c.b, 0.0f, 1.0f))), byte(c.a)};
}

Oklab to_oklab(LinearColor c) {
    const f32 l = std::cbrt(0.4122214708f * c.r + 0.5363325363f * c.g + 0.0514459929f * c.b);
    const f32 m = std::cbrt(0.2119034982f * c.r + 0.6806995451f * c.g + 0.1073969566f * c.b);
    const f32 s = std::cbrt(0.0883024619f * c.r + 0.2817188376f * c.g + 0.6299787005f * c.b);
    return {0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s,
            1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s,
            0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s};
}

LinearColor from_oklab(Oklab c, f32 alpha) {
    const f32 l = c.l + 0.3963377774f * c.a + 0.2158037573f * c.b;
    const f32 m = c.l - 0.1055613458f * c.a - 0.0638541728f * c.b;
    const f32 s = c.l - 0.0894841775f * c.a - 1.2914855480f * c.b;
    const f32 l3 = l * l * l, m3 = m * m * m, s3 = s * s * s;
    return {4.0767416621f * l3 - 3.3077115913f * m3 + 0.2309699292f * s3,
            -1.2684380046f * l3 + 2.6097574011f * m3 - 0.3413193965f * s3,
            -0.0041960863f * l3 - 0.7034186147f * m3 + 1.7076147010f * s3, alpha};
}

Color mix(Color a, Color b, f32 t, ColorMix space) {
    if (space == ColorMix::Srgb) {
        return mix(a, b, t);
    }
    const LinearColor la = to_linear(a), lb = to_linear(b);
    const f32 alpha = la.a + (lb.a - la.a) * t;
    if (space == ColorMix::Linear) {
        return to_srgb({la.r + (lb.r - la.r) * t, la.g + (lb.g - la.g) * t, la.b + (lb.b - la.b) * t, alpha});
    }
    const Oklab oa = to_oklab(la), ob = to_oklab(lb);
    return to_srgb(from_oklab({oa.l + (ob.l - oa.l) * t, oa.a + (ob.a - oa.a) * t, oa.b + (ob.b - oa.b) * t}, alpha));
}

} // namespace kin
