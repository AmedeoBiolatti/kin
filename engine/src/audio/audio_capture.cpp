#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <kin/audio/audio_capture.hpp>
#include <string_view>
#include <utility>

namespace kin {
namespace {

bool sdl_audio_ready(std::string* error) {
    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        if (error) {
            *error = std::string("SDL_InitSubSystem audio failed: ") + SDL_GetError();
        }
        return false;
    }
    return true;
}

// The recording device with this name, or 0.
SDL_AudioDeviceID find_recording_device(std::string_view name) {
    int count = 0;
    SDL_AudioDeviceID* devices = SDL_GetAudioRecordingDevices(&count);
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

// SDL records on its own thread into the stream, which converts to our format; read() takes from it.
class SdlAudioCapture final : public IAudioCapture {
public:
    SdlAudioCapture(SDL_AudioStream* stream, i32 sample_rate, std::string device)
        : _stream(stream), _sample_rate(sample_rate), _device(std::move(device)) {}
    ~SdlAudioCapture() override { SDL_DestroyAudioStream(_stream); }

    i32 sample_rate() const override { return _sample_rate; }
    std::string device() const override { return _device; }

    std::size_t read(std::span<f32> out) override {
        const int available = SDL_GetAudioStreamAvailable(_stream);
        const std::size_t want = std::min(out.size(), static_cast<std::size_t>(std::max(0, available)) / sizeof(f32));
        if (want == 0) {
            return 0;
        }
        const int got = SDL_GetAudioStreamData(_stream, out.data(), static_cast<int>(want * sizeof(f32)));
        return got > 0 ? static_cast<std::size_t>(got) / sizeof(f32) : 0;
    }

    void set_paused(bool paused) override {
        if (paused == _paused) {
            return;
        }
        _paused = paused;
        if (paused) {
            SDL_PauseAudioStreamDevice(_stream);
        } else {
            SDL_ClearAudioStream(_stream); // what was left from before the pause is stale
            SDL_ResumeAudioStreamDevice(_stream);
        }
    }
    bool paused() const override { return _paused; }

private:
    SDL_AudioStream* _stream = nullptr;
    i32 _sample_rate = 48000;
    std::string _device;
    bool _paused = false;
};

class MemoryAudioCapture final : public IAudioCapture {
public:
    MemoryAudioCapture(std::vector<f32> samples, i32 sample_rate, std::function<f64()> seconds)
        : _samples(std::move(samples)), _sample_rate(std::max(1, sample_rate)), _seconds(std::move(seconds)) {
        if (!_seconds) {
            const auto start = std::chrono::steady_clock::now();
            _seconds = [start] { return std::chrono::duration<f64>(std::chrono::steady_clock::now() - start).count(); };
        }
        _last = _seconds();
    }

    i32 sample_rate() const override { return _sample_rate; }
    std::string device() const override { return {}; }

    std::size_t read(std::span<f32> out) override {
        const f64 now = _seconds();
        _owed += std::max(0.0, now - _last) * static_cast<f64>(_sample_rate);
        _last = now;
        _owed = std::min(_owed, static_cast<f64>(_sample_rate)); // a device keeps about a second
        if (_paused || _samples.empty()) {
            _owed = 0.0;
            return 0;
        }
        const std::size_t count = std::min(out.size(), static_cast<std::size_t>(_owed));
        for (std::size_t i = 0; i < count; ++i) {
            out[i] = _samples[_position];
            _position = (_position + 1) % _samples.size();
        }
        _owed -= static_cast<f64>(count);
        return count;
    }

    void set_paused(bool paused) override { _paused = paused; }
    bool paused() const override { return _paused; }

private:
    std::vector<f32> _samples;
    i32 _sample_rate = 48000;
    std::function<f64()> _seconds;
    f64 _last = 0.0;
    f64 _owed = 0.0;
    std::size_t _position = 0;
    bool _paused = false;
};

} // namespace

std::unique_ptr<IAudioCapture> open_audio_capture(const AudioCaptureOptions& options, std::string* error) {
    if (!sdl_audio_ready(error)) {
        return nullptr;
    }
    SDL_AudioDeviceID id = SDL_AUDIO_DEVICE_DEFAULT_RECORDING;
    if (!options.device.empty()) {
        id = find_recording_device(options.device);
        if (id == 0) {
            if (error) {
                *error = "no recording device named " + options.device;
            }
            return nullptr;
        }
    }
    const SDL_AudioSpec spec{
        .format = SDL_AUDIO_F32,
        .channels = 1,
        .freq = std::max(1, options.sample_rate),
    };
    SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(id, &spec, nullptr, nullptr);
    if (!stream) {
        if (error) {
            *error = std::string("could not open the recording device: ") + SDL_GetError();
        }
        return nullptr;
    }
    if (!SDL_ResumeAudioStreamDevice(stream)) {
        if (error) {
            *error = std::string("could not start recording: ") + SDL_GetError();
        }
        SDL_DestroyAudioStream(stream);
        return nullptr;
    }
    return std::make_unique<SdlAudioCapture>(stream, spec.freq, options.device);
}

std::unique_ptr<IAudioCapture> make_memory_audio_capture(std::vector<f32> samples, i32 sample_rate,
                                                         std::function<f64()> seconds) {
    return std::make_unique<MemoryAudioCapture>(std::move(samples), sample_rate, std::move(seconds));
}

AudioOutputTap::AudioOutputTap(i32 sample_rate, i32 channels, f32 seconds)
    : _sample_rate(std::max(1, sample_rate)), _channels(std::max(1, channels)),
      _capacity(static_cast<std::size_t>(std::max(1.0f, std::max(0.0f, seconds) * static_cast<f32>(_sample_rate)))) {
    _ring.assign(_capacity * static_cast<std::size_t>(_channels), 0.0f);
}

void AudioOutputTap::write(std::span<const f32> interleaved, i64 frame) {
    const std::size_t stride = static_cast<std::size_t>(_channels);
    std::size_t frames = interleaved.size() / stride;
    const std::scoped_lock lock{_mutex};
    if (_count == 0) {
        _next = frame;
    }
    if (frames > _capacity) { // more than it holds: only the newest
        interleaved = interleaved.subspan((frames - _capacity) * stride);
        _lost += _count + frames - _capacity;
        _start = 0;
        _count = 0;
        _next = frame + static_cast<i64>(frames - _capacity);
        frames = _capacity;
    }
    if (_count + frames > _capacity) {
        const std::size_t drop = _count + frames - _capacity;
        _start = (_start + drop) % _capacity;
        _count -= drop;
        _next += static_cast<i64>(drop);
        _lost += drop;
    }
    std::size_t at = (_start + _count) % _capacity;
    for (std::size_t i = 0; i < frames; ++i) {
        std::copy_n(interleaved.data() + i * stride, stride, _ring.data() + at * stride);
        at = at + 1 == _capacity ? 0 : at + 1;
    }
    _count += frames;
}

std::size_t AudioOutputTap::read(std::span<f32> out, i64* first) {
    const std::size_t stride = static_cast<std::size_t>(_channels);
    const std::scoped_lock lock{_mutex};
    const std::size_t frames = std::min(out.size() / stride, _count);
    if (first) {
        *first = _next;
    }
    for (std::size_t i = 0; i < frames; ++i) {
        std::copy_n(_ring.data() + _start * stride, stride, out.data() + i * stride);
        _start = _start + 1 == _capacity ? 0 : _start + 1;
    }
    _count -= frames;
    _next += static_cast<i64>(frames);
    return frames;
}

i64 AudioOutputTap::next_frame() const {
    const std::scoped_lock lock{_mutex};
    return _next;
}

std::size_t AudioOutputTap::available() const {
    const std::scoped_lock lock{_mutex};
    return _count;
}

u64 AudioOutputTap::lost() const {
    const std::scoped_lock lock{_mutex};
    return _lost;
}

std::vector<std::string> list_audio_input_devices() {
    if (!sdl_audio_ready(nullptr)) {
        return {};
    }
    std::vector<std::string> names;
    int count = 0;
    SDL_AudioDeviceID* devices = SDL_GetAudioRecordingDevices(&count);
    for (int i = 0; devices && i < count; ++i) {
        if (const char* name = SDL_GetAudioDeviceName(devices[i])) {
            names.emplace_back(name);
        }
    }
    SDL_free(devices);
    return names;
}

} // namespace kin
