#include "audio_effects.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <vector>

namespace kin {
namespace {

f32 db_to_gain(f32 db) {
    return std::pow(10.0f, db / 20.0f);
}

f32 gain_to_db(f32 gain) {
    return 20.0f * std::log10(std::max(gain, 1e-9f));
}

// Robert Bristow-Johnson's cookbook filters, in transposed direct form II.
// The cutoff glides toward a new value over ~20 ms, recomputing the
// coefficients every few samples, so sweeping it does not zipper.
class BiquadFilter final : public AudioEffectProcessor {
public:
    BiquadFilter(const AudioEffect& effect, i32 sample_rate)
        : _sample_rate(static_cast<f32>(sample_rate)),
          _glide(1.0f - std::exp(-static_cast<f32>(update_every) / (0.02f * static_cast<f32>(sample_rate)))) {
        set(effect);
        _cutoff = target_cutoff();
        compute();
    }

    void set(const AudioEffect& effect) override { _effect = effect; }

    void process(std::span<f32> interleaved, i32 channels) override {
        if (!_effect.enabled) {
            return;
        }
        const std::size_t stride = static_cast<std::size_t>(channels);
        const std::size_t frames = interleaved.size() / stride;
        for (std::size_t frame = 0; frame < frames; ++frame) {
            if (frame % update_every == 0 && (_cutoff != target_cutoff() || _q != _effect.q)) {
                const f32 target = target_cutoff();
                _cutoff += (target - _cutoff) * _glide;
                if (std::fabs(target - _cutoff) < 0.01f) {
                    _cutoff = target;
                }
                compute();
            }
            for (std::size_t c = 0; c < stride && c < 2; ++c) {
                f32& sample = interleaved[frame * stride + c];
                const f32 in = sample;
                const f32 out = _b0 * in + _z1[c];
                _z1[c] = _b1 * in - _a1 * out + _z2[c];
                _z2[c] = _b2 * in - _a2 * out;
                sample = out;
            }
        }
    }

private:
    static constexpr std::size_t update_every = 32;

    f32 target_cutoff() const { return std::clamp(_effect.cutoff, 10.0f, _sample_rate * 0.49f); }

    void compute() {
        _q = _effect.q;
        const f32 w0 = 2.0f * std::numbers::pi_v<f32> * _cutoff / _sample_rate;
        const f32 cos_w0 = std::cos(w0);
        const f32 alpha = std::sin(w0) / (2.0f * std::max(_q, 0.05f));
        const f32 a0 = 1.0f + alpha;
        const bool low = _effect.type == AudioEffectType::LowPass;
        const f32 b1 = low ? 1.0f - cos_w0 : -(1.0f + cos_w0);
        const f32 b0 = (low ? 1.0f - cos_w0 : 1.0f + cos_w0) * 0.5f;
        _b0 = b0 / a0;
        _b1 = b1 / a0;
        _b2 = b0 / a0;
        _a1 = -2.0f * cos_w0 / a0;
        _a2 = (1.0f - alpha) / a0;
    }

