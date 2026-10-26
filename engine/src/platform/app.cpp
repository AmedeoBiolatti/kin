#include <kin/platform/app.hpp>

#include <kin/platform/log.hpp>
#include <kin/renderer/build.hpp>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string_view>
#include <stdexcept>
#include <string>
#include <utility>

namespace kin {
namespace {

std::string_view app_mode_name(AppMode mode) {
    return mode == AppMode::Headless ? "headless" : "windowed";
}

} // namespace

App::App(AppConfig config)
    : _config(config), _wait_for_events(config.wait_for_events) {
    set_max_fps(config.max_fps);
    // A GPU-only headless app still needs a real driver and a hidden window.
    const auto renderer = requested_renderer();
    if (headless() && renderer != RendererRequest::Gpu) {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        const std::string error = std::string("SDL_Init failed: ") + SDL_GetError();
        KIN_LOG_ERROR_F("runtime",
                        "app initialization failed",
                        (LogFields{
                            {.name = "mode", .value = std::string{app_mode_name(_config.mode)}},
                            {.name = "error", .value = error},
                        }));
        throw std::runtime_error(error);
    }
    KIN_LOG_INFO_F("runtime",
                   "app initialized",
                   (LogFields{
                       {.name = "mode", .value = std::string{app_mode_name(_config.mode)}},
                       {.name = "fixed_dt", .value = std::to_string(_config.fixed_dt)},
                       {.name = "max_frame_time", .value = std::to_string(_config.max_frame_time)},
                       {.name = "max_steps", .value = std::to_string(_config.max_steps)},
                       {.name = "vsync", .value = _config.vsync ? "true" : "false"},
                       {.name = "max_fps", .value = std::to_string(_config.max_fps)},
                   }));
}

App::~App() {
    KIN_LOG_INFO_F("runtime",
                   "app shutdown",
                   (LogFields{{.name = "windows", .value = std::to_string(_windows.size())}}));
    _update = {};
    _render = {};
    _windows.clear();
    SDL_Quit();
}

Window& App::create_window(const WindowConfig& config) {
    auto window = std::make_unique<Window>(config);
    Window& result = *window;
    _windows.push_back(std::move(window));
    KIN_LOG_INFO_F("runtime",
                   "window created",
                   (LogFields{
                       {.name = "title", .value = std::string{config.title}},
                       {.name = "width", .value = std::to_string(config.width)},
                       {.name = "height", .value = std::to_string(config.height)},
                       {.name = "resizable", .value = config.resizable ? "true" : "false"},
                       {.name = "hidden", .value = config.hidden ? "true" : "false"},
                       {.name = "borderless", .value = config.borderless ? "true" : "false"},
                       {.name = "id", .value = std::to_string(result.id())},
                   }));
    return result;
}

Window* App::find_window(WindowId id) {
    const auto found = std::ranges::find_if(_windows, [id](const auto& window) {
        return window->id() == id;
    });

    return found == _windows.end() ? nullptr : found->get();
}

const Window* App::find_window(WindowId id) const {
    const auto found = std::ranges::find_if(_windows, [id](const auto& window) {
        return window->id() == id;
    });

    return found == _windows.end() ? nullptr : found->get();
}

i32 App::window_count() const {
    return static_cast<i32>(_windows.size());
}

i32 App::open_window_count() const {
    return static_cast<i32>(std::ranges::count_if(_windows, [](const auto& window) {
        return !window->close_requested();
    }));
}

void App::handle_window_close(WindowId window_id) {
    Window* window = find_window(window_id);
    if (window) {
        window->request_close();
    }

    if (open_window_count() == 0) {
        _running = false;
    }
}

void App::pump_events() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        _input.process_native_event(&event);

        if (event.type == SDL_EVENT_QUIT) {
            if (_hold_close) {
                _close_asked = true;
            } else {
                _running = false;
            }
        } else if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            if (_hold_close && open_window_count() <= 1) {
                _close_asked = true; // the last one: the app decides
            } else {
                handle_window_close(static_cast<WindowId>(event.window.windowID));
            }
        }
    }
}

f32 FrameTimeSnapper::advance(f32 frame_time, f32 fixed_dt, f32 tolerance) {
    if (tolerance <= 0.0f || fixed_dt <= 0.0f) {
        return frame_time;
    }
    const f64 steps = std::round(static_cast<f64>(frame_time) / fixed_dt);
    const f64 snapped = steps * fixed_dt;
    if (steps < 1.0 || std::abs(frame_time - snapped) > tolerance) {
        return frame_time;
    }
    _debt += frame_time - snapped;
    // Paid back in whole steps, so the accumulator's remainder (and with it
    // the steps per frame and the render alpha) stays where it was.
    f64 paid = 0.0;
    if (_debt >= fixed_dt) {
        paid = fixed_dt;
    } else if (_debt <= -fixed_dt) {
        paid = -static_cast<f64>(fixed_dt);
    }
    _debt -= paid;
    return static_cast<f32>(snapped + paid);
}

