#pragma once

#include <kin/core/types.hpp>

#include <array>
#include <memory>
#include <string_view>
#include <vector>

namespace kin {

using WindowId = u32;

class SdlRenderer2DBackend;
struct Image;
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

// The pointer's shape while it is over a window (Window::set_cursor).
enum class SystemCursor : u8 {
    Default,    // the arrow
    Text,       // an I-beam, over text that can be edited or selected
    Pointer,    // a hand, over a link
    Move,
    ResizeEW,   // left-right, over a vertical splitter
    ResizeNS,   // up-down, over a horizontal splitter
    ResizeNWSE,
    ResizeNESW,
    NotAllowed,
    Wait,
    Crosshair,
};

// A window without the system's title bar (WindowConfig::borderless) draws its
// own; this tells the system what its parts do. Rects are in window coordinates.
struct WindowChrome {
    std::vector<Rectf> drag;    // pressing here moves the window (a double click maximizes it, where the system does)
    std::vector<Rectf> exclude; // not dragging though inside `drag`: the buttons on a title bar
    f32 resize_border = 0.0f;   // this close to an edge resizes a resizable window; 0 for none
};

// What pressing a point of a window does, as the system sees it.
enum class WindowArea : u8 {
    Client, // the app's: clicks go to it
    Drag,
    ResizeTopLeft,
    ResizeTop,
    ResizeTopRight,
    ResizeRight,
    ResizeBottomRight,
    ResizeBottom,
    ResizeBottomLeft,
    ResizeLeft,
};

// The area under `point` of a window of `size` with `chrome`: an edge (when it
// can be resized), else a drag rect not excluded, else the client.
WindowArea window_area_at(const WindowChrome& chrome, Vec2f point, Vec2f size, bool resizable);

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
    // Waits (a moment at most) until what was asked of the window is done:
    // some systems (X11) size, move and show it later. False when it timed out.
    bool sync();
    bool borderless() const;
    // Desktop (borderless, display-sized) fullscreen, toggled at runtime.
    bool fullscreen() const;
    // The icon the system shows for it (its title bar, the task bar); RGBA.
    void set_icon(const Image& image);
    void set_fullscreen(bool enabled);

    bool close_requested() const { return _close_requested; }
    void request_close() { _close_requested = true; }

    // The title bar's buttons, for a window that draws its own.
    void minimize();
    void toggle_maximized(); // maximized, or back to its size before
    bool maximized() const;
    // What its own title bar does (see WindowChrome); replaces the last. Call it
    // each frame (ui2::Context::apply does), as the bar's layout may change.
    void set_chrome(WindowChrome chrome);
    struct Chrome; // its state, for the hit test in window.cpp

    void show();
    void hide();

    // Relative mouse mode (a first-person camera): the pointer hidden and held
    // in the window, and Input::mouse_motion() still reporting every movement.
    // SDL lets it go while the window is out of focus and takes it back after.
    void set_relative_mouse(bool enabled);
    bool relative_mouse() const;

    void set_text_input_enabled(bool enabled);
    bool text_input_enabled() const;
    // Where text is being typed, in window coordinates, so an input method
    // puts its candidate list beside it rather than in a corner. `cursor` is
    // the caret's x within `area`.
    void set_text_input_area(Rectf area, i32 cursor = 0);

    // The pointer's shape while it is over this window. The pointer is shared
    // by every window, so this changes it only while it is over this one; call
    // it each frame (ui2::Context::apply does) and it takes effect on entering.
    void set_cursor(SystemCursor cursor);

private:
    friend class SdlRenderer2DBackend;
    friend class WebGL2Renderer2DBackend;
    friend class WebGLDevice;
    friend class CompactRenderer2DBackend;
    friend class gpu::GpuDevice;
    friend class FileDialogs;

    void* native_handle() const { return _handle; }

    void* _handle = nullptr;
    bool _close_requested = false;
    std::array<void*, 11> _cursors{}; // made on first use, one per SystemCursor
    std::unique_ptr<Chrome> _chrome; // read by the system's hit test, from the event thread
};

} // namespace kin
