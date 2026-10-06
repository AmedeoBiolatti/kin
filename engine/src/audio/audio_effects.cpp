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
            if (frame % update_every == 0 && (_cutoff != target_cutoff() || _q != _effect.q || _gain != _effect.gain)) {
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
        _gain = _effect.gain;
        const f32 w0 = 2.0f * std::numbers::pi_v<f32> * _cutoff / _sample_rate;
        const f32 c = std::cos(w0);
        const f32 alpha = std::sin(w0) / (2.0f * std::max(_q, 0.05f));
        const f32 a = std::pow(10.0f, _gain / 40.0f); // amplitude, for peak and shelves
        const f32 root = 2.0f * std::sqrt(a) * alpha;
        f32 b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a0 = 1.0f, a1 = 0.0f, a2 = 0.0f;
        switch (_effect.type) {
        case AudioEffectType::LowPass:
            b0 = b2 = (1.0f - c) * 0.5f, b1 = 1.0f - c, a0 = 1.0f + alpha, a1 = -2.0f * c, a2 = 1.0f - alpha;
            break;
        case AudioEffectType::HighPass:
            b0 = b2 = (1.0f + c) * 0.5f, b1 = -(1.0f + c), a0 = 1.0f + alpha, a1 = -2.0f * c, a2 = 1.0f - alpha;
            break;
        case AudioEffectType::BandPass: // 0 dB at the centre
            b0 = alpha, b1 = 0.0f, b2 = -alpha, a0 = 1.0f + alpha, a1 = -2.0f * c, a2 = 1.0f - alpha;
            break;
        case AudioEffectType::Notch:
            b0 = b2 = 1.0f, b1 = -2.0f * c, a0 = 1.0f + alpha, a1 = -2.0f * c, a2 = 1.0f - alpha;
            break;
        case AudioEffectType::Peak:
            b0 = 1.0f + alpha * a, b1 = -2.0f * c, b2 = 1.0f - alpha * a;
            a0 = 1.0f + alpha / a, a1 = -2.0f * c, a2 = 1.0f - alpha / a;
            break;
        case AudioEffectType::LowShelf:
            b0 = a * ((a + 1.0f) - (a - 1.0f) * c + root);
            b1 = 2.0f * a * ((a - 1.0f) - (a + 1.0f) * c);
            b2 = a * ((a + 1.0f) - (a - 1.0f) * c - root);
            a0 = (a + 1.0f) + (a - 1.0f) * c + root;
            a1 = -2.0f * ((a - 1.0f) + (a + 1.0f) * c);
            a2 = (a + 1.0f) + (a - 1.0f) * c - root;
            break;
        case AudioEffectType::HighShelf:
            b0 = a * ((a + 1.0f) + (a - 1.0f) * c + root);
            b1 = -2.0f * a * ((a - 1.0f) + (a + 1.0f) * c);
            b2 = a * ((a + 1.0f) + (a - 1.0f) * c - root);
            a0 = (a + 1.0f) - (a - 1.0f) * c + root;
            a1 = 2.0f * ((a - 1.0f) - (a + 1.0f) * c);
            a2 = (a + 1.0f) - (a - 1.0f) * c - root;
            break;
        default:
            break;
        }
        _b0 = b0 / a0;
        _b1 = b1 / a0;
        _b2 = b2 / a0;
        _a1 = a1 / a0;
        _a2 = a2 / a0;
    }

    f32 _sample_rate;
    f32 _glide;
    f32 _cutoff = 1000.0f;
    f32 _q = 0.7071f;
    f32 _gain = 0.0f;
    f32 _b0 = 1.0f, _b1 = 0.0f, _b2 = 0.0f, _a1 = 0.0f, _a2 = 0.0f;
    std::array<f32, 2> _z1{};
    std::array<f32, 2> _z2{};
};

// A feedback delay: echoes `time` apart, each `feedback` times the last. A new
// time glides in (the read point moves like a tape head), so changing it does
// not crackle; the line holds up to twice the first time asked for (at least
// a second, at most five).
class Delay final : public AudioEffectProcessor {
public:
    Delay(const AudioEffect& effect, i32 sample_rate)
        : _sample_rate(static_cast<f32>(sample_rate)),
          _glide(1.0f - std::exp(-1.0f / (0.05f * static_cast<f32>(sample_rate)))) {
        const f32 seconds = std::clamp(effect.time * 2.0f, 1.0f, 5.0f);
        _length = static_cast<std::size_t>(seconds * _sample_rate) + 2;
        _line.assign(_length * 2, 0.0f);
        set(effect);
        _delay = _target;
    }

    void set(const AudioEffect& effect) override {
        _effect = effect;
        _target = std::clamp(effect.time * _sample_rate, 1.0f, static_cast<f32>(_length - 2));
    }

    void process(std::span<f32> interleaved, i32 channels) override {
        if (!_effect.enabled) {
            return;
        }
        const std::size_t stride = static_cast<std::size_t>(channels);
        const std::size_t frames = interleaved.size() / stride;
        const f32 feedback = std::clamp(_effect.feedback, 0.0f, 0.98f);
        const f32 wet = std::max(0.0f, _effect.wet);
        const f32 dry = std::max(0.0f, _effect.dry);
        for (std::size_t frame = 0; frame < frames; ++frame) {
            _delay += (_target - _delay) * _glide;
            f32 read = static_cast<f32>(_write) - _delay;
            if (read < 0.0f) {
                read += static_cast<f32>(_length);
            }
            const std::size_t i0 = static_cast<std::size_t>(read);
            const std::size_t i1 = i0 + 1 == _length ? 0 : i0 + 1;
            const f32 t = read - static_cast<f32>(i0);
            for (std::size_t c = 0; c < stride && c < 2; ++c) {
                f32& sample = interleaved[frame * stride + c];
                const f32 echo = _line[i0 * 2 + c] + (_line[i1 * 2 + c] - _line[i0 * 2 + c]) * t;
                _line[_write * 2 + c] = sample + echo * feedback;
                sample = sample * dry + echo * wet;
            }
            _write = _write + 1 == _length ? 0 : _write + 1;
        }
    }

private:
    f32 _sample_rate;
    f32 _glide;
    std::size_t _length = 0;
    std::vector<f32> _line; // stereo frames
    std::size_t _write = 0;
    f32 _delay = 1.0f; // frames
    f32 _target = 1.0f;
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
    case AudioEffectType::HighPass:
    case AudioEffectType::BandPass:
    case AudioEffectType::Notch:
    case AudioEffectType::Peak:
    case AudioEffectType::LowShelf:
    case AudioEffectType::HighShelf: return std::make_unique<BiquadFilter>(effect, sample_rate);
    case AudioEffectType::Delay: return std::make_unique<Delay>(effect, sample_rate);
    case AudioEffectType::Reverb: return std::make_unique<Freeverb>(effect, sample_rate);
    case AudioEffectType::Compressor: return std::make_unique<Compressor>(effect, sample_rate);
    }
    return nullptr;
}

} // namespace kin
