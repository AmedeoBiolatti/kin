#include <SDL3/SDL.h>
#include <algorithm>
#include <deque>
#include <kin/ui2/accessibility.hpp>
#include <kin/ui2/context.hpp>
#include <map>
#include <mutex>
#include <set>
#if defined(KIN_HAS_ACCESSKIT)
#include <accesskit.h>
#endif

namespace kin::ui2 {
struct Accessibility::Impl {
    Window& window;
    Renderer2D& renderer;
    std::string title;
    mutable std::mutex mutex;
    std::vector<SemanticElement> elements;
    std::map<u64, u64> ids; // semantic -> native, never recycle native IDs
    u64 next_id = 4, announcement_id = 2;
    std::string announcement;
    bool assertive = false, available = false;
    Rectf bounds{};
    struct Action {
        u64 native;
        SemanticAction action;
        std::string text;
    };
    std::deque<Action> actions;

    bool enqueue(u64 native, SemanticAction action, std::string text) {
        if (actions.size() >= 128)
            return false;
        for (const auto& e : elements) {
            if (ids.at(e.id.value) != native || !e.enabled || !e.visible || e.blocked || !e.focusable)
                continue;
            if (action == SemanticAction::SetText && e.role != "text_input" && e.role != "text_edit")
                return false;
            if ((action == SemanticAction::Increment || action == SemanticAction::Decrement ||
                 action == SemanticAction::SetNumber) &&
                e.role != "slider" && e.role != "spinbutton")
                return false;
            actions.push_back({native, action, std::move(text)});
            return true;
        }
        return false;
    }
#if defined(KIN_HAS_ACCESSKIT)
#if defined(_WIN32)
    accesskit_windows_subclassing_adapter* adapter = nullptr;
#elif defined(ACCESSKIT_MACOS)
    accesskit_macos_subclassing_adapter* adapter = nullptr;
#else
    accesskit_unix_adapter* adapter = nullptr;
#endif
    static accesskit_role role(std::string_view name) {
        if (name == "button")
            return ACCESSKIT_ROLE_BUTTON;
        if (name == "checkbox")
            return ACCESSKIT_ROLE_CHECK_BOX;
        if (name == "toggle" || name == "switch")
            return ACCESSKIT_ROLE_SWITCH;
        if (name == "text_input")
            return ACCESSKIT_ROLE_TEXT_INPUT;
        if (name == "text_edit")
            return ACCESSKIT_ROLE_MULTILINE_TEXT_INPUT;
        if (name == "label" || name == "text")
            return ACCESSKIT_ROLE_LABEL;
        if (name == "slider")
            return ACCESSKIT_ROLE_SLIDER;
        if (name == "spinbutton")
            return ACCESSKIT_ROLE_SPIN_BUTTON;
        if (name == "text_view")
            return ACCESSKIT_ROLE_MULTILINE_TEXT_INPUT;
        if (name == "menu_item")
            return ACCESSKIT_ROLE_MENU_ITEM;
        if (name == "menu")
            return ACCESSKIT_ROLE_MENU;
        return ACCESSKIT_ROLE_GROUP;
    }
    static accesskit_rect rect(Rectf r) { return {r.x, r.y, r.x + r.w, r.y + r.h}; }
    accesskit_tree_update* tree() {
        std::lock_guard lock{mutex};
        u64 focus = 1;
        std::map<u64, u64> parents;
        std::map<u64, Id> semantic_parents;
        for (const auto& e : elements)
            semantic_parents.emplace(e.id.value, e.parent);
        for (const auto& e : elements) {
            const auto id = ids.at(e.id.value);
            auto parent = e.parent;
            std::set<u64> visited{e.id.value};
            bool cycle = false;
            while (parent && ids.contains(parent.value)) {
                if (visited.size() >= 256 || !visited.insert(parent.value).second) {
                    cycle = true;
                    break;
                }
                parent = semantic_parents.at(parent.value);
            }
            parents[id] = !cycle && ids.contains(e.parent.value) ? ids.at(e.parent.value) : 1;
            if (e.focused && e.enabled && e.visible && !e.blocked)
                focus = id;
        }
        auto* update = accesskit_tree_update_with_capacity_and_focus(elements.size() + 2, focus);
        std::map<u64, std::vector<u64>> children;
        for (const auto& [id, parent] : parents)
            children[parent].push_back(id);
        accesskit_tree_update_set_tree_info(update, accesskit_tree_info_new(1));
        auto* root = accesskit_node_new(ACCESSKIT_ROLE_WINDOW);
        accesskit_node_set_label_with_length(root, title.data(), title.size());
        accesskit_node_set_bounds(root, rect(bounds));
        for (const auto id : children[1])
            accesskit_node_push_child(root, id);
        if (!announcement.empty())
            accesskit_node_push_child(root, announcement_id);
        accesskit_tree_update_push_node(update, 1, root);
        for (const auto& e : elements) {
            const auto id = ids.at(e.id.value);
            auto* node = accesskit_node_new(role(e.role));
            accesskit_node_set_label_with_length(node, e.label.data(), e.label.size());
            accesskit_node_set_value_with_length(node, e.value.data(), e.value.size());
            accesskit_node_set_bounds(node, rect(e.bounds));
            if (e.numeric_value)
                accesskit_node_set_numeric_value(node, *e.numeric_value);
            if (e.numeric_min)
                accesskit_node_set_min_numeric_value(node, *e.numeric_min);
            if (e.numeric_max)
                accesskit_node_set_max_numeric_value(node, *e.numeric_max);
            if (e.numeric_step)
                accesskit_node_set_numeric_value_step(node, *e.numeric_step);
            if (e.role == "text_view")
                accesskit_node_set_read_only(node);
            if (!e.visible || e.blocked)
                accesskit_node_set_hidden(node);
            if (!e.enabled || e.blocked)
                accesskit_node_set_disabled(node);
            if (e.role == "checkbox" || e.role == "toggle" || e.role == "switch")
                accesskit_node_set_toggled(node, e.checked ? ACCESSKIT_TOGGLED_TRUE : ACCESSKIT_TOGGLED_FALSE);
            if (e.focusable && e.enabled && e.visible && !e.blocked) {
                accesskit_node_add_action(node, ACCESSKIT_ACTION_FOCUS);
                if (e.role == "slider" || e.role == "spinbutton") {
                    accesskit_node_add_action(node, ACCESSKIT_ACTION_INCREMENT);
                    accesskit_node_add_action(node, ACCESSKIT_ACTION_DECREMENT);
                    accesskit_node_add_action(node, ACCESSKIT_ACTION_SET_VALUE);
                } else if (e.role == "text_input" || e.role == "text_edit")
                    accesskit_node_add_action(node, ACCESSKIT_ACTION_SET_VALUE);
                else
                    accesskit_node_add_action(node, ACCESSKIT_ACTION_CLICK);
            }
            for (const auto child : children[id])
                accesskit_node_push_child(node, child);
            accesskit_tree_update_push_node(update, id, node);
        }
        if (!announcement.empty()) {
            auto* node = accesskit_node_new(ACCESSKIT_ROLE_LABEL);
            accesskit_node_set_value_with_length(node, announcement.data(), announcement.size());
            accesskit_node_set_live(node, assertive ? ACCESSKIT_LIVE_ASSERTIVE : ACCESSKIT_LIVE_POLITE);
            accesskit_tree_update_push_node(update, announcement_id, node);
        }
        return update;
    }
    static accesskit_tree_update* make_tree(void* data) {
        try {
            return static_cast<Impl*>(data)->tree();
        } catch (...) {
            return nullptr;
        }
    }
    static void deactivate(void*) {}
    static void action(accesskit_action_request* request, void* data) {
        try {
            auto& self = *static_cast<Impl*>(data);
            std::lock_guard lock{self.mutex};
            if (request->action == ACCESSKIT_ACTION_FOCUS)
                self.enqueue(request->target_node, SemanticAction::Focus, {});
            else if (request->action == ACCESSKIT_ACTION_CLICK)
                self.enqueue(request->target_node, SemanticAction::Click, {});
            else if (request->action == ACCESSKIT_ACTION_INCREMENT)
                self.enqueue(request->target_node, SemanticAction::Increment, {});
            else if (request->action == ACCESSKIT_ACTION_DECREMENT)
                self.enqueue(request->target_node, SemanticAction::Decrement, {});
            else if (request->action == ACCESSKIT_ACTION_SET_VALUE && request->data.has_value &&
                     request->data.value.tag == ACCESSKIT_ACTION_DATA_NUMERIC_VALUE)
                self.enqueue(request->target_node, SemanticAction::SetNumber,
                             std::to_string(request->data.value.numeric_value));
            else if (request->action == ACCESSKIT_ACTION_SET_VALUE && request->data.has_value &&
                     request->data.value.tag == ACCESSKIT_ACTION_DATA_VALUE && request->data.value.value)
                self.enqueue(request->target_node, SemanticAction::SetText, request->data.value.value);
        } catch (...) {
        } // callbacks must not unwind through the C ABI
        accesskit_action_request_free(request);
    }
#endif
    Impl(Window& w, Renderer2D& r, std::string t) : window(w), renderer(r), title(std::move(t)) {
        if (title.empty())
            title = SDL_GetWindowTitle(static_cast<SDL_Window*>(w.native_handle()));
        bounds = {0, 0, static_cast<f32>(w.width()), static_cast<f32>(w.height())};
#if defined(KIN_HAS_ACCESSKIT)
        auto* native = static_cast<SDL_Window*>(w.native_handle());
#if defined(_WIN32)
        auto* hwnd =
            SDL_GetPointerProperty(SDL_GetWindowProperties(native), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
        if (hwnd)
            adapter = accesskit_windows_subclassing_adapter_new(static_cast<HWND>(hwnd), make_tree, this, action, this);
#elif defined(ACCESSKIT_MACOS)
        accesskit_macos_add_focus_forwarder_to_window_class("SDL3Window");
        auto* nswindow =
            SDL_GetPointerProperty(SDL_GetWindowProperties(native), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
        if (nswindow)
            adapter = accesskit_macos_subclassing_adapter_for_window(nswindow, make_tree, this, action, this);
#else
        adapter = accesskit_unix_adapter_new(make_tree, this, action, this, deactivate, this);
#endif
        available = adapter != nullptr;
#endif
    }
    ~Impl() {
#if defined(KIN_HAS_ACCESSKIT)
#if defined(_WIN32)
        if (adapter)
            accesskit_windows_subclassing_adapter_free(adapter);
#elif defined(ACCESSKIT_MACOS)
        if (adapter)
            accesskit_macos_subclassing_adapter_free(adapter);
#else
        if (adapter)
            accesskit_unix_adapter_free(adapter);
#endif
#endif
    }
    void publish() {
#if defined(KIN_HAS_ACCESSKIT)
        if (!adapter)
            return;
        auto* native = static_cast<SDL_Window*>(window.native_handle());
        const bool focused = (SDL_GetWindowFlags(native) & SDL_WINDOW_INPUT_FOCUS) != 0;
#if defined(_WIN32)
        if (auto* events = accesskit_windows_subclassing_adapter_update_if_active(adapter, make_tree, this))
            accesskit_windows_queued_events_raise(events);
#elif defined(ACCESSKIT_MACOS)
        if (auto* events = accesskit_macos_subclassing_adapter_update_view_focus_state(adapter, focused))
            accesskit_macos_queued_events_raise(events);
        if (auto* events = accesskit_macos_subclassing_adapter_update_if_active(adapter, make_tree, this))
            accesskit_macos_queued_events_raise(events);
#else
        int x = 0, y = 0, w = 0, h = 0, top = 0, left = 0, bottom = 0, right = 0;
        if (std::string_view{SDL_GetCurrentVideoDriver()} == "x11" && SDL_GetWindowPosition(native, &x, &y) &&
            SDL_GetWindowSize(native, &w, &h)) {
            SDL_GetWindowBordersSize(native, &top, &left, &bottom, &right);
            accesskit_unix_adapter_set_root_window_bounds(
                adapter, {double(x - left), double(y - top), double(x + w + right), double(y + h + bottom)},
                {double(x), double(y), double(x + w), double(y + h)});
        }
        accesskit_unix_adapter_update_window_focus_state(adapter, focused);
        accesskit_unix_adapter_update_if_active(adapter, make_tree, this);
#endif
#endif
    }
};

Accessibility::Accessibility(Window& window, Renderer2D& renderer, std::string title)
    : _impl(std::make_unique<Impl>(window, renderer, std::move(title))) {}
Accessibility::~Accessibility() = default;
void Accessibility::update(Context& context) {
    auto& s = *_impl;
    {
        std::lock_guard lock{s.mutex};
        // One action per UI frame. Keep a busy Context's next action queued.
        while (!s.actions.empty()) {
            auto& action = s.actions.front();
            const auto id = std::ranges::find_if(s.ids, [&](const auto& pair) { return pair.second == action.native; });
            if (id == s.ids.end()) {
                s.actions.pop_front();
                continue;
            }
            std::string error;
            const auto selector = std::to_string(id->first);
            if (!context.resolve_action(selector, action.action, error)) {
                s.actions.pop_front();
                continue;
            }
            if (context.queue_action(selector, action.action, action.text, error))
                s.actions.pop_front();
            break;
        }
        s.elements = context.elements();
        std::set<u64> live;
        for (auto& e : s.elements) {
            live.insert(e.id.value);
            if (!s.ids.contains(e.id.value))
                s.ids.emplace(e.id.value, s.next_id++);
            e.focused = context.state().is_focused(e.id);
            const auto a = s.renderer.logical_to_window({e.bounds.x, e.bounds.y});
            const auto b = s.renderer.logical_to_window({e.bounds.x + e.bounds.w, e.bounds.y + e.bounds.h});
            e.bounds = {a.x, a.y, b.x - a.x, b.y - a.y};
        }
        std::erase_if(s.ids, [&](const auto& pair) { return !live.contains(pair.first); });
        s.bounds = {0, 0, static_cast<f32>(s.window.width()), static_cast<f32>(s.window.height())};
    }
    s.publish();
}
void Accessibility::announce(std::string text, bool assertive) {
    std::lock_guard lock{_impl->mutex};
    _impl->announcement = std::move(text);
    _impl->assertive = assertive;
    _impl->announcement_id = _impl->next_id++; // repeated text still announces
}
bool Accessibility::request_action(Id id, SemanticAction action, std::string text) {
    std::lock_guard lock{_impl->mutex};
    const auto found = _impl->ids.find(id.value);
    return found != _impl->ids.end() && _impl->enqueue(found->second, action, std::move(text));
}
std::vector<SemanticElement> Accessibility::snapshot() const {
    std::lock_guard lock{_impl->mutex};
    return _impl->elements;
}
bool Accessibility::native_available() const {
    return _impl->available;
}
} // namespace kin::ui2
