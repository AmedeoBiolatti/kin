#include <kin/runtime/windowed_app.hpp>

namespace kin {

void run_windowed_app(const WindowedAppConfig& config, FrameUpdate update, FrameRender render) {
    {
        const f32 fixed_dt = config.fixed_dt > 0.0f ? config.fixed_dt : default_fixed_dt;
        App app{{
            .mode = config.mode,
            .fixed_dt = fixed_dt,
            .max_frame_time = config.max_frame_time,
            .max_steps = config.max_steps,
            .vsync = config.vsync,
            .yield_when_unpaced = config.yield_when_unpaced,
            .max_fps = config.max_fps,
            .snap_tolerance = config.snap_tolerance,
        }};

        // Create hidden, then show after the renderer is built: the SDL_GPU backend can
        // only claim a window's swapchain while it is hidden, so this keeps GPU + SDL
        // selection uniform. (No visible change for the SDL backend.)
        const bool want_visible = !(config.hidden || app.headless());
        // Logical games opt in automatically; native UI can opt in explicitly
        // and size its physical-pixel content using Window::display_scale().
        const bool uses_logical_presentation = config.logical_width > 0 && config.logical_height > 0;
        Window& window = app.create_window({
            .title = config.title,
            .width = config.width,
            .height = config.height,
            .resizable = config.resizable,
            .maximized = config.maximized,
            .fullscreen = config.fullscreen,
            .hidden = true,
            .borderless = config.borderless,
            .high_pixel_density = uses_logical_presentation || config.high_pixel_density,
        });

        Renderer2D renderer{make_render_backend(window, config.vsync && !app.headless(), !app.headless())};
        if (config.color_space != ColorSpace::Gamma || config.hdr) {
            renderer.set_color_space(config.color_space, config.hdr);
        }
        if (want_visible) {
            window.show();
        }
        if (config.logical_width > 0 && config.logical_height > 0) {
            if (config.integer_scale) {
                renderer.set_integer_logical_size(config.logical_width, config.logical_height);
            } else {
                renderer.set_logical_size(config.logical_width, config.logical_height);
            }
        }

        if (config.input_map) {
            app.input().set_map(*config.input_map);
        }

        auto make_context = [&](f32 dt, f32 alpha) {
            FrameContext ctx{
                .app = app,
                .window = window,
                .renderer = renderer,
                .input = app.input(),
                .dt = dt,
                .alpha = alpha,
            };
            return ctx;
        };

        if (config.max_frames > 0) {
            app.run_for(config.max_frames, [&](f32 dt, i32) {
                FrameContext ctx = make_context(dt, 0.0f);
                update(ctx);
            }, [&](f32 alpha) {
                FrameContext ctx = make_context(fixed_dt, alpha);
                render(ctx);
            });
        } else {
            app.run([&](f32 dt) {
                FrameContext ctx = make_context(dt, 0.0f);
                update(ctx);
            }, [&](f32 alpha) {
                FrameContext ctx = make_context(fixed_dt, alpha);
                render(ctx);
            });
        }
        if (config.shutdown) {
            FrameContext ctx = make_context(fixed_dt, 0.0f);
            config.shutdown(ctx);
        }
    } // App + window + renderer (GpuDevice) fully torn down here, and SDL_Quit has run.

}

} // namespace kin
