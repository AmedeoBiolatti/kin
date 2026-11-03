#include <kin/ui2/context.hpp>
#include <kin/core/json.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace kin::ui2 {

SemanticElement* Context::element(Id id) {
    const auto found = _element_indices.find(id.value);
    return found == _element_indices.end() ? nullptr : &_elements[found->second];
}

bool Context::accepts_input(Id id) const {
    if (const auto found = _element_indices.find(id.value); found != _element_indices.end()) {
        const auto& e = _elements[found->second];
        return e.enabled && !e.blocked;
    }
    return !_modal_blocking || !_modal_stack.empty();
}

void Context::semantic_key(Id id, std::string_view key) {
    _semantic_keys[id.value] = key;
    if (auto* e = element(id)) e->key = key;
}

void Context::semantic_parent(Id id, Id parent) {
    if (auto* e = element(id)) e->parent = parent;
}

void Context::describe(Id id, std::string_view role, std::string_view label, Rectf bounds,
                       bool enabled, bool focusable, std::string_view value, bool checked) {
    if (!id) return;
    auto [found, added] = _element_indices.emplace(id.value, _elements.size());
    if (added) _elements.push_back(SemanticElement{.id = id});
    auto& e = _elements[found->second];
    e.role = role;
    e.label = label;
    e.value = value;
    e.bounds = to_screen(bounds);
    if (!_clip_stack.empty()) {
        const auto clip = _clip_stack.back();
        const f32 x = std::max(e.bounds.x, clip.x), y = std::max(e.bounds.y, clip.y);
        e.bounds = {x, y, std::max(0.0f, std::min(e.bounds.x + e.bounds.w, clip.x + clip.w) - x),
                          std::max(0.0f, std::min(e.bounds.y + e.bounds.h, clip.y + clip.h) - y)};
    }
    e.visible = e.bounds.w > 0 && e.bounds.h > 0;
    e.enabled = enabled;
    e.focusable = focusable;
    e.checked = checked;
    e.focused = _state.is_focused(id);
    e.modal = _modal_stack.empty() ? Id{} : _open_modal;
    e.blocked = _modal_blocking && _open_modal && e.modal != _open_modal && id != _open_modal;
    if (const auto key = _semantic_keys.find(id.value); key != _semantic_keys.end()) e.key = key->second;
    e.parent = _running_node >= 0 && _running_node < static_cast<i32>(_node_owner.size())
                   ? _node_owner[static_cast<std::size_t>(_running_node)]
                   : !_compose_ids.empty() ? _compose_ids.back() : Id{};
    if (!e.parent && e.modal && e.modal != id) e.parent = e.modal;
    if (_semantic_action && _semantic_action->id == id && enabled && e.visible && !e.blocked &&
        _semantic_action->action == SemanticAction::Focus) {
        _state.set_focused(id);
        _semantic_action.reset();
    }
}

std::optional<Id> Context::resolve_action(std::string_view selector, SemanticAction action, std::string& error) const {
    const SemanticElement* target = nullptr;
    for (const auto& e : _elements) {
        if (selector != e.key && selector != e.label && selector != std::to_string(e.id.value)) continue;
        if (target) { error = "ambiguous UI selector; use its numeric id"; return std::nullopt; }
        target = &e;
    }
    if (!target || selector.empty()) { error = "UI element not found"; return std::nullopt; }
    if (!target->enabled || !target->visible || target->blocked) {
        error = "UI element is disabled, hidden or blocked"; return std::nullopt;
    }
    if (!target->focusable) { error = "UI element does not accept actions"; return std::nullopt; }
    if (action == SemanticAction::SetText && target->role != "text_input" && target->role != "text_edit") {
        error = "set_text requires a text input"; return std::nullopt;
    }
    return target->id;
}

bool Context::queue_action(std::string_view selector, SemanticAction action, std::string text, std::string& error) {
    const auto target = resolve_action(selector, action, error);
    if (!target) return false;
    if (_semantic_action) { error = "a UI action is already pending; advance a frame first"; return false; }
    _semantic_action = PendingSemanticAction{*target, action, std::move(text)};
    return true;
}

std::optional<std::string> Context::take_text_action(Id id) {
    const auto* e = element(id);
    if (!_semantic_action || _semantic_action->id != id || _semantic_action->action != SemanticAction::SetText ||
        !e || !e->enabled || !e->visible || e->blocked) return std::nullopt;
    std::string text = std::move(_semantic_action->text);
    _semantic_action.reset();
    _state.set_focused(id);
    return text;
}

