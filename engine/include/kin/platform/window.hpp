#pragma once

#include <kin/core/types.hpp>

#include <string_view>

namespace kin {

using WindowId = u32;

class SdlRenderer2DBackend;
namespace gpu {
class GpuDevice;
}

struct WindowConfig {
    std::string_view title = "Kin";
    i32 width = 1280;
    i32 height = 720;
    bool resizable = false;
    bool maximized = false;
    bool fullscreen = false;
    bool hidden = false;
    // Drop the native OS title bar / window border (SDL_WINDOW_BORDERLESS). The
    // app is expected to draw its own chrome (and provide its own close path).
    bool borderless = false;
    // Request a backbuffer at the display's physical pixel resolution rather than
    // logical points. On a DPI-scaled display this keeps rendering crisp (the game
    // upscales through its logical presentation; native-coordinate UI like the
    // debug overlay draws at full native resolution) instead of being rendered at
    // point resolution and bilinearly upscaled by the OS.
    //
    // Without logical presentation, applications must lay out in physical pixels
    // and use display_scale() for the size of UI content.
    bool high_pixel_density = false;
};

// Usable bounds of the primary display in logical points, excluding desktop
// chrome such as taskbars. Returns {0, 0} when no display can be queried, so a
// caller must always have a fallback size.
//
// Safe to call before an App exists: it initialises the video subsystem only if
// nobody else has, and releases it again if so.
Vec2i primary_display_size();

class Window {
public:
    explicit Window(const WindowConfig& config);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    Window(Window&&) = delete;
    Window& operator=(Window&&) = delete;

    WindowId id() const;
    i32 width() const;
    i32 height() const;
    Vec2i size() const;
    Vec2i pixel_size() const;
    f32 display_scale() const;
    void set_minimum_size(Vec2i size);
    void set_size(Vec2i size);
    void set_size(i32 width, i32 height);
    bool borderless() const;
    // Desktop (borderless, display-sized) fullscreen, toggled at runtime.
    bool fullscreen() const;
    void set_fullscreen(bool enabled);

    bool close_requested() const { return _close_requested; }
    void request_close() { _close_requested = true; }

    void show();
    void hide();

    void set_text_input_enabled(bool enabled);
    bool text_input_enabled() const;
    // Where text is being typed, in window coordinates, so an input method
    // puts its candidate list beside it rather than in a corner. `cursor` is
    // the caret's x within `area`.
    void set_text_input_area(Rectf area, i32 cursor = 0);

private:
    friend class SdlRenderer2DBackend;
    friend class gpu::GpuDevice;
    friend class FileDialogs;

    void* native_handle() const { return _handle; }

    void* _handle = nullptr;
    bool _close_requested = false;
};

} // namespace kin
