#include <kin/platform/log.hpp>
#include <kin/runtime/windowed_app.hpp>
#include <string>
#include <utility>

namespace kin {
namespace {
AppConfig app_config(const WindowedAppConfig& config) {
    return {.mode = config.mode,
            .fixed_dt = config.fixed_dt > 0 ? config.fixed_dt : default_fixed_dt,
            .max_frame_time = config.max_frame_time,
            .max_steps = config.max_steps,
            .vsync = config.vsync,
            .yield_when_unpaced = config.yield_when_unpaced,
            .max_fps = config.max_fps,
            .snap_tolerance = config.snap_tolerance};
}
WindowConfig window_config(const WindowedAppConfig& config) {
    return {.title = config.title,
            .width = config.width,
            .height = config.height,
            .resizable = config.resizable,
            .maximized = config.maximized,
            .fullscreen = config.fullscreen,
            .hidden = true,
            .borderless = config.borderless,
            .high_pixel_density = (config.logical_width > 0 && config.logical_height > 0) || config.high_pixel_density};
}
} // namespace

struct WindowedAppSession::Impl {
    std::string title;
    WindowedAppConfig config;
    App app;
    Window& window;
    Renderer2D renderer;
    FrameUpdate update;
    FrameRender render;
    bool finished = false;
    i32 rendered = 0;

    Impl(WindowedAppConfig requested, FrameUpdate update_callback, FrameRender render_callback)
        : title(requested.title), config(std::move(requested)), app(app_config(config)),
          window(app.create_window(window_config(config))),
          renderer(make_render_backend(window, config.vsync && !app.headless(), !app.headless())),
          update(std::move(update_callback)), render(std::move(render_callback)) {
        config.title = title;
        if (config.color_space != ColorSpace::Gamma || config.hdr)
            renderer.set_color_space(config.color_space, config.hdr);
        if (!(config.hidden || app.headless()))
            window.show();
        if (config.logical_width > 0 && config.logical_height > 0) {
            if (config.integer_scale)
                renderer.set_integer_logical_size(config.logical_width, config.logical_height);
            else
                renderer.set_logical_size(config.logical_width, config.logical_height);
        }
        if (config.input_map)
            app.input().set_map(*config.input_map);
        app.start_loop([this](f32 dt) { on_update(dt); }, [this](f32 alpha) { on_render(alpha); });
    }
    ~Impl() {
        try {
            finish();
        } catch (const std::exception& error) {
            KIN_LOG_ERROR("runtime", error.what());
        } catch (...) {
            KIN_LOG_ERROR("runtime", "windowed session shutdown failed");
        }
    }
    FrameContext context(f32 dt, f32 alpha) {
        return {.app = app, .window = window, .renderer = renderer, .input = app.input(), .dt = dt, .alpha = alpha};
    }
    void on_update(f32 dt) {
        auto frame = context(dt, 0);
        update(frame);
    }
    void on_render(f32 alpha) {
        auto frame = context((config.fixed_dt > 0 ? config.fixed_dt : default_fixed_dt), alpha);
        render(frame);
        if (config.max_frames > 0 && ++rendered >= config.max_frames)
            app.quit();
    }
    void finish() {
        if (finished)
            return;
        finished = true;
        app.quit();
        auto shutdown = std::move(config.shutdown);
        if (shutdown) {
            auto frame = context((config.fixed_dt > 0 ? config.fixed_dt : default_fixed_dt), 0);
            shutdown(frame);
        }
        update = {};
        render = {};
    }
};

WindowedAppSession::WindowedAppSession(WindowedAppConfig config, FrameUpdate update, FrameRender render)
    : _impl(std::make_unique<Impl>(std::move(config), std::move(update), std::move(render))) {}
WindowedAppSession::~WindowedAppSession() = default;

bool WindowedAppSession::step(f32 elapsed_seconds) {
    if (_impl->finished)
        return false;
    if (!_impl->app.step_loop(elapsed_seconds)) {
        finish();
        return false;
    }
    return true;
}
void WindowedAppSession::run() {
    if (_impl->finished)
        return;
    if (_impl->config.max_frames > 0) {
        _impl->app.run_for(
            _impl->config.max_frames, [this](f32 dt, i32) { _impl->on_update(dt); },
            [this](f32 alpha) { _impl->on_render(alpha); });
    } else {
        _impl->app.run([this](f32 dt) { _impl->on_update(dt); }, [this](f32 alpha) { _impl->on_render(alpha); });
    }
    finish();
}
void WindowedAppSession::finish() {
    _impl->finish();
}
App& WindowedAppSession::app() {
    return _impl->app;
}
Window& WindowedAppSession::window() {
    return _impl->window;
}
Renderer2D& WindowedAppSession::renderer() {
    return _impl->renderer;
}

void run_windowed_app(const WindowedAppConfig& config, FrameUpdate update, FrameRender render) {
    WindowedAppSession session(config, std::move(update), std::move(render));
    session.run();
}
} // namespace kin
