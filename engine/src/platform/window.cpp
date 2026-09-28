#include <kin/platform/window.hpp>

#include <SDL3/SDL.h>

#include <stdexcept>
#include <string>
#include <cmath>

namespace kin {
namespace {

SDL_Window* to_sdl_window(void* handle) {
    return static_cast<SDL_Window*>(handle);
}

const SDL_Window* to_sdl_window(const void* handle) {
    return static_cast<const SDL_Window*>(handle);
}

} // namespace

Vec2i primary_display_size() {
    const bool had_video = SDL_WasInit(SDL_INIT_VIDEO) != 0;
    if (!had_video && !SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        return {0, 0};
    }
    Vec2i out{0, 0};
    const SDL_DisplayID display = SDL_GetPrimaryDisplay();
    SDL_Rect bounds{};
    if (display != 0 && SDL_GetDisplayUsableBounds(display, &bounds)) {
        out = {bounds.w, bounds.h};
    }
    // Leave the subsystem exactly as it was found.
    if (!had_video) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }
    return out;
}

Window::Window(const WindowConfig& config) {
    SDL_WindowFlags flags = 0;
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

    const std::string title{config.title};
    _handle = SDL_CreateWindow(title.c_str(), config.width, config.height, flags);
    if (!_handle) {
        throw std::runtime_error(std::string("SDL_CreateWindow failed: ") + SDL_GetError());
    }
}

Window::~Window() {
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

    Vec2i result{};
    SDL_GetWindowSize(to_sdl_window(_handle), &result.x, &result.y);
    return result;
}

Vec2i Window::pixel_size() const {
    Vec2i result{};
    if (!_handle || !SDL_GetWindowSizeInPixels(to_sdl_window(_handle), &result.x, &result.y))
        return size();
    return result;
}

f32 Window::display_scale() const {
    const f32 scale = _handle ? SDL_GetWindowDisplayScale(to_sdl_window(_handle)) : 1.0f;
    return std::isfinite(scale) && scale > 0.0f ? scale : 1.0f;
}

void Window::set_minimum_size(Vec2i size) {
    if (_handle) SDL_SetWindowMinimumSize(to_sdl_window(_handle), size.x, size.y);
}

void Window::set_size(Vec2i size) {
    if (_handle) {
        SDL_SetWindowSize(to_sdl_window(_handle), size.x, size.y);
    }
}

void Window::set_size(i32 width, i32 height) {
    set_size(Vec2i{width, height});
}

bool Window::borderless() const {
    return _handle && ((SDL_GetWindowFlags(to_sdl_window(_handle)) & SDL_WINDOW_BORDERLESS) != 0);
}

bool Window::fullscreen() const {
    return _handle && ((SDL_GetWindowFlags(to_sdl_window(_handle)) & SDL_WINDOW_FULLSCREEN) != 0);
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

bool Window::text_input_enabled() const {
    return _handle && SDL_TextInputActive(to_sdl_window(_handle));
}

} // namespace kin
