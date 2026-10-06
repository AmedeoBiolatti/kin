#include "audio_analysis.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

namespace kin {

std::vector<f32> audio_spectrum_bins(std::span<const f32> samples) {
    const std::size_t n = samples.size();
    if (n < 2 || (n & (n - 1)) != 0) {
        return {};
    }
    std::vector<std::complex<f32>> data(n);
    for (std::size_t i = 0; i < n; ++i) {
        const f32 window = 0.5f - 0.5f * std::cos(2.0f * std::numbers::pi_v<f32> * static_cast<f32>(i) / static_cast<f32>(n));
        data[i] = samples[i] * window;
    }

    // In-place iterative radix-2 FFT.
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(data[i], data[j]);
        }
    }
    for (std::size_t length = 2; length <= n; length <<= 1) {
        const f32 angle = -2.0f * std::numbers::pi_v<f32> / static_cast<f32>(length);
        const std::complex<f32> step{std::cos(angle), std::sin(angle)};
        for (std::size_t start = 0; start < n; start += length) {
            std::complex<f32> w{1.0f, 0.0f};
            for (std::size_t k = 0; k < length / 2; ++k) {
                const std::complex<f32> even = data[start + k];
                const std::complex<f32> odd = data[start + k + length / 2] * w;
                data[start + k] = even + odd;
                data[start + k + length / 2] = even - odd;
                w *= step;
            }
        }
    }

    // A Hann window sums to n / 2; a real sine splits between two mirrored bins.
    std::vector<f32> bins(n / 2 + 1);
    const f32 scale = 4.0f / static_cast<f32>(n);
    for (std::size_t k = 0; k < bins.size(); ++k) {
        bins[k] = std::abs(data[k]) * scale;
    }
    return bins;
}

f32 audio_band_magnitude(std::span<const f32> bins, f32 sample_rate, f32 from_hz, f32 to_hz) {
    if (bins.size() < 2 || sample_rate <= 0.0f) {
        return 0.0f;
    }
    const f32 hz_per_bin = sample_rate / (2.0f * static_cast<f32>(bins.size() - 1));
    const f32 low = std::max(0.0f, std::min(from_hz, to_hz)) / hz_per_bin;
    const f32 high = std::max(from_hz, to_hz) / hz_per_bin;
    const std::size_t first = static_cast<std::size_t>(std::ceil(low));
    const std::size_t last = std::min(bins.size() - 1, static_cast<std::size_t>(std::floor(high)));
    if (first <= last) {
        return *std::max_element(bins.begin() + static_cast<std::ptrdiff_t>(first),
                                 bins.begin() + static_cast<std::ptrdiff_t>(last) + 1);
    }
    // Narrower than a bin: interpolate at the centre.
    const f32 centre = std::clamp((low + high) * 0.5f, 0.0f, static_cast<f32>(bins.size() - 1));
    const std::size_t i = std::min(bins.size() - 2, static_cast<std::size_t>(centre));
    const f32 t = centre - static_cast<f32>(i);
    return bins[i] + (bins[i + 1] - bins[i]) * t;
}

} // namespace kin
