#pragma once

#include <kin/core/types.hpp>
#include <kin/platform/window_types.hpp>

#include <array>
#include <memory>
#include <string_view>
#include <vector>

namespace kin {

class SdlRenderer2DBackend;
struct Image;
namespace gpu {
class GpuDevice;
}

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
    // Its size in points, the unit of its coordinates (the mouse's, the
    // fingers', safe_area()'s); pixel_size() is what it is drawn at. On a
    // phone a point is the screen's density of pixels (2.8 at 450 dpi), as on
    // macOS and iOS, so UI laid out in points is as big as on a desktop.
    // KIN_WINDOW_POINT_SCALE=<n> sets that density anywhere: a desktop window
    // then shows a game as a phone would.
    i32 width() const;
    i32 height() const;
    Vec2i size() const;
    Vec2i pixel_size() const;
    f32 display_scale() const;
    // Where on the window UI can be seen and touched, in the units of size():
    // the whole window except what the system covers or keeps for itself (a
    // phone's camera cutout, its bars, the strips at its edges a swipe goes
    // back from). The whole window on a desktop.
    Rectf safe_area() const;
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

    // Tells the system the rate the game draws at (0: none), so a phone's
    // display can run at it: a 120 Hz screen showing a 60 fps game switches
    // to 60 Hz, and vsync paces the game with no waiting of its own. App
    // gives it App::set_max_fps's rate. Android 11 and newer; elsewhere
    // nothing. Made again when the system gives the window a new surface.
    void hint_frame_rate(f32 fps);

    void set_text_input_enabled(bool enabled);
    bool text_input_enabled() const;
    // Where text is being typed, in window coordinates, so an input method
    // puts its candidate list beside it rather than in a corner. `cursor` is
    // the caret's x within `area`.
    void set_text_input_area(Rectf area, i32 cursor = 0);
    // The last area set_text_input_area was given (where an on-screen keyboard
    // must not cover).
    Rectf text_input_area() const { return _text_input_area; }

    // The pointer's shape while it is over this window. The pointer is shared
    // by every window, so this changes it only while it is over this one; call
    // it each frame (ui2::Context::apply does) and it takes effect on entering.
    void set_cursor(SystemCursor cursor);

private:
    friend class AccessKitAdapter;
    friend class SdlRenderer2DBackend;
    friend class WebGL2Renderer2DBackend;
    friend class WebGLDevice;
    friend class CompactRenderer2DBackend;
    friend class gpu::GpuDevice;
    friend class FileDialogs;

    void* native_handle() const { return _handle; }

    void* _handle = nullptr;
    bool _close_requested = false;
    Rectf _text_input_area{};
    std::array<void*, 11> _cursors{}; // made on first use, one per SystemCursor
    std::unique_ptr<Chrome> _chrome; // read by the system's hit test, from the event thread
    void* _rate_surface = nullptr;   // the surface hint_frame_rate last told, at _rate
    f32 _rate = -1.0f;
};

} // namespace kin
