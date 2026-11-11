#pragma once

#include <functional>
#include <kin/core/types.hpp>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace kin {

// Sound from a microphone (or any recording device), as mono 32-bit float samples at the rate asked for: the device's
// own format is converted. read() takes what has arrived since the last call and never waits. For voice chat, or a
// game that listens.
//
//     std::string error;
//     auto mic = kin::open_audio_capture({.sample_rate = 16000}, &error);
//     std::vector<kin::f32> samples(4096);
//     const std::size_t got = mic ? mic->read(samples) : 0;
struct AudioCaptureOptions {
    i32 sample_rate = 48000;
    std::string device; // a name from list_audio_input_devices(); "" for the system's default
};

class IAudioCapture {
public:
    virtual ~IAudioCapture() = default;
    virtual i32 sample_rate() const = 0;
    // The device recording; "" for the default (or a capture that is not a device).
    virtual std::string device() const = 0;
    // Copies up to out.size() of the samples that arrived, oldest first, and returns how many. Samples not read wait
    // for the next call (a device keeps about a second).
    virtual std::size_t read(std::span<f32> out) = 0;
    // Paused, nothing is recorded (and a device is let go of, so the system shows the microphone off).
    virtual void set_paused(bool paused) = 0;
    virtual bool paused() const = 0;
};

// Opens a recording device. nullptr, and `error` says why, when there is none or it cannot be opened (no audio, no
// microphone, or the system refused access to it).
std::unique_ptr<IAudioCapture> open_audio_capture(const AudioCaptureOptions& options = {},
                                                  std::string* error = nullptr);
// Samples from memory played as if they were being recorded, again and again: what read() gives is what would have
// arrived since the last read by `seconds`, a clock in seconds (the steady clock if empty). For tests, and for trying a
// voice chat without a microphone.
std::unique_ptr<IAudioCapture> make_memory_audio_capture(std::vector<f32> samples, i32 sample_rate,
                                                         std::function<f64()> seconds = {});
// The names of the recording devices plugged in now, for a settings menu.
std::vector<std::string> list_audio_input_devices();

// What the engine plays, as it plays it (AudioEngine::open_output_tap): a copy of the final output, after the limiter,
// taken as each block is mixed, for an echo canceller (what the speakers play, to find in the microphone) or to write
// to a file. The audio thread writes and read() takes on any thread. It holds `seconds` of it: what is not read in
// time is lost, oldest first, and counted.
//
// Blocks are mixed ahead of the device, so a frame is heard some milliseconds after the tap has it.
class AudioOutputTap {
public:
    AudioOutputTap(i32 sample_rate, i32 channels, f32 seconds);
    i32 sample_rate() const { return _sample_rate; }
    i32 channels() const { return _channels; }
    // Copies up to out.size() / channels() frames, interleaved as the engine plays them, oldest first, and returns
    // how many frames; `first`, if given, is set to the output frame of the first of them.
    std::size_t read(std::span<f32> out, i64* first = nullptr);
    // The output frame (counted from the engine's first) of the frame read() gives next.
    i64 next_frame() const;
    std::size_t available() const; // frames waiting
    u64 lost() const;              // frames dropped before they were read

    // The engine's side: `interleaved` is output frame `frame` on.
    void write(std::span<const f32> interleaved, i64 frame);

private:
    mutable std::mutex _mutex;
    i32 _sample_rate = 48000;
    i32 _channels = 2;
    std::vector<f32> _ring; // capacity frames, interleaved
    std::size_t _capacity = 0;
    std::size_t _start = 0; // where the oldest frame is
    std::size_t _count = 0;
    i64 _next = 0; // the oldest frame's output frame
    u64 _lost = 0;
};

} // namespace kin