void App::start_loop(std::function<void(f32)> update, std::function<void(f32)> render) {
    _update = std::move(update);
    _render = std::move(render);
    _accumulator = 0.0f;
    _snapper.reset();
    _advance_input = true;
    _frame_stats = {};
    _running = true;
    KIN_LOG_INFO_F(
        "runtime", "app loop started",
        (LogFields{
            {.name = "mode", .value = std::string{app_mode_name(_config.mode)}},
            {.name = "fixed_dt", .value = std::to_string(_config.fixed_dt > 0 ? _config.fixed_dt : default_fixed_dt)},
            {.name = "max_frame_time", .value = std::to_string(std::max(0.0f, _config.max_frame_time))},
            {.name = "max_steps", .value = std::to_string(std::max(1, _config.max_steps))},
        }));
}

bool App::step_loop(f32 elapsed_seconds) {
    if (!_running) {
        return false;
    }
    const f32 fixed_dt = _config.fixed_dt > 0.0f ? _config.fixed_dt : default_fixed_dt;
    const f32 max_frame_time = std::max(0.0f, _config.max_frame_time);
    const i32 max_steps = std::max(1, _config.max_steps);
    const f32 raw_frame_time = std::isfinite(elapsed_seconds) ? std::max(0.0f, elapsed_seconds) : 0.0f;
    const f32 frame_time = std::min(raw_frame_time, max_frame_time);
    const f32 pacing_wait = _frame_stats.pacing_wait;
    // time_scale paces real time into the accumulator (game-speed control);
    // it never changes fixed_dt or step contents, so determinism holds.
    const f32 snapped = _snapper.advance(frame_time, fixed_dt, _config.snap_tolerance);
    _accumulator += snapped * _time_scale;
    _frame_stats = {
        .raw_frame_time = raw_frame_time,
        .clamped_frame_time = frame_time,
        .accumulator_before_update = _accumulator,
        .accumulator_after_update = _accumulator,
        .alpha = 0.0f,
        .update_steps = 0,
        .hit_max_steps = false,
        .pacing_wait = pacing_wait,
        .snapped_frame_time = snapped,
        .idle_wait = std::exchange(_idle_wait, 0.0f),
    };

    _input.begin_frame(_advance_input);
    pump_events();

    i32 steps = 0;
    while (_running && _accumulator >= fixed_dt && steps < max_steps) {
        // Counts the step being run, so update() sees 1 in a frame's first step,
        // as under run_for.
        _frame_stats.update_steps = steps + 1;
        _update(fixed_dt);
        _accumulator -= fixed_dt;
        ++steps;
        _frame_stats.accumulator_after_update = _accumulator;
        if (steps == 1) {
            // Consume the KEYBOARD edges and the step wheel after the FIRST fixed
            // step so navigation read in update() does not re-fire in later steps
            // or in render() (ui2's MenuScene reads nav in both update() and
            // render()), and a wheel notch reaches exactly one step. Mouse, text
            // and frame-wheel edges are intentionally preserved: render-time
            // immediate UI (ui2 reads clicks via mouse_frame_pressed in render())
            // is the sole consumer of those, so clearing them here dropped roughly
            // half of all clicks — whichever frames ran a fixed step. Held state in
            // _key_cur/_mouse_cur is left intact regardless.
            _input.advance_step_edges();
        }
    }
    if (steps == max_steps && _accumulator >= fixed_dt) {
        // We exhausted the step budget with time still backlogged: drop it to
        // avoid a spiral of death. If the loop instead drained naturally on
        // its last allowed step (_accumulator < fixed_dt), there is no backlog
        // to discard and the leftover feeds the render interpolation alpha.
        _accumulator = 0.0f;
        _snapper.reset();
        _frame_stats.hit_max_steps = true;
        _frame_stats.accumulator_after_update = _accumulator;
    }

    if (_running) {
        _frame_stats.alpha = fixed_dt > 0.0f ? _accumulator / fixed_dt : 0.0f;
        _render(_frame_stats.alpha);
    }
    _advance_input = steps > 0;
    return _running;
}

