#pragma once

#include <kin/core/types.hpp>

#include <span>
#include <vector>

namespace kin {

// Frames a spectrum is computed from: ~43 ms at 48 kHz, ~23 Hz per bin.
constexpr i32 audio_analysis_frames = 2048;

// The amplitude of each FFT bin of `samples` (a power-of-two count, mono),
// Hann-windowed and scaled so a sine of amplitude A centred on a bin reads A.
// Bin k is k * sample_rate / samples.size() Hz; there are size / 2 + 1 bins.
std::vector<f32> audio_spectrum_bins(std::span<const f32> samples);

// The strongest amplitude between two frequencies; between bins, the
// amplitude interpolated at the range's centre.
f32 audio_band_magnitude(std::span<const f32> bins, f32 sample_rate, f32 from_hz, f32 to_hz);

} // namespace kin
