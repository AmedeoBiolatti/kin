#pragma once
// GPU time per frame without timestamp queries, which SDL_GPU does not have. Each
// presented frame's command buffer carries a fence; a helper thread polls the
// fences in submission order and notes when each signals. A frame's GPU time is
// then signal time - max(its first submit, the previous frame's signal): the span
// the GPU spent on that frame's command buffers, plus any gaps between them
// while the CPU was still recording.
//
// The thread only ever calls SDL_QueryGPUFence, a plain status read. Waiting with
// SDL_WaitForGPUFences there would also run SDL's cleanup (finished command
// buffers, pending resource destroys) on this thread, at moments the render
// thread does not expect: on Vulkan that lost the device under load.
#include <kin/core/types.hpp>

#include <SDL3/SDL.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>

namespace kin::gpu {

// A finished frame's GPU time. When frames went untimed before it (the timer
// was full), its span covers them too: `frames` of them in `ms` together.
struct GpuFrameSample {
    f64 ms = 0.0;
    u32 frames = 1;
};

class GpuFrameTimer {
public:
    explicit GpuFrameTimer(SDL_GPUDevice* device);
    ~GpuFrameTimer(); // waits (on the calling thread) for the frames still on the GPU

    GpuFrameTimer(const GpuFrameTimer&) = delete;
    GpuFrameTimer& operator=(const GpuFrameTimer&) = delete;

    // Whether enough frames already wait for timing (a GPU behind the CPU): the
    // next frame should then be submitted without a fence, and go untimed. Asked
    // before submitting, since a fence must not be released before it signals:
    // SDL would put it back in its pool and reset it for another submission
    // while the GPU still runs this one (on Vulkan, a lost device).
    bool full() const;

    // Takes `fence`, from a frame whose first command buffer was submitted at
    // `first_submit_ns` (SDL_GetTicksNS), after `untimed` frames submitted
    // without one. Call only when not full().
    void track(SDL_GPUFence* fence, u64 first_submit_ns, u32 untimed = 0);

    // The latest frame that finished since the last call, if any. Releases the
    // finished frames' fences.
    std::optional<GpuFrameSample> collect();

private:
    struct Pending {
        SDL_GPUFence* fence = nullptr;
        u64 first_submit_ns = 0;
        u32 frames = 1;    // this one and the untimed ones before it
        f64 gpu_ms = -1.0; // set once the fence signals
    };

    void run();

    SDL_GPUDevice* _device = nullptr;
    mutable std::mutex _mutex;
    std::condition_variable _wake;
    std::deque<Pending> _waiting;  // oldest first; the thread waits on the front
    std::deque<Pending> _finished; // signalled, fence not yet released
    u64 _last_signal_ns = 0;       // thread only
    bool _stop = false;
    std::thread _thread;
};

} // namespace kin::gpu
