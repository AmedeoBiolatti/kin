#pragma once

// kin's window coordinates are points: Window::size(), the mouse, the fingers,
// the safe area. Where SDL gives a window in pixels with the screen's density
// apart (Android: a 450 dpi phone is 2.8 pixels a point), kin divides SDL's
// coordinates by that density, as macOS and iOS give them, so UI laid out in
// points is as big on a phone as on a desktop and drawn at the phone's pixels
// (Window::pixel_size()). Elsewhere SDL's are kin's (one unit a point).
//
// KIN_WINDOW_POINT_SCALE=<n> sets it on any system: a desktop window then
// shows a game as a screen of that density would (tests, a phone's look).

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdlib>

namespace kin::detail {

inline float window_point_scale_override() {
    static const float value = [] {
        const char* text = std::getenv("KIN_WINDOW_POINT_SCALE");
        const float parsed = text ? std::strtof(text, nullptr) : 0.0f;
        return std::isfinite(parsed) && parsed >= 0.25f && parsed <= 8.0f ? parsed : 0.0f;
    }();
    return value;
}

// SDL's window units per kin point, for `window` (null: the override or 1).
inline float window_units_per_point(SDL_Window* window) {
    if (const float forced = window_point_scale_override(); forced > 0.0f) {
        return forced;
    }
#if defined(SDL_PLATFORM_ANDROID)
    // A pixel density of 1 there: the display scale is the screen's density.
    const float scale = window ? SDL_GetWindowDisplayScale(window) : 0.0f;
    return std::isfinite(scale) && scale > 0.0f ? scale : 1.0f;
#else
    (void)window;
    return 1.0f;
#endif
}

inline float window_units_per_point(SDL_WindowID id) {
    return window_units_per_point(id ? SDL_GetWindowFromID(id) : nullptr);
}

} // namespace kin::detail
