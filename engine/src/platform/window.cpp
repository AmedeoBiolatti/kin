#include <kin/platform/window.hpp>

#include <kin/assets/image.hpp>
#include <kin/platform/log.hpp>

#include "window_points.hpp"

#include <SDL3/SDL.h>

#if defined(SDL_PLATFORM_ANDROID)
#include <android/native_window.h>
#include <dlfcn.h>
#endif

#include <algorithm>
#include <cmath>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <string>
#include <tuple>

namespace kin {
namespace {

SDL_Window* to_sdl_window(void* handle) {
    return static_cast<SDL_Window*>(handle);
}

const SDL_Window* to_sdl_window(const void* handle) {
    return static_cast<const SDL_Window*>(handle);
}

using detail::window_units_per_point;

[[maybe_unused]] bool window_point_scale_override_unset() {
    return detail::window_point_scale_override() <= 0.0f;
}

i32 to_points(int units, float scale) {
    return static_cast<i32>(std::lround(static_cast<float>(units) / scale));
}

int to_units(i32 points, float scale) {
    return static_cast<int>(std::lround(static_cast<float>(points) * scale));
}

bool inside(Rectf r, Vec2f p) {
    return p.x >= r.x && p.y >= r.y && p.x < r.x + r.w && p.y < r.y + r.h;
}

} // namespace

struct Window::Chrome {
    std::mutex mutex;
    WindowChrome chrome;
    bool installed = false;
};

namespace {

SDL_HitTestResult chrome_hit_test(SDL_Window* window, const SDL_Point* point, void* data) {
    auto* state = static_cast<Window::Chrome*>(data);
    Vec2i size{};
    SDL_GetWindowSize(window, &size.x, &size.y);
    const SDL_WindowFlags flags = SDL_GetWindowFlags(window);
    const bool resizable = (flags & SDL_WINDOW_RESIZABLE) != 0 && (flags & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN)) == 0;
    // The chrome is in points, SDL's point and size in its window units.
    const f32 scale = window_units_per_point(window);
    WindowArea area = WindowArea::Client;
    {
        const std::scoped_lock lock(state->mutex);
        area = window_area_at(state->chrome, {static_cast<f32>(point->x) / scale, static_cast<f32>(point->y) / scale},
                              {static_cast<f32>(size.x) / scale, static_cast<f32>(size.y) / scale}, resizable);
    }
    switch (area) {
    case WindowArea::Client: return SDL_HITTEST_NORMAL;
    case WindowArea::Drag: return SDL_HITTEST_DRAGGABLE;
    case WindowArea::ResizeTopLeft: return SDL_HITTEST_RESIZE_TOPLEFT;
    case WindowArea::ResizeTop: return SDL_HITTEST_RESIZE_TOP;
    case WindowArea::ResizeTopRight: return SDL_HITTEST_RESIZE_TOPRIGHT;
    case WindowArea::ResizeRight: return SDL_HITTEST_RESIZE_RIGHT;
    case WindowArea::ResizeBottomRight: return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
    case WindowArea::ResizeBottom: return SDL_HITTEST_RESIZE_BOTTOM;
    case WindowArea::ResizeBottomLeft: return SDL_HITTEST_RESIZE_BOTTOMLEFT;
    case WindowArea::ResizeLeft: return SDL_HITTEST_RESIZE_LEFT;
    }
    return SDL_HITTEST_NORMAL;
}

} // namespace

WindowArea window_area_at(const WindowChrome& chrome, Vec2f point, Vec2f size, bool resizable) {
    const f32 b = chrome.resize_border;
    if (resizable && b > 0.0f) {
        const bool left = point.x < b;
        const bool right = point.x >= size.x - b;
        const bool top = point.y < b;
        const bool bottom = point.y >= size.y - b;
        if (top) {
            return left ? WindowArea::ResizeTopLeft : right ? WindowArea::ResizeTopRight : WindowArea::ResizeTop;
        }
        if (bottom) {
            return left ? WindowArea::ResizeBottomLeft : right ? WindowArea::ResizeBottomRight : WindowArea::ResizeBottom;
        }
        if (left) {
            return WindowArea::ResizeLeft;
        }
        if (right) {
            return WindowArea::ResizeRight;
        }
    }
    const auto in = [&](const std::vector<Rectf>& rects) {
        return std::ranges::any_of(rects, [&](Rectf r) { return inside(r, point); });
    };
    return in(chrome.drag) && !in(chrome.exclude) ? WindowArea::Drag : WindowArea::Client;
}