    f32 _sample_rate;
    f32 _glide;
    f32 _cutoff = 1000.0f;
    f32 _q = 0.7071f;
    f32 _b0 = 1.0f, _b1 = 0.0f, _b2 = 0.0f, _a1 = 0.0f, _a2 = 0.0f;
    std::array<f32, 2> _z1{};
    std::array<f32, 2> _z2{};
};

// Jezar's Freeverb: eight damped comb filters into four allpasses per side,
// the right side's delays a little longer for width. Delays are tuned for
// 44.1 kHz and scaled to the engine's rate.
class Freeverb final : public AudioEffectProcessor {
public:
    Freeverb(const AudioEffect& effect, i32 sample_rate) {
        const f32 scale = static_cast<f32>(sample_rate) / 44100.0f;
        constexpr std::array<i32, 8> comb_tuning{1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
        constexpr std::array<i32, 4> allpass_tuning{556, 441, 341, 225};
        constexpr i32 spread = 23;
        for (std::size_t side = 0; side < 2; ++side) {
            for (std::size_t i = 0; i < comb_tuning.size(); ++i) {
                _combs[side][i].buffer.assign(length(comb_tuning[i] + (side ? spread : 0), scale), 0.0f);
            }
            for (std::size_t i = 0; i < allpass_tuning.size(); ++i) {
                _allpasses[side][i].buffer.assign(length(allpass_tuning[i] + (side ? spread : 0), scale), 0.0f);
            }
        }
        set(effect);
    }

    void set(const AudioEffect& effect) override {
        _effect = effect;
        _feedback = std::clamp(effect.room_size, 0.0f, 1.0f) * 0.28f + 0.7f;
        _damp = std::clamp(effect.damping, 0.0f, 1.0f) * 0.4f;
        const f32 wet = std::max(0.0f, effect.wet) * 3.0f;
        const f32 width = std::clamp(effect.width, 0.0f, 1.0f);
        _wet1 = wet * (width * 0.5f + 0.5f);
        _wet2 = wet * ((1.0f - width) * 0.5f);
    }

    void process(std::span<f32> interleaved, i32 channels) override {
        if (!_effect.enabled) {
            return;
        }
        const std::size_t stride = static_cast<std::size_t>(channels);
        const std::size_t frames = interleaved.size() / stride;
        const f32 dry = std::max(0.0f, _effect.dry);
        for (std::size_t frame = 0; frame < frames; ++frame) {
            f32* sample = interleaved.data() + frame * stride;
            const f32 left = sample[0];
            const f32 right = stride > 1 ? sample[1] : left;
            const f32 input = (left + right) * 0.015f;
            f32 out[2]{};
            for (std::size_t side = 0; side < 2; ++side) {
                for (Comb& comb : _combs[side]) {
                    out[side] += comb.process(input, _feedback, _damp);
                }
                for (Allpass& allpass : _allpasses[side]) {
                    out[side] = allpass.process(out[side]);
                }
            }
            if (stride > 1) {
                sample[0] = out[0] * _wet1 + out[1] * _wet2 + left * dry;
                sample[1] = out[1] * _wet1 + out[0] * _wet2 + right * dry;
            } else {
                sample[0] = (out[0] + out[1]) * 0.5f * (_wet1 + _wet2) + left * dry;
            }
        }
    }

private:
    static std::size_t length(i32 tuning, f32 scale) {
        return static_cast<std::size_t>(std::max(1, static_cast<i32>(std::lround(static_cast<f32>(tuning) * scale))));
    }

    struct Comb {
        std::vector<f32> buffer;
        std::size_t index = 0;
        f32 store = 0.0f;
        f32 process(f32 input, f32 feedback, f32 damp) {
            const f32 output = buffer[index];
            store = output * (1.0f - damp) + store * damp;
            buffer[index] = input + store * feedback;
            index = index + 1 == buffer.size() ? 0 : index + 1;
            return output;
        }
    };

    struct Allpass {
        std::vector<f32> buffer;
        std::size_t index = 0;
        f32 process(f32 input) {
            const f32 delayed = buffer[index];
            buffer[index] = input + delayed * 0.5f;
            index = index + 1 == buffer.size() ? 0 : index + 1;
            return delayed - input;
        }
    };

    std::array<std::array<Comb, 8>, 2> _combs;
    std::array<std::array<Allpass, 4>, 2> _allpasses;
    f32 _feedback = 0.84f;
    f32 _damp = 0.2f;
    f32 _wet1 = 0.9f;
    f32 _wet2 = 0.0f;
};

// A feed-forward compressor on the block's peak level, with the gain change
// smoothed by attack and release (in dB, so it sounds even at every level).
class Compressor final : public AudioEffectProcessor {
public:
    Compressor(const AudioEffect& effect, i32 sample_rate)
        : _sample_rate(static_cast<f32>(sample_rate)) {
        set(effect);
    }

    void set(const AudioEffect& effect) override {
        _effect = effect;
        _attack = coefficient(effect.attack);
        _release = coefficient(effect.release);
    }

    void process(std::span<f32> interleaved, i32 channels) override {
        if (!_effect.enabled) {
            return;
        }
        const std::size_t stride = static_cast<std::size_t>(channels);
        const std::size_t frames = interleaved.size() / stride;
        const f32 slope = 1.0f - 1.0f / std::max(1.0f, _effect.ratio);
        for (std::size_t frame = 0; frame < frames; ++frame) {
            f32* sample = interleaved.data() + frame * stride;
            f32 peak = 0.0f;
            for (std::size_t c = 0; c < stride; ++c) {
                peak = std::max(peak, std::fabs(sample[c]));
            }
            const f32 over = gain_to_db(peak) - _effect.threshold;
            const f32 reduction = over > 0.0f ? over * slope : 0.0f;
            const f32 coef = reduction > _reduction ? _attack : _release;
            _reduction = reduction + (_reduction - reduction) * coef;
            const f32 gain = db_to_gain(_effect.makeup - _reduction);
            for (std::size_t c = 0; c < stride; ++c) {
                sample[c] *= gain;
            }
        }
    }

private:
    f32 coefficient(f32 seconds) const {
        return seconds > 0.0f ? std::exp(-1.0f / (seconds * _sample_rate)) : 0.0f;
    }

    f32 _sample_rate;
    f32 _attack = 0.0f;
    f32 _release = 0.0f;
    f32 _reduction = 0.0f; // dB the gain is down by
};

} // namespace

std::unique_ptr<AudioEffectProcessor> make_audio_effect(const AudioEffect& effect, i32 sample_rate) {
    switch (effect.type) {
    case AudioEffectType::LowPass:
    case AudioEffectType::HighPass: return std::make_unique<BiquadFilter>(effect, sample_rate);
    case AudioEffectType::Reverb: return std::make_unique<Freeverb>(effect, sample_rate);
    case AudioEffectType::Compressor: return std::make_unique<Compressor>(effect, sample_rate);
    }
    return nullptr;
}

} // namespace kin
