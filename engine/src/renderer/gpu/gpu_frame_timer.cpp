#include "gpu_frame_timer.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <utility>

namespace kin::gpu {

namespace {
// Fences allowed to wait at once; past this (a stalled GPU) frames go untimed.
// Room for 8 frames of a few scopes each.
constexpr std::size_t MaxFencesInFlight = 64;
// How often the thread checks the oldest fence: the timing's resolution.
constexpr std::chrono::microseconds PollInterval{100};

f64 ms_from(u64 start_ns, u64 end_ns) {
    return end_ns > start_ns ? static_cast<f64>(end_ns - start_ns) / 1'000'000.0 : 0.0;
}
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
    for (const Pending& pending : _waiting) {
        SDL_GPUFence* fence = pending.fence;
        SDL_WaitForGPUFences(_device, true, &fence, 1);
        SDL_ReleaseGPUFence(_device, pending.fence);
    }
    for (const Pending& pending : _finished) {
        SDL_ReleaseGPUFence(_device, pending.fence);
    }
}

bool GpuFrameTimer::full(std::size_t fences) const {
    std::lock_guard lock{_mutex};
    return _waiting.size() + _finished.size() + fences > MaxFencesInFlight;
}

void GpuFrameTimer::push(Pending pending) {
    {
        std::lock_guard lock{_mutex};
        _waiting.push_back(std::move(pending));
    }
    _wake.notify_one();
}

void GpuFrameTimer::track(SDL_GPUFence* fence, u64 first_submit_ns, u32 untimed) {
    push(Pending{.fence = fence, .kind = Kind::Frame, .submit_ns = first_submit_ns, .frames = 1 + untimed});
}

void GpuFrameTimer::track_scope_start(SDL_GPUFence* fence) {
    push(Pending{.fence = fence, .kind = Kind::ScopeStart});
}

void GpuFrameTimer::track_scope_end(std::string name, SDL_GPUFence* fence, u64 end_submit_ns, f64 pixels) {
    push(Pending{.fence = fence, .kind = Kind::ScopeEnd, .submit_ns = end_submit_ns, .name = std::move(name),
                 .pixels = pixels});
}

GpuTimerSamples GpuFrameTimer::collect() {
    std::deque<Pending> finished;
    {
        std::lock_guard lock{_mutex};
        finished.swap(_finished);
    }
    // In submission order, as they signalled.
    GpuTimerSamples samples;
    for (Pending& pending : finished) {
        SDL_ReleaseGPUFence(_device, pending.fence);
        switch (pending.kind) {
        case Kind::Frame:
            samples.frame = GpuFrameSample{
                .ms = ms_from(std::max(pending.submit_ns, _last_frame_signal_ns), pending.signal_ns),
                .frames = pending.frames};
            _last_frame_signal_ns = pending.signal_ns;
            break;
        case Kind::ScopeStart:
            _scope_start_signal_ns = pending.signal_ns;
            break;
        case Kind::ScopeEnd:
            samples.scopes.push_back(GpuScopeSample{
                .name = std::move(pending.name),
                .ms = ms_from(std::max(pending.submit_ns, _scope_start_signal_ns), pending.signal_ns),
                .pixels = pending.pixels});
            break;
        }
    }
    return samples;
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
        Pending pending = std::move(_waiting.front());
        _waiting.pop_front();
        pending.signal_ns = now_ns;
        _finished.push_back(std::move(pending));
    }
}

} // namespace kin::gpu
