#include <kin/audio/audio_clip.hpp>

#include <utility>

namespace kin {

AudioClip::AudioClip(std::string name,
                     AudioFormat format,
                     i32 channels,
                     i32 sample_rate,
                     i32 frame_count,
                     std::shared_ptr<IAudioClipBackend> backend)
    : _name(std::move(name)),
      _format(format),
      _channels(channels),
      _sample_rate(sample_rate),
      _frame_count(frame_count),
      _backend(std::move(backend)) {
}

bool AudioClip::valid() const {
    return _backend && _format == AudioFormat::F32 && _channels > 0 && _sample_rate > 0 && _frame_count > 0;
}

f32 AudioClip::duration() const {
    return _sample_rate > 0 ? static_cast<f32>(_frame_count) / static_cast<f32>(_sample_rate) : 0.0f;
}

std::span<const f32> AudioClip::samples() const {
    return _backend ? _backend->samples() : std::span<const f32>{};
}

MemoryAudioClipBackend::MemoryAudioClipBackend(std::vector<f32> samples)
    : _samples(std::move(samples)) {
}

std::span<const f32> MemoryAudioClipBackend::samples() const {
    return _samples;
}

AudioClip make_memory_audio_clip(std::string name, std::vector<f32> samples, i32 channels, i32 sample_rate) {
    const i32 frame_count = channels > 0 ? static_cast<i32>(samples.size()) / channels : 0;
    return AudioClip{
        std::move(name),
        AudioFormat::F32,
        channels,
        sample_rate,
        frame_count,
        std::make_shared<MemoryAudioClipBackend>(std::move(samples)),
    };
}

} // namespace kin
