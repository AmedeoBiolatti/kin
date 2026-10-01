#pragma once

#include <kin/core/types.hpp>
#include <kin/platform/input.hpp>
#include <kin/platform/window.hpp>

#include <functional>
#include <memory>
#include <vector>

namespace kin {

enum class AppMode {
    Windowed,
    Headless,
};

// Default fixed-timestep duration (60 Hz) shared by the app/runtime configs and
// their run-loop fallbacks.
inline constexpr f32 default_fixed_dt = 1.0f / 60.0f;

struct AppConfig {
    AppMode mode = AppMode::Windowed;
    f32 fixed_dt = default_fixed_dt;
    f32 max_frame_time = 0.25f;
    i32 max_steps = 8;
    bool vsync = false;
    bool yield_when_unpaced = true;
    // Most frames a second a windowed run draws (0: no cap). Frames are paced
    // to deadlines, so the rate holds without drift; headless runs ignore it.
    f32 max_fps = 0.0f;
};

struct AppFrameStats {
    f32 raw_frame_time = 0.0f;
    f32 clamped_frame_time = 0.0f;
    f32 accumulator_before_update = 0.0f;
    f32 accumulator_after_update = 0.0f;
    f32 alpha = 0.0f;
    i32 update_steps = 0;
    bool hit_max_steps = false;
    f32 pacing_wait = 0.0f; // seconds slept after the previous frame to hold max_fps
};

class App {
public:
    explicit App(AppConfig config = {});
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    Window& create_window(const WindowConfig& config = {});
    Window* find_window(WindowId id);
    const Window* find_window(WindowId id) const;

    i32 window_count() const;
    i32 open_window_count() const;

    Input& input() { return _input; }
    const Input& input() const { return _input; }

    void run(std::function<void(f32 dt)> update, std::function<void(f32 alpha)> render);
    void run_for(i32 frames, std::function<void(f32 dt, i32 frame)> update);
    void run_for(i32 frames,
                 std::function<void(f32 dt, i32 frame)> update,
                 std::function<void(f32 alpha)> render);
    void quit() { _running = false; }

    bool running() const { return _running; }
    bool headless() const { return _config.mode == AppMode::Headless; }
    const AppFrameStats& frame_stats() const { return _frame_stats; }

    // Real-time pacing multiplier for the windowed fixed-timestep loop: scales
    // how fast wall-clock time feeds the step accumulator (0.5x/1x/2x/...),
    // WITHOUT changing fixed_dt or the per-step contents. Determinism is
    // unaffected (the headless run_for path ignores this entirely). Effective
    // fast-forward is bounded by max_steps. Clamped to >= 0.
    // The frame rate cap (0: none); a game's settings may change it at any time.
    f32 max_fps() const { return _max_fps; }
    void set_max_fps(f32 fps) { _max_fps = fps > 0.0f ? fps : 0.0f; }

    f32 time_scale() const { return _time_scale; }
    void set_time_scale(f32 scale) { _time_scale = scale > 0.0f ? scale : 0.0f; }

private:
    void pump_events();
    void handle_window_close(WindowId window_id);

    AppConfig _config;
    std::vector<std::unique_ptr<Window>> _windows;
    Input _input;
    AppFrameStats _frame_stats{};
    f32 _time_scale = 1.0f;
    f32 _max_fps = 0.0f;

    void pace_frame(u64& next_frame_ns);
    bool _running = false;
};

} // namespace kin
