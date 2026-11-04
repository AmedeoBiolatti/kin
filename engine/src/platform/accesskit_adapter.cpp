#include "accesskit_adapter.hpp"

#include <kin/platform/window.hpp>

#include <SDL3/SDL.h>

#include <string_view>

namespace kin {
namespace {

#if !defined(_WIN32) && !defined(ACCESSKIT_MACOS)
void deactivated(void*) {}
#endif

} // namespace

AccessKitAdapter::AccessKitAdapter(Window& window, TreeSource tree, accesskit_action_handler_callback action,
                                   void* data)
    : _window(window), _tree(tree), _data(data) {
    auto* native = static_cast<SDL_Window*>(window.native_handle());
#if defined(_WIN32)
    auto* hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(native), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    if (hwnd) {
        _adapter = accesskit_windows_subclassing_adapter_new(static_cast<HWND>(hwnd), tree, data, action, data);
    }
#elif defined(ACCESSKIT_MACOS)
    accesskit_macos_add_focus_forwarder_to_window_class("SDL3Window");
    auto* nswindow =
        SDL_GetPointerProperty(SDL_GetWindowProperties(native), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
    if (nswindow) {
        _adapter = accesskit_macos_subclassing_adapter_for_window(nswindow, tree, data, action, data);
    }
#else
    (void)native;
    _adapter = accesskit_unix_adapter_new(tree, data, action, data, deactivated, data);
#endif
}

AccessKitAdapter::~AccessKitAdapter() {
    if (!_adapter) {
        return;
    }
#if defined(_WIN32)
    accesskit_windows_subclassing_adapter_free(_adapter);
#elif defined(ACCESSKIT_MACOS)
    accesskit_macos_subclassing_adapter_free(_adapter);
#else
    accesskit_unix_adapter_free(_adapter);
#endif
}

void AccessKitAdapter::publish() {
    if (!_adapter) {
        return;
    }
    auto* native = static_cast<SDL_Window*>(_window.native_handle());
    const bool focused = (SDL_GetWindowFlags(native) & SDL_WINDOW_INPUT_FOCUS) != 0;
#if defined(_WIN32)
    (void)focused;
    if (auto* events = accesskit_windows_subclassing_adapter_update_if_active(_adapter, _tree, _data)) {
        accesskit_windows_queued_events_raise(events);
    }
#elif defined(ACCESSKIT_MACOS)
    if (auto* events = accesskit_macos_subclassing_adapter_update_view_focus_state(_adapter, focused)) {
        accesskit_macos_queued_events_raise(events);
    }
    if (auto* events = accesskit_macos_subclassing_adapter_update_if_active(_adapter, _tree, _data)) {
        accesskit_macos_queued_events_raise(events);
    }
#else
    // Only X11 lets an application know where its window is.
    int x = 0, y = 0, w = 0, h = 0, top = 0, left = 0, bottom = 0, right = 0;
    if (std::string_view{SDL_GetCurrentVideoDriver()} == "x11" && SDL_GetWindowPosition(native, &x, &y) &&
        SDL_GetWindowSize(native, &w, &h)) {
        SDL_GetWindowBordersSize(native, &top, &left, &bottom, &right);
        accesskit_unix_adapter_set_root_window_bounds(
            _adapter, {double(x - left), double(y - top), double(x + w + right), double(y + h + bottom)},
            {double(x), double(y), double(x + w), double(y + h)});
    }
    accesskit_unix_adapter_update_window_focus_state(_adapter, focused);
    accesskit_unix_adapter_update_if_active(_adapter, _tree, _data);
#endif
}

std::string AccessKitAdapter::window_title(const Window& window) {
    const char* title = SDL_GetWindowTitle(static_cast<SDL_Window*>(window.native_handle()));
    return title ? title : "";
}

} // namespace kin
