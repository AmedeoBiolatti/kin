#pragma once

#include <kin/audio/audio_catalog.hpp>

#include <memory>
#include <span>

namespace kin {

// Runs one AudioEffect over a bus's interleaved block on the audio thread. Made
// (and its buffers allocated) on the game thread; process() never allocates.
class AudioEffectProcessor {
public:
    virtual ~AudioEffectProcessor() = default;
    virtual void process(std::span<f32> interleaved, i32 channels) = 0;
    // New parameters of the same type, keeping the effect's state (filter
    // memory, reverb tail) so changing them while playing does not click.
    virtual void set(const AudioEffect& effect) = 0;
    const AudioEffect& effect() const { return _effect; }

protected:
    AudioEffect _effect;
};

std::unique_ptr<AudioEffectProcessor> make_audio_effect(const AudioEffect& effect, i32 sample_rate);

} // namespace kin
