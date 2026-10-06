#include <kin/audio/backend.hpp>

#include <kin/platform/log.hpp>

#include <SDL3/SDL.h>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>
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
    void start(AudioRenderFn) override {}
    void stop() override {}

private:
    i32 _sample_rate = 48000;
    i32 _channels = 2;
};

// Mixes on SDL's audio thread: the stream asks for more data whenever the device
// runs low, so a long frame on the game thread no longer starves the device.
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

    void start(AudioRenderFn render) override {
        if (!_stream) {
            return;
        }
        stop();
        _render = std::move(render);
        SDL_SetAudioStreamGetCallback(_stream, &SdlAudioBackend::callback, this);
        SDL_ResumeAudioStreamDevice(_stream);
    }

    void stop() override {
        if (_stream) {
            // Takes the stream lock, so a callback in progress finishes first.
            SDL_SetAudioStreamGetCallback(_stream, nullptr, nullptr);
        }
        _render = {};
    }

private:
    static void SDLCALL callback(void* userdata, SDL_AudioStream* stream, int additional_amount, int) {
        auto* self = static_cast<SdlAudioBackend*>(userdata);
        const int frame_bytes = static_cast<int>(sizeof(f32)) * self->_channels;
        const int frames = frame_bytes > 0 ? additional_amount / frame_bytes : 0;
        if (frames <= 0 || !self->_render) {
            return;
        }
        // Grows to the largest request once, then never allocates on this thread.
        self->_buffer.resize(std::max(self->_buffer.size(), static_cast<std::size_t>(frames * self->_channels)));
        const std::span<f32> out{self->_buffer.data(), static_cast<std::size_t>(frames * self->_channels)};
        self->_render(out);
        SDL_PutAudioStreamData(stream, out.data(), static_cast<int>(out.size_bytes()));
    }

    SDL_AudioStream* _stream = nullptr;
    i32 _sample_rate = 48000;
    i32 _channels = 2;
    AudioRenderFn _render;
    std::vector<f32> _buffer;
};

} // namespace

std::unique_ptr<IAudioBackend> create_null_audio_backend(i32 sample_rate, i32 channels) {
    return std::make_unique<NullAudioBackend>(sample_rate, channels);
}

std::unique_ptr<IAudioBackend> create_sdl_audio_backend(i32 sample_rate, i32 channels) {
    return std::make_unique<SdlAudioBackend>(sample_rate, channels);
}

} // namespace kin
