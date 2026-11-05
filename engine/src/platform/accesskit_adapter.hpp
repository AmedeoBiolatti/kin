#pragma once

// AccessKit's adapter for one window: the platform half of ui2::Accessibility,
// which builds the tree. Built only with KIN_ENABLE_ACCESSIBILITY
// (cmake/kin_accessibility.cmake).

#include <accesskit.h>

#include <string>

namespace kin {

class Window;

class AccessKitAdapter {
public:
    // Builds the whole tree, when assistive technology starts listening and
    // after each change (any thread).
    using TreeSource = accesskit_tree_update* (*)(void* data);

    // `action` takes assistive technology's requests (any thread). Both get `data`.
    AccessKitAdapter(Window& window, TreeSource tree, accesskit_action_handler_callback action, void* data);
    ~AccessKitAdapter();
    AccessKitAdapter(const AccessKitAdapter&) = delete;
    AccessKitAdapter& operator=(const AccessKitAdapter&) = delete;

    // Whether the platform's adapter started.
    bool active() const { return _adapter != nullptr; }
    // After the UI changed: the window's focus (and place), then a new tree if
    // assistive technology is listening.
    void publish();
    // The window's title, the tree's root name when the game gives none.
    static std::string window_title(const Window& window);

private:
    Window& _window;
    TreeSource _tree;
    void* _data;
#if defined(_WIN32)
    accesskit_windows_subclassing_adapter* _adapter = nullptr;
#elif defined(ACCESSKIT_MACOS)
    accesskit_macos_subclassing_adapter* _adapter = nullptr;
#else
    accesskit_unix_adapter* _adapter = nullptr;
#endif
};

} // namespace kin