Vec2i primary_display_size() {
    const bool had_video = SDL_WasInit(SDL_INIT_VIDEO) != 0;
    if (!had_video && !SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        return {0, 0};
    }
    Vec2i out{0, 0};
    const SDL_DisplayID display = SDL_GetPrimaryDisplay();
    SDL_Rect bounds{};
    if (display != 0 && SDL_GetDisplayUsableBounds(display, &bounds)) {
        float scale = window_units_per_point(static_cast<SDL_Window*>(nullptr));
#if defined(SDL_PLATFORM_ANDROID)
        if (window_point_scale_override_unset()) {
            const float content = SDL_GetDisplayContentScale(display);
            scale = std::isfinite(content) && content > 0.0f ? content : 1.0f;
        }
#endif
        out = {to_points(bounds.w, scale), to_points(bounds.h, scale)}; // in points, as a window's size
    }
    // Leave the subsystem exactly as it was found.
    if (!had_video) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }
    return out;
}

Window::Window(const WindowConfig& config) {
    SDL_WindowFlags flags = 0;
#if KIN_HAS_WEBGL2_RENDERER
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    flags |= SDL_WINDOW_OPENGL;
#endif
    if (config.resizable) {
        flags |= SDL_WINDOW_RESIZABLE;
    }
    if (config.maximized) {
        flags |= SDL_WINDOW_MAXIMIZED;
    }
    if (config.fullscreen) {
        flags |= SDL_WINDOW_FULLSCREEN;
    }
    if (config.hidden) {
        flags |= SDL_WINDOW_HIDDEN;
    }
    if (config.borderless) {
        flags |= SDL_WINDOW_BORDERLESS;
    }
    if (config.high_pixel_density) {
        flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
    }
#if defined(SDL_PLATFORM_ANDROID)
    // A phone's window is the whole screen, at its own pixels, with the system
    // bars hidden. The game's shape picks the orientation (a wider window turns
    // the phone to landscape) unless the game set SDL_HINT_ORIENTATIONS.
    flags |= SDL_WINDOW_FULLSCREEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (const char* orientations = SDL_GetHint(SDL_HINT_ORIENTATIONS); !orientations || !*orientations) {
        if (config.width > config.height) {
            SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
        } else if (config.height > config.width) {
            SDL_SetHint(SDL_HINT_ORIENTATIONS, "Portrait");
        }
    }
#endif

    const std::string title{config.title};
    // The size asked for is in points (window_points.hpp).
    const float scale = window_units_per_point(static_cast<SDL_Window*>(nullptr));
    _handle = SDL_CreateWindow(title.c_str(), to_units(config.width, scale), to_units(config.height, scale), flags);
    if (!_handle) {
        throw std::runtime_error(std::string("SDL_CreateWindow failed: ") + SDL_GetError());
    }
}

Window::~Window() {
    if (_handle && _chrome && _chrome->installed) {
        SDL_SetWindowHitTest(to_sdl_window(_handle), nullptr, nullptr);
    }
    for (void* cursor : _cursors) {
        if (cursor) {
            SDL_DestroyCursor(static_cast<SDL_Cursor*>(cursor)); // the default comes back if it was showing
        }
    }
    if (_handle) {
        SDL_DestroyWindow(to_sdl_window(_handle));
    }
}

WindowId Window::id() const {
    return _handle ? static_cast<WindowId>(SDL_GetWindowID(to_sdl_window(_handle))) : 0;
}

i32 Window::width() const {
    return size().x;
}

i32 Window::height() const {
    return size().y;
}

Vec2i Window::size() const {
    if (!_handle) {
        return {};
    }

    Vec2i units{};
    SDL_Window* window = to_sdl_window(_handle);
    SDL_GetWindowSize(window, &units.x, &units.y);
    const float scale = window_units_per_point(window);
    return {to_points(units.x, scale), to_points(units.y, scale)};
}

Vec2i Window::pixel_size() const {
    Vec2i result{};
    if (!_handle || !SDL_GetWindowSizeInPixels(to_sdl_window(_handle), &result.x, &result.y))
        return size();
    return result;
}

