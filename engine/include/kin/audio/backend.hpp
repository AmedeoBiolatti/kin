#pragma once

#include <kin/core/types.hpp>

#include <filesystem>
#include <memory>
#include <span>
#include <string>

namespace kin {

class AudioClip;

class IAudioBackend {
public:
    virtual ~IAudioBackend() = default;
    virtual bool available() const = 0;
    virtual i32 sample_rate() const = 0;
    virtual i32 channels() const = 0;
    virtual i32 queued_frames() const = 0;
    virtual void queue_interleaved(std::span<const f32> samples) = 0;
};

std::unique_ptr<IAudioBackend> create_null_audio_backend(i32 sample_rate = 48000, i32 channels = 2);
std::unique_ptr<IAudioBackend> create_sdl_audio_backend(i32 sample_rate = 48000, i32 channels = 2);

AudioClip load_audio_clip(const std::filesystem::path& path);

} // namespace kin
