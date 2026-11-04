#include <kin/ui2/accessibility.hpp>

#include <kin/platform/window.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/ui2/context.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>

#if defined(KIN_HAS_ACCESSKIT)
#include "../platform/accesskit_adapter.hpp"
#endif

namespace kin::ui2 {

struct Accessibility::Impl {
    Window& window;
    Renderer2D& renderer;
    std::string title;
    mutable std::mutex mutex;
    std::vector<SemanticElement> elements; // the last update's, in window pixels
    // Each element's native id, by semantic id: native ids are never reused, so
    // a request for a removed element never reaches one built later.
    struct NativeId {
        u64 native = 0;
        u64 seen = 0; // the update that last saw it
    };
    std::unordered_map<u64, NativeId> ids;
    std::unordered_map<u64, u64> semantic_of; // native -> semantic
    u64 next_id = 4, announcement_id = 2, updates = 0;
    std::string announcement;
    bool assertive = false;
    Rectf bounds{};
    struct Action {
        u64 native;
        SemanticAction action;
        std::string text;
        f64 number;
    };
    std::deque<Action> actions;

    // Under `mutex`: a request for what the last update showed, rechecked when dispatched.
    bool enqueue(u64 native, SemanticAction action, std::string text, f64 number) {
        if (actions.size() >= 128) {
            return false;
        }
        const auto semantic = semantic_of.find(native);
        if (semantic == semantic_of.end()) {
            return false;
        }
        const auto e = std::ranges::find(elements, semantic->second,
                                         [](const SemanticElement& element) { return element.id.value; });
        if (e == elements.end() || !e->available() || !e->focusable || !semantic_role_accepts(e->role, action)) {
            return false;
        }
        actions.push_back({native, action, std::move(text), number});
        return true;
    }

#if defined(KIN_HAS_ACCESSKIT)
    static accesskit_role role(std::string_view name) {
        if (name == "button") {
            return ACCESSKIT_ROLE_BUTTON;
        }
        if (name == "toggle") {
            return ACCESSKIT_ROLE_SWITCH;
        }
        if (name == "text_input") {
            return ACCESSKIT_ROLE_TEXT_INPUT;
        }
        if (name == "text_edit" || name == "text_view") {
            return ACCESSKIT_ROLE_MULTILINE_TEXT_INPUT;
        }
        if (name == "text") {
            return ACCESSKIT_ROLE_LABEL;
        }
        if (name == "slider") {
            return ACCESSKIT_ROLE_SLIDER;
        }
        if (name == "spinbutton") {
            return ACCESSKIT_ROLE_SPIN_BUTTON;
        }
        if (name == "menu_item") {
            return ACCESSKIT_ROLE_MENU_ITEM;
        }
        if (name == "menu") {
            return ACCESSKIT_ROLE_MENU;
        }
        return ACCESSKIT_ROLE_GROUP;
    }

    static accesskit_rect rect(Rectf r) { return {r.x, r.y, r.x + r.w, r.y + r.h}; }