void Context::write_snapshot(JsonWriter& json) const {
    json.begin_object();
    json.field("frame", static_cast<i64>(_state.frame()));
    json.key("elements").begin_array();
    for (const auto& e : _elements) {
        json.begin_object();
        json.field("id", std::to_string(e.id.value)); // u64 ids must survive JSON/JavaScript round trips
        json.field("parent", std::to_string(e.parent.value));
        json.field("key", e.key);
        json.field("role", e.role);
        json.field("label", e.label);
        json.field("value", e.value);
        json.field("enabled", e.enabled);
        json.field("visible", e.visible);
        json.field("blocked", e.blocked);
        json.field("focusable", e.focusable);
        json.field("focused", _state.is_focused(e.id));
        json.field("checked", e.checked);
        json.key("bounds").begin_object();
        json.field("x", e.bounds.x); json.field("y", e.bounds.y);
        json.field("w", e.bounds.w); json.field("h", e.bounds.h);
        json.end_object();
        json.end_object();
    }
    json.end_array();
    json.end_object();
}

void Context::finish_semantics() {
    for (auto& e : _elements) {
        e.blocked = _modal_blocking && _open_modal && e.modal != _open_modal && e.id != _open_modal;
        if (!_focus_trap || e.id == _focus_trap) continue;
        Id parent = e.parent;
        for (std::size_t guard = 0; parent && parent != _focus_trap && guard < 256; ++guard) {
            const auto found = _element_indices.find(parent.value);
            if (found == _element_indices.end()) { parent = {}; break; }
            parent = _elements[found->second].parent;
        }
        if (parent != _focus_trap) e.blocked = true;
    }
}

void Context::begin_navigation() {
    _controller_accept = _controller_back = false;
    _controller_direction = 0;
    _controller_direction_consumed = false;
    if (!_input) return;
    const auto& pad = _input->gamepad();
    if (_controller_options.enabled && pad.connected) {
        _controller_accept = pad.button(GamepadButton::A) && !_previous_pad.button(GamepadButton::A);
        _controller_back = pad.button(GamepadButton::B) && !_previous_pad.button(GamepadButton::B);
        i32 direction = 0;
        const f32 dead = std::clamp(_controller_options.dead_zone, 0.1f, 0.99f);
        if (pad.button(GamepadButton::DPadUp)) direction = 1;
        else if (pad.button(GamepadButton::DPadRight)) direction = 2;
        else if (pad.button(GamepadButton::DPadDown)) direction = 3;
        else if (pad.button(GamepadButton::DPadLeft)) direction = 4;
        else if (std::max(std::abs(pad.left_x), std::abs(pad.left_y)) >= dead)
            direction = std::abs(pad.left_x) > std::abs(pad.left_y) ? (pad.left_x > 0 ? 2 : 4) : (pad.left_y > 0 ? 1 : 3);
        if (direction != _held_direction) {
            _controller_direction = direction;
            _controller_repeat = std::max(0.0f, _controller_options.repeat_delay);
        } else if (direction) {
            _controller_repeat -= std::max(0.0f, _dt);
            if (_controller_repeat <= 0) {
                _controller_direction = direction;
                _controller_repeat = std::max(0.01f, _controller_options.repeat_interval);
            }
        }
        _held_direction = direction;
        if (_controller_direction || _controller_accept || _controller_back) _controller_active = true;
    } else {
        _held_direction = 0;
    }
    if (_any_press || _input->frame_pressed(Key::Tab) || _input->frame_pressed(Key::Enter)) _controller_active = false;
    _previous_pad = pad;
}

bool Context::controller_key(Key key) const {
    if (key == Key::Enter) return _controller_accept;
    if (key == Key::Escape) return _controller_back;
    const i32 direction = key == Key::Up ? 1 : key == Key::Right ? 2 : key == Key::Down ? 3 : key == Key::Left ? 4 : 0;
    if (direction && direction == _controller_direction) {
        _controller_direction_consumed = true;
        return true;
    }
    return false;
}

void Context::finish_navigation() {
    if (!_controller_direction || _controller_direction_consumed || _focus_move || _focus_order.empty()) return;
    const Id focused = _state.focused();
    const SemanticElement* origin = element(focused);
    Id best{};
    f32 score = std::numeric_limits<f32>::infinity();
    for (const auto& candidate : _focus_order) {
        const auto* e = element(candidate.id);
        if (!e || !e->enabled || !e->visible || e->blocked || e->id == focused) continue;
        if (!origin) { best = e->id; break; }
        const f32 dx = e->bounds.x + e->bounds.w * 0.5f - origin->bounds.x - origin->bounds.w * 0.5f;
        const f32 dy = e->bounds.y + e->bounds.h * 0.5f - origin->bounds.y - origin->bounds.h * 0.5f;
        const bool vertical = _controller_direction == 1 || _controller_direction == 3;
        const f32 forward = (_controller_direction == 1 ? -dy : _controller_direction == 2 ? dx :
                             _controller_direction == 3 ? dy : -dx);
        if (forward <= 0.01f) continue;
        const f32 side = std::abs(vertical ? dx : dy);
        const f32 distance = forward + side * 2 + side * side / forward;
        if (distance < score) { score = distance; best = e->id; }
    }
    if (best) { _state.set_focused(best); reveal({best, -1}); _focus_moved = true; }
}

} // namespace kin::ui2
