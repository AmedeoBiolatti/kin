#pragma once
// GPU time per frame, and per named scope, without timestamp queries, which
// SDL_GPU does not have. Each presented frame's command buffer carries a fence,
// and so does each side of a scope (Renderer2D::gpu_scope, which splits the
// frame's submission there); a helper thread polls the fences in submission
// order and notes when each signals.
//
// A frame's GPU time is its fence's signal - max(its first submit, the previous
// frame's signal): the span the GPU spent on its command buffers, plus any gaps
// while the CPU was still recording. A scope's is its end fence's signal -
// max(its start fence's signal, the submit of its own command buffer).
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
#include <string>
#include <thread>
#include <vector>

namespace kin::gpu {

// A finished frame's GPU time. When frames went untimed before it (the timer
// was full), its span covers them too: `frames` of them in `ms` together.
struct GpuFrameSample {
    f64 ms = 0.0;
    u32 frames = 1;
};

struct GpuScopeSample {
    std::string name;
    f64 ms = 0.0;
};

// What finished since the last collect(): the latest frame, and every scope.
struct GpuTimerSamples {
    std::optional<GpuFrameSample> frame;
    std::vector<GpuScopeSample> scopes;
};

class GpuFrameTimer {
public:
    explicit GpuFrameTimer(SDL_GPUDevice* device);
    ~GpuFrameTimer(); // waits (on the calling thread) for the fences still on the GPU

    GpuFrameTimer(const GpuFrameTimer&) = delete;
    GpuFrameTimer& operator=(const GpuFrameTimer&) = delete;

    // Whether `fences` more would go past what may wait at once (a GPU behind
    // the CPU): the frame should then be submitted without a fence, and go
    // untimed, or the scope not be timed. Asked before submitting, since a fence
    // must not be released before it signals: SDL would put it back in its pool
    // and reset it for another submission while the GPU still runs this one (on
    // Vulkan, a lost device).
    bool full(std::size_t fences = 1) const;

    // Takes `fence`, from a frame whose first command buffer was submitted at
    // `first_submit_ns` (SDL_GetTicksNS), after `untimed` frames submitted
    // without one. Call only when not full().
    void track(SDL_GPUFence* fence, u64 first_submit_ns, u32 untimed = 0);
    // Takes the fences on each side of a scope: the start's, submitted after the
    // work before it (check full(2) first, for both), then the end's, after its
    // own, whose command buffer was submitted at `end_submit_ns`. Scopes do not
    // nest, and a frame's fence comes after its scopes'.
    void track_scope_start(SDL_GPUFence* fence);
    void track_scope_end(std::string name, SDL_GPUFence* fence, u64 end_submit_ns);

    // What finished since the last call. Releases the finished fences.
    GpuTimerSamples collect();

private:
    enum class Kind : u8 { Frame, ScopeStart, ScopeEnd };
    struct Pending {
        SDL_GPUFence* fence = nullptr;
        Kind kind = Kind::Frame;
        u64 submit_ns = 0;  // the frame's first submit, or the scope's own
        u32 frames = 1;     // a frame: this one and the untimed ones before it
        std::string name;   // a scope's
        u64 signal_ns = 0;  // set once the fence signals
    };

    void push(Pending pending);
    void run();

    SDL_GPUDevice* _device = nullptr;
    mutable std::mutex _mutex;
    std::condition_variable _wake;
    std::deque<Pending> _waiting;  // oldest first; the thread polls the front
    std::deque<Pending> _finished; // signalled, fence not yet released
    u64 _last_frame_signal_ns = 0; // collect() only
    u64 _scope_start_signal_ns = 0; // collect() only
    bool _stop = false;
    std::thread _thread;
};

} // namespace kin::gpu
