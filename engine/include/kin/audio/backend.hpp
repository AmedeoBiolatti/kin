#pragma once

#include <kin/core/types.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kin {

class AudioClip;

// Fills `interleaved` (frames * channels samples) with the next output audio.
using AudioRenderFn = std::function<void(std::span<f32> interleaved)>;

// Where mixed audio goes. A device backend calls the render function on its own
// audio thread whenever the device needs more; a backend that is not real time
// (the null backend) never calls it, and AudioEngine::update renders elapsed
// time itself instead.
class IAudioBackend {
public:
    virtual ~IAudioBackend() = default;
    // True when a device consumes audio in real time and pulls from `render`.
    virtual bool available() const = 0;
    virtual i32 sample_rate() const = 0;
    virtual i32 channels() const = 0;
    virtual void start(AudioRenderFn render) = 0;
    // After stop() returns, the render function is never called again.
    virtual void stop() = 0;
    // Moves playback to the named output device ("" for the default); false if
    // it cannot. A backend without devices never can.
    virtual bool set_device(std::string_view) { return false; }
    // The device playing now; "" for the default.
    virtual std::string device() const { return {}; }
    // Called about once a second from AudioEngine::update.
    virtual void poll() {}
};

std::unique_ptr<IAudioBackend> create_null_audio_backend(i32 sample_rate = 48000, i32 channels = 2);
// `device`: a name from list_audio_output_devices; empty or unknown opens the default.
std::unique_ptr<IAudioBackend> create_sdl_audio_backend(i32 sample_rate = 48000,
                                                        i32 channels = 2,
                                                        std::string_view device = {});
// The names of the output devices plugged in now, for a settings menu.
std::vector<std::string> list_audio_output_devices();

// Decodes a whole audio file (WAV, AIFF, FLAC, Ogg Vorbis or MP3) to 32-bit
// float samples at the file's own sample rate, as mono or stereo. Throws
// std::runtime_error if the file cannot be read.
AudioClip load_audio_clip(const std::filesystem::path& path);
// Reads an audio file into memory without decoding it: a streamed clip, which
// voices decode as they play. For music and long ambience, where the decoded
// samples would take ten times the memory.
AudioClip load_audio_stream(const std::filesystem::path& path);

} // namespace kin