Rectf Window::safe_area() const {
    const Vec2i whole = size();
    SDL_Rect rect{};
    if (!_handle || !SDL_GetWindowSafeArea(to_sdl_window(_handle), &rect) || rect.w <= 0 || rect.h <= 0) {
        return {0.0f, 0.0f, static_cast<f32>(whole.x), static_cast<f32>(whole.y)};
    }
    const f32 scale = window_units_per_point(to_sdl_window(_handle));
    return {static_cast<f32>(rect.x) / scale, static_cast<f32>(rect.y) / scale, static_cast<f32>(rect.w) / scale,
            static_cast<f32>(rect.h) / scale};
}

f32 Window::display_scale() const {
    if (const f32 forced = detail::window_point_scale_override(); forced > 0.0f && _handle) {
        // A density set for the window's points (window_points.hpp): its pixels a point.
        const f32 density = SDL_GetWindowPixelDensity(to_sdl_window(_handle));
        return forced * (std::isfinite(density) && density > 0.0f ? density : 1.0f);
    }
    const f32 scale = _handle ? SDL_GetWindowDisplayScale(to_sdl_window(_handle)) : 1.0f;
    return std::isfinite(scale) && scale > 0.0f ? scale : 1.0f;
}

void Window::set_minimum_size(Vec2i size) {
    if (_handle) {
        SDL_Window* window = to_sdl_window(_handle);
        const float scale = window_units_per_point(window);
        SDL_SetWindowMinimumSize(window, to_units(size.x, scale), to_units(size.y, scale));
    }
}

void Window::set_size(Vec2i size) {
    if (_handle) {
        SDL_Window* window = to_sdl_window(_handle);
        const float scale = window_units_per_point(window);
        SDL_SetWindowSize(window, to_units(size.x, scale), to_units(size.y, scale));
    }
}

void Window::set_size(i32 width, i32 height) {
    set_size(Vec2i{width, height});
}

bool Window::sync() {
    return _handle && SDL_SyncWindow(to_sdl_window(_handle));
}

void Window::minimize() {
    if (_handle) {
        SDL_MinimizeWindow(to_sdl_window(_handle));
    }
}

void Window::toggle_maximized() {
    if (_handle) {
        maximized() ? SDL_RestoreWindow(to_sdl_window(_handle)) : SDL_MaximizeWindow(to_sdl_window(_handle));
    }
}

bool Window::maximized() const {
    return _handle && ((SDL_GetWindowFlags(to_sdl_window(_handle)) & SDL_WINDOW_MAXIMIZED) != 0);
}

void Window::set_chrome(WindowChrome chrome) {
    if (!_handle) {
        return;
    }
    if (!_chrome) {
        _chrome = std::make_unique<Chrome>();
    }
    {
        const std::scoped_lock lock(_chrome->mutex);
        _chrome->chrome = std::move(chrome);
    }
    if (!_chrome->installed) {
        _chrome->installed = SDL_SetWindowHitTest(to_sdl_window(_handle), chrome_hit_test, _chrome.get());
    }
}

bool Window::borderless() const {
    return _handle && ((SDL_GetWindowFlags(to_sdl_window(_handle)) & SDL_WINDOW_BORDERLESS) != 0);
}

bool Window::fullscreen() const {
    return _handle && ((SDL_GetWindowFlags(to_sdl_window(_handle)) & SDL_WINDOW_FULLSCREEN) != 0);
}

void Window::set_icon(const Image& image) {
    if (!_handle || !image.valid()) {
        return;
    }
    SDL_Surface* surface = SDL_CreateSurfaceFrom(image.size.x, image.size.y, SDL_PIXELFORMAT_RGBA32,
                                                 const_cast<u8*>(image.rgba.data()), image.size.x * 4);
    if (surface) {
        SDL_SetWindowIcon(to_sdl_window(_handle), surface);
        SDL_DestroySurface(surface);
    }
}

void Window::set_fullscreen(bool enabled) {
    if (_handle) {
        SDL_SetWindowFullscreen(to_sdl_window(_handle), enabled);
    }
}

void Window::show() {
    if (_handle) {
        SDL_ShowWindow(to_sdl_window(_handle));
    }
}

void Window::hide() {
    if (_handle) {
        SDL_HideWindow(to_sdl_window(_handle));
    }
}

void Window::set_relative_mouse(bool enabled) {
    if (_handle) {
        SDL_SetWindowRelativeMouseMode(to_sdl_window(_handle), enabled);
    }
}

