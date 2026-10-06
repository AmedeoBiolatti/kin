#pragma once

#include <kin/core/types.hpp>

#include <memory>
#include <span>
#include <string>
#include <vector>

namespace kin {

class AudioDecoder;

enum class AudioFormat {
    Unknown,
    F32,
};

class IAudioClipBackend {
public:
    virtual ~IAudioClipBackend() = default;
    // Decoded samples; empty for a streamed clip.
    virtual std::span<const f32> samples() const = 0;
    virtual bool streamed() const { return false; }
    // Memory the clip holds: its samples, or a streamed file's bytes.
    virtual std::size_t memory_bytes() const { return samples().size_bytes(); }
    // A new decoder positioned at the start, for a streamed clip; null otherwise.
    virtual std::unique_ptr<AudioDecoder> open_stream() const;
};

class AudioClip {
public:
    AudioClip() = default;
    AudioClip(std::string name,
              AudioFormat format,
              i32 channels,
              i32 sample_rate,
              i32 frame_count,
              std::shared_ptr<IAudioClipBackend> backend);

    bool valid() const;
    explicit operator bool() const { return valid(); }

    const std::string& name() const { return _name; }
    AudioFormat format() const { return _format; }
    i32 channels() const { return _channels; }
    i32 sample_rate() const { return _sample_rate; }
    i32 frame_count() const { return _frame_count; }
    f32 duration() const;
    std::span<const f32> samples() const;
    // A streamed clip keeps its file compressed in memory, and each voice
    // playing it decodes as it goes (see load_audio_stream).
    bool streamed() const;
    std::unique_ptr<AudioDecoder> open_stream() const;
    std::size_t memory_bytes() const;

private:
    std::string _name;
    AudioFormat _format = AudioFormat::Unknown;
    i32 _channels = 0;
    i32 _sample_rate = 0;
    i32 _frame_count = 0;
    std::shared_ptr<IAudioClipBackend> _backend;
};

class MemoryAudioClipBackend final : public IAudioClipBackend {
public:
    explicit MemoryAudioClipBackend(std::vector<f32> samples);
    std::span<const f32> samples() const override;

private:
    std::vector<f32> _samples;
};

AudioClip make_memory_audio_clip(std::string name,
                                 std::vector<f32> samples,
                                 i32 channels = 2,
                                 i32 sample_rate = 48000);

} // namespace kin