void App::run(std::function<void(f32)> update, std::function<void(f32)> render) {
#ifdef __EMSCRIPTEN__
    if (!headless()) {
        throw std::logic_error("Browser apps must drive start_loop/step_loop from requestAnimationFrame");
    }
#endif
    start_loop(std::move(update), std::move(render));
    i32 frames = 0;
    u64 last = SDL_GetTicksNS();
    u64 next_frame_ns = 0;
    while (_running) {
        ++frames;
        const u64 now = SDL_GetTicksNS();
        step_loop(static_cast<f32>(now - last) / 1'000'000'000.0f);
        last = now;
        if (!headless()) {
            pace_frame(next_frame_ns);
            if (_running && _wait_for_events && wait_for_change()) {
                // However long the wait, the frame it ends runs one step: the time
                // spent idle is not game time to catch up on.
                const f32 fixed_dt = _config.fixed_dt > 0.0f ? _config.fixed_dt : default_fixed_dt;
                last = SDL_GetTicksNS() - static_cast<u64>(static_cast<f64>(fixed_dt) * 1'000'000'000.0);
            }
        }
        _redraw_in = -1.0f;
    }
    _update = {};
    _render = {};
    KIN_LOG_INFO_F("runtime", "app loop stopped", (LogFields{{.name = "frames", .value = std::to_string(frames)}}));
}

void App::request_redraw(f32 seconds) {
    seconds = std::max(0.0f, seconds);
    _redraw_in = _redraw_in < 0.0f ? seconds : std::min(_redraw_in, seconds);
}

// Under wait_for_events, after a frame: blocks until an event comes, a redraw
// falls due or max_idle_wait passes. False when the next frame starts at once.
bool App::wait_for_change() {
    if (_settle_frames > 0) {
        --_settle_frames;
        return false;
    }
    if (_redraw_in == 0.0f) {
        return false;
    }
    f32 timeout = _config.max_idle_wait > 0.0f ? _config.max_idle_wait : -1.0f;
    if (_redraw_in > 0.0f) {
        timeout = timeout < 0.0f ? _redraw_in : std::min(timeout, _redraw_in);
    }
    const u64 start = SDL_GetTicksNS();
    // A null event leaves it queued for pump_events().
    const bool event = SDL_WaitEventTimeout(nullptr, timeout < 0.0f ? -1 : static_cast<Sint32>(std::ceil(timeout * 1000.0f)));
    _idle_wait = static_cast<f32>(SDL_GetTicksNS() - start) / 1'000'000'000.0f;
    if (event) {
        _settle_frames = 2;
    }
    return true;
}

void App::pace_frame(u64& next_frame_ns) {
    _frame_stats.pacing_wait = 0.0f;
    if (_max_fps <= 0.0f) {
        next_frame_ns = 0;
        if (!_config.vsync && _config.yield_when_unpaced) {
            SDL_Delay(1);
        }
        return;
    }
    // Deadlines a period apart, not "a period after this frame ended", so the
    // rate holds without drifting. A frame past its deadline starts the next
    // at once; one more than a period past it starts the deadlines over from
    // now instead of rushing the next ones to catch up. Either way a late
    // frame is never made later by a wait.
    const u64 period = static_cast<u64>(1'000'000'000.0 / static_cast<f64>(_max_fps));
    const u64 now = SDL_GetTicksNS();
    next_frame_ns = next_frame_ns == 0 ? now : next_frame_ns + period;
    if (now > next_frame_ns + period) {
        next_frame_ns = now;
    }
    if (next_frame_ns > now) {
        SDL_DelayPrecise(next_frame_ns - now);
        _frame_stats.pacing_wait = static_cast<f32>(next_frame_ns - now) / 1'000'000'000.0f;
    }
}

void App::run_for(i32 frames, std::function<void(f32 dt, i32 frame)> update) {
    run_for(frames, std::move(update), [](f32) {});
}

void App::run_for(i32 frames,
                  std::function<void(f32 dt, i32 frame)> update,
                  std::function<void(f32 alpha)> render) {
    _running = true;
    KIN_LOG_INFO_F("runtime",
                   "fixed frame run started",
                   (LogFields{{.name = "max_frames", .value = std::to_string(frames)}}));
    i32 frames_run = 0;
    for (i32 frame = 0; _running && frame < frames; ++frame) {
        ++frames_run;
        _frame_stats = {
            .raw_frame_time = _config.fixed_dt,
            .clamped_frame_time = _config.fixed_dt,
            .accumulator_before_update = _config.fixed_dt,
            .accumulator_after_update = 0.0f,
            .alpha = 1.0f,
            .update_steps = 1,
            .hit_max_steps = false,
        };
        _input.begin_frame();
        pump_events();
        update(_config.fixed_dt, frame);
        if (_running) {
            render(1.0f);
        }
    }
    KIN_LOG_INFO_F("runtime",
                   "fixed frame run stopped",
                   (LogFields{{.name = "frames", .value = std::to_string(frames_run)}}));
}

} // namespace kin
