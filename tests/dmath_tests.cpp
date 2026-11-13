#include <kin/core/dmath.hpp>
#include <kin/core/hash.hpp>

#include <bit>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

namespace {

using namespace kin;

u32 bits(f32 value) {
    return std::bit_cast<u32>(value);
}

f64 max_error(f32 (*fn)(f32), f64 (*reference)(f64), f32 lo, f32 hi, int samples) {
    f64 worst = 0.0;
    for (int i = 0; i <= samples; ++i) {
        const f32 x = lo + (hi - lo) * static_cast<f32>(i) / static_cast<f32>(samples);
        const f64 error = std::fabs(static_cast<f64>(fn(x)) - reference(static_cast<f64>(x)));
        worst = std::max(worst, error);
    }
    return worst;
}

void test_accuracy() {
    const auto sin_ref = [](f64 x) { return std::sin(x); };
    const auto cos_ref = [](f64 x) { return std::cos(x); };
    const auto asin_ref = [](f64 x) { return std::asin(x); };
    const auto acos_ref = [](f64 x) { return std::acos(x); };
    const auto atan_ref = [](f64 x) { return std::atan(x); };
    const f64 sin_error = max_error(dmath::sin, sin_ref, -100.0f, 100.0f, 200'000);
    const f64 cos_error = max_error(dmath::cos, cos_ref, -100.0f, 100.0f, 200'000);
    const f64 sin_far = max_error(dmath::sin, sin_ref, -8192.0f, 8192.0f, 200'000);
    const f64 asin_error = max_error(dmath::asin, asin_ref, -1.0f, 1.0f, 200'000);
    const f64 acos_error = max_error(dmath::acos, acos_ref, -1.0f, 1.0f, 200'000);
    const f64 atan_error = max_error(dmath::atan, atan_ref, -1000.0f, 1000.0f, 200'000);
    std::printf("  max error: sin %.3g cos %.3g sin(|x|<8192) %.3g asin %.3g acos %.3g atan %.3g\n", sin_error,
                cos_error, sin_far, asin_error, acos_error, atan_error);
    assert(sin_error < 3e-7 && cos_error < 3e-7);
    assert(sin_far < 3e-7);
    assert(asin_error < 3e-7 && acos_error < 4e-7 && atan_error < 3e-7);

    // Far out, still near the true value (whole turns come off in double first).
    assert(std::fabs(static_cast<f64>(dmath::sin(1.0e6f)) - std::sin(1.0e6)) < 1e-4);
    assert(std::fabs(static_cast<f64>(dmath::cos(3.0e8f)) - std::cos(3.0e8)) < 1e-4);

    f64 atan2_error = 0.0;
    for (int i = 0; i < 2000; ++i) {
        const f32 angle = -3.14f + 6.28f * static_cast<f32>(i) / 2000.0f;
        for (const f32 radius : {1e-3f, 1.0f, 750.0f}) {
            const f32 y = radius * std::sin(angle);
            const f32 x = radius * std::cos(angle);
            atan2_error = std::max(atan2_error, std::fabs(static_cast<f64>(dmath::atan2(y, x)) -
                                                          std::atan2(static_cast<f64>(y), static_cast<f64>(x))));
        }
    }
    assert(atan2_error < 4e-7);

    f64 tan_error = 0.0;
    for (int i = -1000; i <= 1000; ++i) {
        const f32 x = 1.5f * static_cast<f32>(i) / 1000.0f;
        tan_error = std::max(tan_error, std::fabs(static_cast<f64>(dmath::tan(x)) - std::tan(static_cast<f64>(x))) /
                                            std::max(1.0, std::fabs(std::tan(static_cast<f64>(x)))));
    }
    assert(tan_error < 1e-6);
}

void test_special_values() {
    const f32 inf = std::numeric_limits<f32>::infinity();
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const f32 pi = 3.14159265358979323846f;
    // Zeros keep their sign, as the C functions do.
    assert(bits(dmath::sin(0.0f)) == bits(0.0f) && bits(dmath::sin(-0.0f)) == bits(-0.0f));
    assert(dmath::cos(0.0f) == 1.0f && dmath::cos(-0.0f) == 1.0f);
    assert(bits(dmath::atan(-0.0f)) == bits(-0.0f) && bits(dmath::asin(-0.0f)) == bits(-0.0f));
    // Out of the domain: one NaN, the same bits whatever the input.
    const u32 quiet = bits(std::numeric_limits<f32>::quiet_NaN());
    for (const f32 bad : {inf, -inf, nan}) {
        assert(bits(dmath::sin(bad)) == quiet && bits(dmath::cos(bad)) == quiet && bits(dmath::tan(bad)) == quiet);
    }
    assert(bits(dmath::asin(1.0001f)) == quiet && bits(dmath::acos(-1.0001f)) == quiet);
    assert(bits(dmath::atan(nan)) == quiet && bits(dmath::atan2(nan, 1.0f)) == quiet);
    assert(dmath::atan(inf) == pi / 2.0f && dmath::atan(-inf) == -pi / 2.0f);
    assert(dmath::asin(1.0f) == pi / 2.0f && dmath::acos(1.0f) == 0.0f && dmath::acos(-1.0f) == pi);
    // atan2's quadrants and axes.
    assert(dmath::atan2(1.0f, 0.0f) == pi / 2.0f && dmath::atan2(-1.0f, 0.0f) == -pi / 2.0f);
    assert(bits(dmath::atan2(0.0f, 1.0f)) == bits(0.0f) && bits(dmath::atan2(-0.0f, 1.0f)) == bits(-0.0f));
    assert(dmath::atan2(0.0f, -1.0f) == pi && dmath::atan2(-0.0f, -1.0f) == -pi);
    assert(dmath::atan2(0.0f, -0.0f) == pi);
    assert(dmath::atan2(inf, inf) == pi / 4.0f && dmath::atan2(-inf, -inf) == -3.0f * pi / 4.0f);
    assert(dmath::atan2(1.0f, inf) == 0.0f && dmath::atan2(1.0f, -inf) == pi && dmath::atan2(-inf, 5.0f) == -pi / 2.0f);
    assert(std::fabs(dmath::atan2(1.0f, 1.0f) - pi / 4.0f) < 1e-7f);
    assert(std::fabs(dmath::atan2(-1.0f, -1.0f) + 3.0f * pi / 4.0f) < 2e-7f);
    // Exact symmetries.
    for (int i = 1; i < 5000; ++i) {
        const f32 x = static_cast<f32>(i) * 0.0137f;
        assert(bits(dmath::sin(-x)) == bits(-dmath::sin(x)));
        assert(bits(dmath::cos(-x)) == bits(dmath::cos(x)));
        assert(bits(dmath::atan(-x)) == bits(-dmath::atan(x)));
        f32 s = 0.0f;
        f32 c = 0.0f;
        dmath::sincos(x, s, c);
        assert(bits(s) == bits(dmath::sin(x)) && bits(c) == bits(dmath::cos(x)));
    }
}

// Every function over a spread of inputs, hashed bit for bit. The constant is what kin's own build gives; another
// compiler, C library or CPU that gives another hash breaks determinism between peers. A change to dmath.cpp that is
// meant to change results updates it.
void test_golden_bits() {
    std::vector<u32> out;
    out.reserve(1 << 21);
    for (int i = -150'000; i <= 150'000; ++i) {
        const f32 x = static_cast<f32>(i) * 0.000731f;
        out.push_back(bits(dmath::sin(x)));
        out.push_back(bits(dmath::cos(x)));
        out.push_back(bits(dmath::tan(x)));
        out.push_back(bits(dmath::atan(x * 37.0f)));
    }
    for (int i = 0; i <= 100'000; ++i) {
        const f32 x = static_cast<f32>(i) * 1.37f + 8000.0f; // the large-argument path from 8192 on
        out.push_back(bits(dmath::sin(x)));
        out.push_back(bits(dmath::cos(x)));
    }
    for (int i = -100'000; i <= 100'000; ++i) {
        const f32 x = static_cast<f32>(i) / 100'000.0f;
        out.push_back(bits(dmath::asin(x)));
        out.push_back(bits(dmath::acos(x)));
    }
    for (int i = -300; i <= 300; ++i) {
        for (int j = -300; j <= 300; j += 3) {
            out.push_back(bits(dmath::atan2(static_cast<f32>(i) * 0.37f, static_cast<f32>(j) * 0.53f)));
        }
    }
    const u64 hash = hash_bytes(out.data(), out.size() * sizeof(u32));
    std::printf("  golden: %zu results, hash 0x%016llx\n", out.size(), static_cast<unsigned long long>(hash));
    constexpr u64 expected = 0xab81ae07a9ff8bc3ull;
    assert(hash == expected);
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    test_accuracy();
    test_special_values();
    test_golden_bits();
    std::printf("dmath tests passed\n");
    return 0;
}
