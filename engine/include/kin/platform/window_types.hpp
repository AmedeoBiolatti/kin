#pragma once

#include <kin/core/types.hpp>

#include <string_view>
#include <vector>

namespace kin {

using WindowId = u32;

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

} // namespace kin
