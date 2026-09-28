#pragma once

#include <kin/core/types.hpp>
#include <kin/platform/app.hpp>
#include <kin/platform/input.hpp>
#include <kin/platform/window.hpp>
#include <kin/renderer/renderer2d.hpp>

#include <functional>
#include <optional>
#include <string_view>

namespace kin {

struct FrameContext;

struct WindowedAppConfig {
    std::string_view title = "Kin";
    i32 width = 1280;
    i32 height = 720;
    i32 logical_width = 0;
    i32 logical_height = 0;
    bool integer_scale = false;
    AppMode mode = AppMode::Windowed;
    f32 fixed_dt = default_fixed_dt;
    f32 max_frame_time = 0.25f;
    i32 max_steps = 8;
    bool vsync = false;
    bool yield_when_unpaced = true;
    i32 max_frames = 0;
    bool resizable = false;
    bool maximized = false;
    bool fullscreen = false;
    bool hidden = false;
    bool borderless = false;
    bool high_pixel_density = false;
    std::optional<InputMap> input_map;
    // Called after the loop exits while the renderer and its device are still
    // alive. Scene runtimes use this to release scene-owned GPU/SDL resources.
    std::function<void(FrameContext&)> shutdown;
};

struct FrameContext {
    App& app;
    Window& window;
    Renderer2D& renderer;
    Input& input;
    f32 dt = 0.0f;
    f32 alpha = 0.0f;
};

using FrameUpdate = std::function<void(FrameContext&)>;
using FrameRender = std::function<void(FrameContext&)>;

void run_windowed_app(const WindowedAppConfig& config, FrameUpdate update, FrameRender render);

} // namespace kin
