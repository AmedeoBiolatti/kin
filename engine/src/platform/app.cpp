#include <kin/platform/app.hpp>

#include <kin/platform/log.hpp>

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
    : _config(config) {
    set_max_fps(config.max_fps);
    // KIN_RENDER_BACKEND=gpu keeps the real video driver even headless, so a
    // --server --render capture can go through the GPU backend the game
    // actually ships on (its window is created hidden either way).
    const char* backend = std::getenv("KIN_RENDER_BACKEND");
    if (headless() && !(backend && std::string_view(backend) == "gpu")) {
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
            _running = false;
        } else if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            handle_window_close(static_cast<WindowId>(event.window.windowID));
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

void App::run(std::function<void(f32 dt)> update, std::function<void(f32 alpha)> render) {
    _running = true;
    const f32 fixed_dt = _config.fixed_dt > 0.0f ? _config.fixed_dt : default_fixed_dt;
    const f32 max_frame_time = std::max(0.0f, _config.max_frame_time);
    const i32 max_steps = std::max(1, _config.max_steps);
    f32 accumulator = 0.0f;
    FrameTimeSnapper snapper;
    u64 last = SDL_GetTicksNS();
    u64 next_frame_ns = 0; // when the next frame may start, under max_fps
    bool advance_input = true;
    i32 frames = 0;

    KIN_LOG_INFO_F("runtime",
                   "app loop started",
                   (LogFields{
                       {.name = "mode", .value = std::string{app_mode_name(_config.mode)}},
                       {.name = "fixed_dt", .value = std::to_string(fixed_dt)},
                       {.name = "max_frame_time", .value = std::to_string(max_frame_time)},
                       {.name = "max_steps", .value = std::to_string(max_steps)},
                   }));

    while (_running) {
        ++frames;
        const u64 now = SDL_GetTicksNS();
        const f32 raw_frame_time = static_cast<f32>(now - last) / 1'000'000'000.0f;
        last = now;
        const f32 frame_time = std::min(raw_frame_time, max_frame_time);
        const f32 pacing_wait = _frame_stats.pacing_wait;
        // time_scale paces real time into the accumulator (game-speed control);
        // it never changes fixed_dt or step contents, so determinism holds.
        const f32 snapped = snapper.advance(frame_time, fixed_dt, _config.snap_tolerance);
        accumulator += snapped * _time_scale;
        _frame_stats = {
            .raw_frame_time = raw_frame_time,
            .clamped_frame_time = frame_time,
            .accumulator_before_update = accumulator,
            .accumulator_after_update = accumulator,
            .alpha = 0.0f,
            .update_steps = 0,
            .hit_max_steps = false,
            .pacing_wait = pacing_wait,
            .snapped_frame_time = snapped,
        };

        _input.begin_frame(advance_input);
        pump_events();

        i32 steps = 0;
        while (_running && accumulator >= fixed_dt && steps < max_steps) {
            // Counts the step being run, so update() sees 1 in a frame's first step,
            // as under run_for.
            _frame_stats.update_steps = steps + 1;
            update(fixed_dt);
            accumulator -= fixed_dt;
            ++steps;
            _frame_stats.accumulator_after_update = accumulator;
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
        if (steps == max_steps && accumulator >= fixed_dt) {
            // We exhausted the step budget with time still backlogged: drop it to
            // avoid a spiral of death. If the loop instead drained naturally on
            // its last allowed step (accumulator < fixed_dt), there is no backlog
            // to discard and the leftover feeds the render interpolation alpha.
            accumulator = 0.0f;
            snapper.reset();
            _frame_stats.hit_max_steps = true;
            _frame_stats.accumulator_after_update = accumulator;
        }

        if (_running) {
            _frame_stats.alpha = fixed_dt > 0.0f ? accumulator / fixed_dt : 0.0f;
            render(_frame_stats.alpha);
        }

        if (!headless()) {
            pace_frame(next_frame_ns);
        }
        advance_input = steps > 0;
    }
    KIN_LOG_INFO_F("runtime",
                   "app loop stopped",
                   (LogFields{{.name = "frames", .value = std::to_string(frames)}}));
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
            .alpha = 0.0f,
            .update_steps = 1,
            .hit_max_steps = false,
        };
        _input.begin_frame();
        pump_events();
        update(_config.fixed_dt, frame);
        if (_running) {
            render(0.0f);
        }
    }
    KIN_LOG_INFO_F("runtime",
                   "fixed frame run stopped",
                   (LogFields{{.name = "frames", .value = std::to_string(frames_run)}}));
}

} // namespace kin
