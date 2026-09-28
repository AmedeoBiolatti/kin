#include <kin/audio/backend.hpp>

#include <kin/audio/audio_clip.hpp>
#include <kin/platform/log.hpp>

#include <SDL3/SDL.h>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

namespace kin {
namespace {

class NullAudioBackend final : public IAudioBackend {
public:
    NullAudioBackend(i32 sample_rate, i32 channels)
        : _sample_rate(sample_rate),
          _channels(channels) {
    }

    bool available() const override { return false; }
    i32 sample_rate() const override { return _sample_rate; }
    i32 channels() const override { return _channels; }
    i32 queued_frames() const override { return _queued_frames; }

    void queue_interleaved(std::span<const f32> samples) override {
        if (_channels > 0) {
            _queued_frames += static_cast<i32>(samples.size()) / _channels;
            _queued_frames = std::min(_queued_frames, _sample_rate);
        }
    }

private:
    i32 _sample_rate = 48000;
    i32 _channels = 2;
    i32 _queued_frames = 0;
};

class SdlAudioBackend final : public IAudioBackend {
public:
    SdlAudioBackend(i32 sample_rate, i32 channels)
        : _sample_rate(sample_rate),
          _channels(channels) {
        if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            throw std::runtime_error(std::string("SDL_InitSubSystem audio failed: ") + SDL_GetError());
        }

        SDL_AudioSpec spec{
            .format = SDL_AUDIO_F32,
            .channels = channels,
            .freq = sample_rate,
        };
        _stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (!_stream) {
            throw std::runtime_error(std::string("SDL_OpenAudioDeviceStream failed: ") + SDL_GetError());
        }
        SDL_ResumeAudioStreamDevice(_stream);
        KIN_LOG_INFO_F("audio",
                       "sdl audio device opened",
                       (LogFields{
                           {.name = "sample_rate", .value = std::to_string(sample_rate)},
                           {.name = "channels", .value = std::to_string(channels)},
                       }));
    }

    ~SdlAudioBackend() override {
        if (_stream) {
            SDL_DestroyAudioStream(_stream);
        }
    }

    bool available() const override { return _stream != nullptr; }
    i32 sample_rate() const override { return _sample_rate; }
    i32 channels() const override { return _channels; }

    i32 queued_frames() const override {
        if (!_stream || _channels <= 0) {
            return 0;
        }
        return SDL_GetAudioStreamQueued(_stream) / static_cast<int>(sizeof(f32) * _channels);
    }

    void queue_interleaved(std::span<const f32> samples) override {
        if (!_stream || samples.empty()) {
            return;
        }
        SDL_PutAudioStreamData(_stream, samples.data(), static_cast<int>(samples.size() * sizeof(f32)));
    }

private:
    SDL_AudioStream* _stream = nullptr;
    i32 _sample_rate = 48000;
    i32 _channels = 2;
};

void ensure_sdl_audio() {
    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        throw std::runtime_error(std::string("SDL_InitSubSystem audio failed: ") + SDL_GetError());
    }
}

} // namespace

std::unique_ptr<IAudioBackend> create_null_audio_backend(i32 sample_rate, i32 channels) {
    return std::make_unique<NullAudioBackend>(sample_rate, channels);
}

std::unique_ptr<IAudioBackend> create_sdl_audio_backend(i32 sample_rate, i32 channels) {
    return std::make_unique<SdlAudioBackend>(sample_rate, channels);
}

AudioClip load_audio_clip(const std::filesystem::path& path) {
    ensure_sdl_audio();

    SDL_AudioSpec src_spec{};
    Uint8* wav = nullptr;
    Uint32 wav_len = 0;
    if (!SDL_LoadWAV(path.string().c_str(), &src_spec, &wav, &wav_len)) {
        const std::string error = "SDL_LoadWAV failed for " + path.string() + ": " + SDL_GetError();
        KIN_LOG_ERROR_F("audio",
                        "audio clip load failed",
                        (LogFields{
                            {.name = "path", .value = path.string()},
                            {.name = "error", .value = error},
                        }));
        throw std::runtime_error(error);
    }

    SDL_AudioSpec dst_spec{
        .format = SDL_AUDIO_F32,
        .channels = 2,
        .freq = 48000,
    };
    SDL_AudioStream* stream = SDL_CreateAudioStream(&src_spec, &dst_spec);
    if (!stream) {
        SDL_free(wav);
        const std::string error = std::string("SDL_CreateAudioStream failed: ") + SDL_GetError();
        KIN_LOG_ERROR_F("audio",
                        "audio conversion stream failed",
                        (LogFields{
                            {.name = "path", .value = path.string()},
                            {.name = "error", .value = error},
                        }));
        throw std::runtime_error(error);
    }

    const bool put_ok = SDL_PutAudioStreamData(stream, wav, static_cast<int>(wav_len));
    SDL_free(wav);
    if (!put_ok || !SDL_FlushAudioStream(stream)) {
        SDL_DestroyAudioStream(stream);
        const std::string error = std::string("SDL audio conversion failed: ") + SDL_GetError();
        KIN_LOG_ERROR_F("audio",
                        "audio conversion failed",
                        (LogFields{
                            {.name = "path", .value = path.string()},
                            {.name = "error", .value = error},
                        }));
        throw std::runtime_error(error);
    }

    const int available = SDL_GetAudioStreamAvailable(stream);
    std::vector<f32> samples(static_cast<std::size_t>(std::max(0, available)) / sizeof(f32));
    const int read = samples.empty() ? 0 : SDL_GetAudioStreamData(stream, samples.data(), available);
    SDL_DestroyAudioStream(stream);
    if (read < 0) {
        const std::string error = std::string("SDL_GetAudioStreamData failed: ") + SDL_GetError();
        KIN_LOG_ERROR_F("audio",
                        "audio conversion read failed",
                        (LogFields{
                            {.name = "path", .value = path.string()},
                            {.name = "error", .value = error},
                        }));
        throw std::runtime_error(error);
    }
    samples.resize(static_cast<std::size_t>(read) / sizeof(f32));

    AudioClip clip = make_memory_audio_clip(path.filename().string(), std::move(samples), dst_spec.channels, dst_spec.freq);
    KIN_LOG_INFO_F("audio",
                   "audio clip loaded",
                   (LogFields{
                       {.name = "path", .value = path.string()},
                       {.name = "frames", .value = std::to_string(clip.frame_count())},
                       {.name = "channels", .value = std::to_string(clip.channels())},
                   }));
    return clip;
}

} // namespace kin
