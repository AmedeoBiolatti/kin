#include <kin/core/dmath.hpp>

#include <cmath>
#include <limits>

// Built with -ffp-contract=off (engine/CMakeLists.txt): a fused multiply-add rounds once where a multiply and an add
// round twice, so a compiler that fuses them on one target and not on another would give other bits.

namespace kin::dmath {
namespace {

constexpr f32 pi = 3.14159265358979323846f;
constexpr f32 half_pi = 1.57079632679489661923f;
constexpr f32 quarter_pi = 0.78539816339744830962f;
constexpr f32 four_over_pi = 1.27323954473516268615f;
// pi/4 in three parts (Cody-Waite): y * dp1 is exact for the y a reduction meets, so x - y * pi/4 keeps its bits.
constexpr f32 dp1 = 0.78515625f;
constexpr f32 dp2 = 2.4187564849853515625e-4f;
constexpr f32 dp3 = 3.77489497744594108e-8f;
// Past this, reduce by whole turns first (in double: std::fmod is exact) so the octant fits the integer.
constexpr f32 large_argument = 8192.0f;
constexpr f64 two_pi = 6.283185307179586476925;

f32 nan_value() {
    return std::numeric_limits<f32>::quiet_NaN();
}

bool finite(f32 x) {
    return std::fabs(x) <= std::numeric_limits<f32>::max();
}

struct Reduced {
    f32 x;      // in [-pi/4, pi/4]
    u32 octant; // 0 to 7
};

// |x| (finite) as a multiple of pi/4 and what is left.
Reduced reduce(f32 ax) {
    if (ax > large_argument) {
        ax = static_cast<f32>(std::fmod(static_cast<f64>(ax), two_pi));
    }
    u32 j = static_cast<u32>(ax * four_over_pi);
    f32 y = static_cast<f32>(j);
    // Odd octants count from the next multiple, so the remainder is in [-pi/4, pi/4].
    if (j & 1u) {
        j += 1;
        y += 1.0f;
    }
    return {((ax - y * dp1) - y * dp2) - y * dp3, j & 7u};
}

f32 sin_poly(f32 x) {
    const f32 z = x * x;
    return ((-1.9515295891e-4f * z + 8.3321608736e-3f) * z - 1.6666654611e-1f) * z * x + x;
}

f32 cos_poly(f32 x) {
    const f32 z = x * x;
    return ((2.443315711809948e-5f * z - 1.388731625493765e-3f) * z + 4.166664568298827e-2f) * z * z - 0.5f * z + 1.0f;
}

f32 atan_core(f32 ax) {
    f32 base = 0.0f;
    f32 t = ax;
    if (ax > 2.414213562373095f) { // tan(3 pi / 8)
        base = half_pi;
        t = -1.0f / ax;
    } else if (ax > 0.4142135623730950f) { // tan(pi / 8)
        base = quarter_pi;
        t = (ax - 1.0f) / (ax + 1.0f);
    }
    const f32 z = t * t;
    return base +
           ((((8.05374449538e-2f * z - 1.38776856032e-1f) * z + 1.99777106478e-1f) * z - 3.33329491539e-1f) * z * t +
            t);
}

f32 asin_core(f32 a) {
    // a in [0, 1]
    f32 z = 0.0f;
    f32 t = 0.0f;
    const bool upper = a > 0.5f;
    if (upper) {
        z = 0.5f * (1.0f - a);
        t = std::sqrt(z);
    } else {
        t = a;
        z = a * a;
    }
    f32 r = ((((4.2163199048e-2f * z + 2.4181311049e-2f) * z + 4.5470025998e-2f) * z + 7.4953002686e-2f) * z +
             1.6666752422e-1f) *
                z * t +
            t;
    if (upper) {
        r = half_pi - (r + r);
    }
    return r;
}

f32 with_sign(f32 magnitude, bool negative) {
    return negative ? -magnitude : magnitude;
}

} // namespace

void sincos(f32 x, f32& sin_out, f32& cos_out) {
    if (!finite(x)) {
        sin_out = cos_out = nan_value();
        return;
    }
    const bool negative = std::signbit(x);
    auto [r, j] = reduce(std::fabs(x));
    bool sin_negative = negative;
    bool cos_negative = false;
    if (j > 3) {
        sin_negative = !sin_negative;
        cos_negative = !cos_negative;
        j -= 4;
    }
    if (j > 1) {
        cos_negative = !cos_negative;
    }
    const f32 s = sin_poly(r);
    const f32 c = cos_poly(r);
    const bool swap = j == 1 || j == 2;
    sin_out = with_sign(swap ? c : s, sin_negative);
    cos_out = with_sign(swap ? s : c, cos_negative);
}

f32 sin(f32 x) {
    f32 s = 0.0f;
    f32 c = 0.0f;
    sincos(x, s, c);
    return s;
}

f32 cos(f32 x) {
    f32 s = 0.0f;
    f32 c = 0.0f;
    sincos(x, s, c);
    return c;
}

f32 tan(f32 x) {
    f32 s = 0.0f;
    f32 c = 0.0f;
    sincos(x, s, c);
    return s / c;
}

f32 asin(f32 x) {
    if (!(std::fabs(x) <= 1.0f)) {
        return nan_value();
    }
    return with_sign(asin_core(std::fabs(x)), std::signbit(x));
}

f32 acos(f32 x) {
    if (!(std::fabs(x) <= 1.0f)) {
        return nan_value();
    }
    if (x < -0.5f) {
        return pi - 2.0f * asin_core(std::sqrt(0.5f * (1.0f + x)));
    }
    if (x > 0.5f) {
        return 2.0f * asin_core(std::sqrt(0.5f * (1.0f - x)));
    }
    return half_pi - with_sign(asin_core(std::fabs(x)), std::signbit(x));
}

f32 atan(f32 x) {
    if (std::isnan(x)) {
        return nan_value();
    }
    return with_sign(atan_core(std::fabs(x)), std::signbit(x));
}

f32 atan2(f32 y, f32 x) {
    if (std::isnan(x) || std::isnan(y)) {
        return nan_value();
    }
    const bool y_negative = std::signbit(y);
    if (y == 0.0f) {
        // atan2(+-0, +x) = +-0; atan2(+-0, -x) = +-pi (also for x = -0).
        return std::signbit(x) ? with_sign(pi, y_negative) : y;
    }
    if (x == 0.0f) {
        return with_sign(half_pi, y_negative);
    }
    if (std::isinf(x) || std::isinf(y)) {
        if (std::isinf(x) && std::isinf(y)) {
            return with_sign(x > 0.0f ? quarter_pi : 3.0f * quarter_pi, y_negative);
        }
        if (std::isinf(y)) {
            return with_sign(half_pi, y_negative);
        }
        return x > 0.0f ? with_sign(0.0f, y_negative) : with_sign(pi, y_negative);
    }
    const f32 angle = with_sign(atan_core(std::fabs(y / x)), y_negative != std::signbit(x));
    if (x > 0.0f) {
        return angle;
    }
    return y_negative ? angle - pi : angle + pi;
}

} // namespace kin::dmath