    accesskit_tree_update* tree() {
        std::lock_guard lock{mutex};
        const auto native = [&](Id id) { return ids.at(id.value).native; };
        u64 focus = 1;
        std::map<u64, u64> parents;
        std::map<u64, Id> semantic_parents;
        for (const auto& e : elements) {
            semantic_parents.emplace(e.id.value, e.parent);
        }
        for (const auto& e : elements) {
            const u64 id = native(e.id);
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
            parents[id] = !cycle && ids.contains(e.parent.value) ? native(e.parent) : 1;
            if (e.focused && e.available()) {
                focus = id;
            }
        }
        auto* update = accesskit_tree_update_with_capacity_and_focus(elements.size() + 2, focus);
        std::map<u64, std::vector<u64>> children;
        for (const auto& [id, parent] : parents) {
            children[parent].push_back(id);
        }
        accesskit_tree_update_set_tree_info(update, accesskit_tree_info_new(1));
        auto* root = accesskit_node_new(ACCESSKIT_ROLE_WINDOW);
        accesskit_node_set_label_with_length(root, title.data(), title.size());
        accesskit_node_set_bounds(root, rect(bounds));
        for (const auto id : children[1]) {
            accesskit_node_push_child(root, id);
        }
        if (!announcement.empty()) {
            accesskit_node_push_child(root, announcement_id);
        }
        accesskit_tree_update_push_node(update, 1, root);
        for (const auto& e : elements) {
            const u64 id = native(e.id);
            auto* node = accesskit_node_new(role(e.role));
            accesskit_node_set_label_with_length(node, e.label.data(), e.label.size());
            accesskit_node_set_value_with_length(node, e.value.data(), e.value.size());
            accesskit_node_set_bounds(node, rect(e.bounds));
            if (e.numeric_value) {
                accesskit_node_set_numeric_value(node, *e.numeric_value);
            }
            if (e.numeric_min) {
                accesskit_node_set_min_numeric_value(node, *e.numeric_min);
            }
            if (e.numeric_max) {
                accesskit_node_set_max_numeric_value(node, *e.numeric_max);
            }
            if (e.numeric_step) {
                accesskit_node_set_numeric_value_step(node, *e.numeric_step);
            }
            if (e.role == "text_view") {
                accesskit_node_set_read_only(node);
            }
            if (!e.visible || e.blocked) {
                accesskit_node_set_hidden(node);
            }
            if (!e.enabled || e.blocked) {
                accesskit_node_set_disabled(node);
            }
            if (e.role == "toggle") {
                accesskit_node_set_toggled(node, e.checked ? ACCESSKIT_TOGGLED_TRUE : ACCESSKIT_TOGGLED_FALSE);
            }
            if (e.focusable && e.available()) {
                // Its main action: a number or a text to set, else a click.
                accesskit_node_add_action(node, ACCESSKIT_ACTION_FOCUS);
                if (semantic_role_accepts(e.role, SemanticAction::SetNumber)) {
                    accesskit_node_add_action(node, ACCESSKIT_ACTION_INCREMENT);
                    accesskit_node_add_action(node, ACCESSKIT_ACTION_DECREMENT);
                    accesskit_node_add_action(node, ACCESSKIT_ACTION_SET_VALUE);
                } else if (semantic_role_accepts(e.role, SemanticAction::SetText)) {
                    accesskit_node_add_action(node, ACCESSKIT_ACTION_SET_VALUE);
                } else {
                    accesskit_node_add_action(node, ACCESSKIT_ACTION_CLICK);
                }
            }
            for (const auto child : children[id]) {
                accesskit_node_push_child(node, child);
            }
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

    static void action(accesskit_action_request* request, void* data) {
        try {
            auto& self = *static_cast<Impl*>(data);
            std::lock_guard lock{self.mutex};
            const u64 target = request->target_node;
            const bool has_value = request->data.has_value;
            switch (request->action) {
            case ACCESSKIT_ACTION_FOCUS:
                self.enqueue(target, SemanticAction::Focus, {}, 0);
                break;
            case ACCESSKIT_ACTION_CLICK:
                self.enqueue(target, SemanticAction::Click, {}, 0);
                break;
            case ACCESSKIT_ACTION_INCREMENT:
                self.enqueue(target, SemanticAction::Increment, {}, 0);
                break;
            case ACCESSKIT_ACTION_DECREMENT:
                self.enqueue(target, SemanticAction::Decrement, {}, 0);
                break;
            case ACCESSKIT_ACTION_SET_VALUE:
                if (has_value && request->data.value.tag == ACCESSKIT_ACTION_DATA_NUMERIC_VALUE) {
                    self.enqueue(target, SemanticAction::SetNumber, {}, request->data.value.numeric_value);
                } else if (has_value && request->data.value.tag == ACCESSKIT_ACTION_DATA_VALUE &&
                           request->data.value.value) {
                    self.enqueue(target, SemanticAction::SetText, request->data.value.value, 0);
                }
                break;
            default:
                break;
            }
        } catch (...) {
        } // callbacks must not unwind through the C ABI
        accesskit_action_request_free(request);
    }

    // Last: made after what its callbacks read, and gone before it.
    std::unique_ptr<AccessKitAdapter> adapter;
#endif

    Impl(Window& w, Renderer2D& r, std::string t) : window(w), renderer(r), title(std::move(t)) {
        bounds = {0, 0, static_cast<f32>(w.width()), static_cast<f32>(w.height())};
#if defined(KIN_HAS_ACCESSKIT)
        if (title.empty()) {
            title = AccessKitAdapter::window_title(w);
        }
        adapter = std::make_unique<AccessKitAdapter>(w, make_tree, action, this);
#endif
    }

    void publish() {
#if defined(KIN_HAS_ACCESSKIT)
        adapter->publish();
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
        // One action per UI frame: while the Context has one pending, the rest wait.
        while (!s.actions.empty() && !context.action_pending()) {
            Impl::Action action = std::move(s.actions.front());
            s.actions.pop_front();
            const auto semantic = s.semantic_of.find(action.native);
            if (semantic == s.semantic_of.end()) {
                continue; // its element is gone
            }
            std::string error;
            if (context.queue_action(Id{semantic->second}, action.action, std::move(action.text), action.number,
                                     error)) {
                break;
            }
        }
        s.elements = context.elements();
        ++s.updates;
        for (auto& e : s.elements) {
            auto& native = s.ids[e.id.value];
            if (!native.native) {
                native.native = s.next_id++;
                s.semantic_of[native.native] = e.id.value;
            }
            native.seen = s.updates;
            const auto a = s.renderer.logical_to_window({e.bounds.x, e.bounds.y});
            const auto b = s.renderer.logical_to_window({e.bounds.x + e.bounds.w, e.bounds.y + e.bounds.h});
            e.bounds = {a.x, a.y, b.x - a.x, b.y - a.y};
        }
        for (auto it = s.ids.begin(); it != s.ids.end();) {
            if (it->second.seen == s.updates) {
                ++it;
                continue;
            }
            s.semantic_of.erase(it->second.native);
            it = s.ids.erase(it);
        }
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
    f64 number = 0;
    if (action == SemanticAction::SetNumber) {
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), number);
        if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(number)) {
            return false;
        }
    }
    std::lock_guard lock{_impl->mutex};
    const auto found = _impl->ids.find(id.value);
    return found != _impl->ids.end() && _impl->enqueue(found->second.native, action, std::move(text), number);
}

std::vector<SemanticElement> Accessibility::snapshot() const {
    std::lock_guard lock{_impl->mutex};
    return _impl->elements;
}

} // namespace kin::ui2