void Window::set_cursor(SystemCursor cursor) {
    if (!_handle || SDL_GetMouseFocus() != to_sdl_window(_handle)) {
        return;
    }
    SDL_Cursor* shape = SDL_GetDefaultCursor();
    if (cursor != SystemCursor::Default) {
        static constexpr SDL_SystemCursor sdl[] = {
            SDL_SYSTEM_CURSOR_DEFAULT,     SDL_SYSTEM_CURSOR_TEXT,      SDL_SYSTEM_CURSOR_POINTER,
            SDL_SYSTEM_CURSOR_MOVE,        SDL_SYSTEM_CURSOR_EW_RESIZE, SDL_SYSTEM_CURSOR_NS_RESIZE,
            SDL_SYSTEM_CURSOR_NWSE_RESIZE, SDL_SYSTEM_CURSOR_NESW_RESIZE, SDL_SYSTEM_CURSOR_NOT_ALLOWED,
            SDL_SYSTEM_CURSOR_WAIT,        SDL_SYSTEM_CURSOR_CROSSHAIR,
        };
        static_assert(std::size(sdl) == std::tuple_size_v<decltype(_cursors)>);
        void*& made = _cursors[static_cast<std::size_t>(cursor)];
        if (!made) {
            made = SDL_CreateSystemCursor(sdl[static_cast<std::size_t>(cursor)]);
        }
        if (made) {
            shape = static_cast<SDL_Cursor*>(made);
        }
    }
    if (shape && SDL_GetCursor() != shape) {
        SDL_SetCursor(shape);
    }
}

bool Window::relative_mouse() const {
    return _handle && SDL_GetWindowRelativeMouseMode(to_sdl_window(_handle));
}

void Window::hint_frame_rate(f32 fps) {
#if defined(SDL_PLATFORM_ANDROID)
    if (!_handle) {
        return;
    }
    fps = std::isfinite(fps) && fps > 0.0f ? fps : 0.0f;
    void* surface = SDL_GetPointerProperty(SDL_GetWindowProperties(to_sdl_window(_handle)), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr);
    if (!surface || (surface == _rate_surface && fps == _rate)) {
        return;
    }
    // Android 11 (API 30) has it; kin runs from Android 10, so it is looked up.
    using SetFrameRate = int32_t (*)(ANativeWindow*, float, int8_t);
    static const auto set_frame_rate = [] {
        for (const char* library : {"libandroid.so", "libnativewindow.so"}) {
            if (void* handle = dlopen(library, RTLD_NOW | RTLD_NOLOAD); handle || (handle = dlopen(library, RTLD_NOW))) {
                if (void* found = dlsym(handle, "ANativeWindow_setFrameRate")) {
                    return reinterpret_cast<SetFrameRate>(found);
                }
            }
        }
        return SetFrameRate{};
    }();
    const int32_t result = set_frame_rate ? set_frame_rate(static_cast<ANativeWindow*>(surface), fps, 0 /* ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT */) : -1;
    KIN_LOG_INFO_F("runtime",
                   "display frame rate asked",
                   (LogFields{{.name = "fps", .value = std::to_string(fps)},
                              {.name = "result", .value = set_frame_rate ? std::to_string(result) : std::string("unsupported")}}));
    _rate_surface = surface;
    _rate = fps;
#else
    (void)fps;
#endif
}

void Window::set_text_input_enabled(bool enabled) {
    if (!_handle) {
        return;
    }

    if (enabled) {
        SDL_StartTextInput(to_sdl_window(_handle));
    } else {
        SDL_StopTextInput(to_sdl_window(_handle));
    }
}

void Window::set_text_input_area(Rectf area, i32 cursor) {
    _text_input_area = area;
    if (!_handle) {
        return;
    }
    const f32 scale = window_units_per_point(to_sdl_window(_handle));
    const SDL_Rect rect{static_cast<int>(area.x * scale), static_cast<int>(area.y * scale), std::max(1, static_cast<int>(area.w * scale)),
                        std::max(1, static_cast<int>(area.h * scale))};
    SDL_SetTextInputArea(to_sdl_window(_handle), &rect, static_cast<int>(static_cast<f32>(cursor) * scale));
}

bool Window::text_input_enabled() const {
    return _handle && SDL_TextInputActive(to_sdl_window(_handle));
}

} // namespace kin
