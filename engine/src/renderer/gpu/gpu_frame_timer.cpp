#include "gpu_frame_timer.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>

namespace kin::gpu {

namespace {
// Frames allowed to wait for timing; past this (a stalled GPU) frames go untimed.
constexpr std::size_t MaxFramesInFlight = 8;
// How often the thread checks the oldest fence: the timing's resolution.
constexpr std::chrono::microseconds PollInterval{100};
} // namespace

GpuFrameTimer::GpuFrameTimer(SDL_GPUDevice* device) : _device(device) {
    _thread = std::thread([this] { run(); });
}

GpuFrameTimer::~GpuFrameTimer() {
    {
        std::lock_guard lock{_mutex};
        _stop = true;
    }
    _wake.notify_one();
    _thread.join();
    // A fence still in flight must not go back to SDL's pool, where it could be
    // reset for another submission: wait here, on the render thread.
    for (const Pending& frame : _waiting) {
        SDL_GPUFence* fence = frame.fence;
        SDL_WaitForGPUFences(_device, true, &fence, 1);
        SDL_ReleaseGPUFence(_device, frame.fence);
    }
    for (const Pending& frame : _finished) {
        SDL_ReleaseGPUFence(_device, frame.fence);
    }
}

bool GpuFrameTimer::full() const {
    std::lock_guard lock{_mutex};
    return _waiting.size() + _finished.size() >= MaxFramesInFlight;
}

void GpuFrameTimer::track(SDL_GPUFence* fence, u64 first_submit_ns) {
    {
        std::lock_guard lock{_mutex};
        _waiting.push_back(Pending{.fence = fence, .first_submit_ns = first_submit_ns});
    }
    _wake.notify_one();
}

std::optional<f64> GpuFrameTimer::collect() {
    std::deque<Pending> finished;
    {
        std::lock_guard lock{_mutex};
        finished.swap(_finished);
    }
    std::optional<f64> latest;
    for (const Pending& frame : finished) {
        SDL_ReleaseGPUFence(_device, frame.fence);
        if (frame.gpu_ms >= 0.0) {
            latest = frame.gpu_ms;
        }
    }
    return latest;
}

void GpuFrameTimer::run() {
    std::unique_lock lock{_mutex};
    for (;;) {
        _wake.wait(lock, [this] { return _stop || !_waiting.empty(); });
        if (_stop) {
            return; // the destructor waits for what is left
        }
        SDL_GPUFence* fence = _waiting.front().fence;
        lock.unlock();
        const bool signalled = SDL_QueryGPUFence(_device, fence);
        const u64 now_ns = SDL_GetTicksNS();
        lock.lock();
        if (!signalled) {
            _wake.wait_for(lock, PollInterval, [this] { return _stop; });
            continue;
        }

        Pending frame = _waiting.front();
        _waiting.pop_front();
        const u64 start_ns = std::max(frame.first_submit_ns, _last_signal_ns);
        frame.gpu_ms = now_ns > start_ns ? static_cast<f64>(now_ns - start_ns) / 1'000'000.0 : 0.0;
        _last_signal_ns = now_ns;
        _finished.push_back(frame);
    }
}

} // namespace kin::gpu
