#include <kin/audio/backend.hpp>

#include <kin/platform/log.hpp>

#include <SDL3/SDL.h>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>
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

void ensure_sdl_audio() {
    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        throw std::runtime_error(std::string("SDL_InitSubSystem audio failed: ") + SDL_GetError());
    }
}

// The playback device with this name, or 0.
SDL_AudioDeviceID find_playback_device(std::string_view name) {
    int count = 0;
    SDL_AudioDeviceID* devices = SDL_GetAudioPlaybackDevices(&count);
    SDL_AudioDeviceID found = 0;
    for (int i = 0; devices && i < count && !found; ++i) {
        const char* device_name = SDL_GetAudioDeviceName(devices[i]);
        if (device_name && name == device_name) {
            found = devices[i];
        }
    }
    SDL_free(devices);
    return found;
}

// Mixes on SDL's audio thread: the stream asks for more data whenever the device
// runs low, so a long frame on the game thread no longer starves the device.
class SdlAudioBackend final : public IAudioBackend {
public:
    SdlAudioBackend(i32 sample_rate, i32 channels, std::string_view device)
        : _sample_rate(sample_rate),
          _channels(channels) {
        ensure_sdl_audio();
        if (!device.empty() && open(device)) {
            _wanted = std::string{device};
        } else {
            if (!device.empty()) {
                KIN_LOG_WARN_F("audio", "audio device not found, using the default",
                               (LogFields{{.name = "device", .value = std::string{device}}}));
            }
            if (!open({})) {
                throw std::runtime_error(std::string("SDL_OpenAudioDeviceStream failed: ") + SDL_GetError());
            }
        }
    }

    ~SdlAudioBackend() override {
        if (_stream) {
            SDL_DestroyAudioStream(_stream);
        }
    }

    bool available() const override { return _stream != nullptr; }
    i32 sample_rate() const override { return _sample_rate; }
    i32 channels() const override { return _channels; }
    std::string device() const override { return _device; }

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

    bool set_device(std::string_view name) override {
        if (!open(name)) {
            return false;
        }
        _wanted = std::string{name};
        return true;
    }

    // Follows a chosen device that goes away (to the default) and comes back.
    void poll() override {
        if (_wanted.empty()) {
            return;
        }
        const bool present = find_playback_device(_wanted) != 0;
        if (!present && !_device.empty()) {
            KIN_LOG_WARN_F("audio", "audio device lost, using the default", (LogFields{{.name = "device", .value = _wanted}}));
            open({});
        } else if (present && _device != _wanted) {
            KIN_LOG_INFO_F("audio", "audio device back", (LogFields{{.name = "device", .value = _wanted}}));
            open(_wanted);
        }
    }

private:
    // Opens a stream on the named device ("" for the default) and moves
    // playback to it; the old stream is closed only once the new one works.
    bool open(std::string_view name) {
        const SDL_AudioDeviceID id = name.empty() ? SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK : find_playback_device(name);
        if (id == 0) {
            return false;
        }
        const SDL_AudioSpec spec{
            .format = SDL_AUDIO_F32,
            .channels = _channels,
            .freq = _sample_rate,
        };
        SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(id, &spec, nullptr, nullptr);
        if (!stream) {
            return false;
        }
        if (_stream) {
            SDL_SetAudioStreamGetCallback(_stream, nullptr, nullptr); // waits for a callback in progress
            SDL_DestroyAudioStream(_stream);
        }
        _stream = stream;
        _device = std::string{name};
        if (_render) {
            SDL_SetAudioStreamGetCallback(_stream, &SdlAudioBackend::callback, this);
            SDL_ResumeAudioStreamDevice(_stream);
        }
        KIN_LOG_INFO_F("audio",
                       "sdl audio device opened",
                       (LogFields{
                           {.name = "device", .value = name.empty() ? std::string{"default"} : std::string{name}},
                           {.name = "sample_rate", .value = std::to_string(_sample_rate)},
                           {.name = "channels", .value = std::to_string(_channels)},
                       }));
        return true;
    }

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
    std::string _device; // the device playing now; "" is the default
    std::string _wanted; // the device the game chose
    AudioRenderFn _render;
    std::vector<f32> _buffer;
};

} // namespace

std::unique_ptr<IAudioBackend> create_null_audio_backend(i32 sample_rate, i32 channels) {
    return std::make_unique<NullAudioBackend>(sample_rate, channels);
}

std::unique_ptr<IAudioBackend> create_sdl_audio_backend(i32 sample_rate, i32 channels, std::string_view device) {
    return std::make_unique<SdlAudioBackend>(sample_rate, channels, device);
}

std::vector<std::string> list_audio_output_devices() {
    try {
        ensure_sdl_audio();
    } catch (const std::exception&) {
        return {};
    }
    std::vector<std::string> names;
    int count = 0;
    SDL_AudioDeviceID* devices = SDL_GetAudioPlaybackDevices(&count);
    for (int i = 0; devices && i < count; ++i) {
        if (const char* name = SDL_GetAudioDeviceName(devices[i])) {
            names.emplace_back(name);
        }
    }
    SDL_free(devices);
    return names;
}

} // namespace kin
