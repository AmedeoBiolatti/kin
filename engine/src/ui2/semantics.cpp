#include <kin/ui2/semantics.hpp>

#include <kin/core/json.hpp>
#include <kin/ui2/context.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <span>
#include <utility>

namespace kin::ui2 {
namespace {

// Slots and overrides not used for this many frames are dropped.
constexpr u64 stale_frames = 120;

bool text_role(std::string_view role) {
    return role == "text_input" || role == "text_edit";
}

bool numeric_role(std::string_view role) {
    return role == "slider" || role == "spinbutton";
}

bool numeric_action(SemanticAction action) {
    return action == SemanticAction::Increment || action == SemanticAction::Decrement ||
           action == SemanticAction::SetNumber;
}

bool valid_range(f64 value, f64 min, f64 max, f64 step) {
    return std::isfinite(value) && std::isfinite(min) && std::isfinite(max) && std::isfinite(step) && min <= max &&
           step >= 0;
}

bool parse_number(std::string_view text, f64& value) {
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size() && std::isfinite(value);
}

} // namespace

bool semantic_role_accepts(std::string_view role, SemanticAction action) {
    switch (action) {
    case SemanticAction::Click:
    case SemanticAction::Focus:
        return true;
    case SemanticAction::SetText:
        return text_role(role);
    case SemanticAction::Increment:
    case SemanticAction::Decrement:
    case SemanticAction::SetNumber:
        return numeric_role(role);
    }
    return false;
}

const SemanticElement* Context::element(Id id) const {
    const auto found = _element_slots.find(id.value);
    if (found == _element_slots.end() || found->second.frame != _state.frame() ||
        found->second.index >= _element_count) {
        return nullptr;
    }
    return &_elements[found->second.index];
}

SemanticElement* Context::element(Id id) {
    return const_cast<SemanticElement*>(std::as_const(*this).element(id));
}

SemanticElement& Context::claim_element(Id id) {
    const u64 frame = _state.frame();
    auto [slot, added] = _element_slots.try_emplace(id.value);
    if (!added && slot->second.frame == frame && slot->second.index < _element_count) {
        return _elements[slot->second.index];
    }
    slot->second = {.index = _element_count++, .frame = frame};
    if (slot->second.index == _elements.size()) {
        _elements.emplace_back();
    }
    // A fresh element in a slot an earlier frame used: its strings keep their storage.
    SemanticElement& e = _elements[slot->second.index];
    const auto emptied = [](std::string& text) {
        std::string kept = std::move(text);
        kept.clear();
        return kept;
    };
    e = SemanticElement{
        .id = id, .key = emptied(e.key), .role = emptied(e.role), .label = emptied(e.label), .value = emptied(e.value)};
    return e;
}

Context::SemanticOverride& Context::semantic_override(Id id) {
    SemanticOverride& entry = _semantic_overrides[id.value];
    if (entry.frame != _state.frame()) {
        entry.key.clear();
        entry.label.clear();
        entry.frame = _state.frame();
    }
    return entry;
}

void Context::begin_semantics() {
    _element_count = 0;
    const u64 frame = _state.frame();
    if (frame % stale_frames == 0) {
        std::erase_if(_element_slots, [frame](const auto& kv) { return kv.second.frame + stale_frames < frame; });
        std::erase_if(_semantic_overrides, [frame](const auto& kv) { return kv.second.frame + stale_frames < frame; });
    }
}

bool Context::accepts_input(Id id) const {
    if (const SemanticElement* e = element(id)) {
        return e->enabled && !e->blocked;
    }
    return !_modal_blocking || !_modal_stack.empty();
}

void Context::semantic_key(Id id, std::string_view key) {
    semantic_override(id).key = key;
    if (auto* e = element(id)) {
        e->key = key;
    }
}

void Context::semantic_parent(Id id, Id parent) {
    if (auto* e = element(id)) {
        e->parent = parent;
    }
}

void Context::semantic_label(Id id, std::string_view label) {
    semantic_override(id).label = label;
    if (auto* e = element(id)) {
        e->label = label;
    }
}

void Context::semantic_numeric(Id id, f64 value, f64 min, f64 max, f64 step) {
    if (!valid_range(value, min, max, step)) {
        return;
    }
    if (auto* e = element(id)) {
        e->numeric_value = value;
        e->numeric_min = min;
        e->numeric_max = max;
        e->numeric_step = step;
    }
}

std::optional<f64> Context::take_numeric_action(Id id, f64 value, f64 min, f64 max, f64 step) {
    const auto* e = element(id);
    if (!_semantic_action || _semantic_action->id != id || !e || !e->available() ||
        !numeric_action(_semantic_action->action) || !valid_range(value, min, max, step)) {
        return std::nullopt;
    }
    if (_semantic_action->action == SemanticAction::SetNumber) {
        value = _semantic_action->number;
    } else {
        value += _semantic_action->action == SemanticAction::Increment ? step : -step;
    }
    _semantic_action.reset();
    _state.set_focused(id);
    return std::clamp(value, min, max);
}

void Context::describe(Id id, std::string_view role, std::string_view label, Rectf bounds, bool enabled, bool focusable,
                       std::string_view value, bool checked) {
    if (!id) {
        return;
    }
    SemanticElement& e = claim_element(id);
    e.role = role;
    e.label = label;
    e.value = value;
    if (const auto found = _semantic_overrides.find(id.value);
        found != _semantic_overrides.end() && found->second.frame == _state.frame()) {
        if (!found->second.key.empty()) {
            e.key = found->second.key;
        }
        if (!found->second.label.empty()) {
            e.label = found->second.label;
        }
    }
    e.bounds = to_screen(bounds);
    if (!_clip_stack.empty()) {
        const auto clip = _clip_stack.back();
        const f32 x = std::max(e.bounds.x, clip.x);
        const f32 y = std::max(e.bounds.y, clip.y);
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
    e.parent = _running_node >= 0 && _running_node < static_cast<i32>(_node_owner.size())
                   ? _node_owner[static_cast<std::size_t>(_running_node)]
               : !_compose_ids.empty() ? _compose_ids.back()
                                       : Id{};
    if (!e.parent && e.modal && e.modal != id) {
        e.parent = e.modal;
    }
    if (_semantic_action && _semantic_action->id == id && enabled && e.visible && !e.blocked &&
        _semantic_action->action == SemanticAction::Focus) {
        _state.set_focused(id);
        _semantic_action.reset();
    }
}

void Context::semantic_state(Id id, std::string_view value, bool checked) {
    if (auto* e = element(id)) {
        e->value = value;
        e->checked = checked;
    }
}

bool Context::keyboard_activated(const Interaction& it) const {
    return it.focused && !_wants_text_input_last &&
           (key_pressed(Key::Enter) || key_pressed(Key::KeypadEnter) || key_pressed(Key::Space));
}

std::optional<Id> Context::resolve_action(const SemanticElement* target, SemanticAction action,
                                          std::string& error) const {
    if (!target) {
        error = "UI element not found";
        return std::nullopt;
    }
    if (!target->available()) {
        error = "UI element is disabled, hidden or blocked";
        return std::nullopt;
    }
    if (!target->focusable) {
        error = "UI element does not accept actions";
        return std::nullopt;
    }
    if (!semantic_role_accepts(target->role, action)) {
        error = action == SemanticAction::SetText ? "set_text requires a text input"
                                                  : "numeric action requires a slider or number input";
        return std::nullopt;
    }
    return target->id;
}

std::optional<Id> Context::resolve_action(std::string_view selector, SemanticAction action, std::string& error) const {
    if (selector.empty()) {
        error = "UI element not found";
        return std::nullopt;
    }
    u64 number = 0;
    const auto [end, code] = std::from_chars(selector.data(), selector.data() + selector.size(), number);
    const bool numeric = code == std::errc{} && end == selector.data() + selector.size();
    const SemanticElement* target = nullptr;
    for (const SemanticElement& e : std::span(_elements).first(_element_count)) {
        if (selector != e.key && selector != e.label && !(numeric && e.id.value == number)) {
            continue;
        }
        if (target) {
            error = "ambiguous UI selector; use its numeric id";
            return std::nullopt;
        }
        target = &e;
    }
    return resolve_action(target, action, error);
}

bool Context::queue_action(std::string_view selector, SemanticAction action, std::string text, std::string& error) {
    const auto target = resolve_action(selector, action, error);
    if (!target) {
        return false;
    }
    f64 number = 0;
    if (action == SemanticAction::SetNumber && !parse_number(text, number)) {
        error = "set_number requires finite numeric text";
        return false;
    }
    return queue_action(*target, action, std::move(text), number, error);
}

bool Context::queue_action(Id target, SemanticAction action, std::string text, f64 number, std::string& error) {
    if (!resolve_action(element(target), action, error)) {
        return false;
    }
    if (action == SemanticAction::SetNumber && !std::isfinite(number)) {
        error = "set_number requires a finite number";
        return false;
    }
    if (_semantic_action) {
        error = "a UI action is already pending; advance a frame first";
        return false;
    }
    _semantic_action = PendingSemanticAction{target, action, std::move(text), number};
    return true;
}

std::optional<std::string> Context::take_text_action(Id id) {
    const auto* e = element(id);
    if (!_semantic_action || _semantic_action->id != id || _semantic_action->action != SemanticAction::SetText || !e ||
        !e->available()) {
        return std::nullopt;
    }
    std::string text = std::move(_semantic_action->text);
    _semantic_action.reset();
    _state.set_focused(id);
    return text;
}

void Context::write_snapshot(JsonWriter& json) const {
    json.begin_object();
    json.field("frame", static_cast<i64>(_state.frame()));
    json.key("elements").begin_array();
    for (const SemanticElement& e : std::span(_elements).first(_element_count)) {
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
        json.field("focused", e.focused);
        json.field("checked", e.checked);
        if (e.numeric_value) {
            json.field("numeric_value", *e.numeric_value);
        }
        if (e.numeric_min) {
            json.field("numeric_min", *e.numeric_min);
        }
        if (e.numeric_max) {
            json.field("numeric_max", *e.numeric_max);
        }
        if (e.numeric_step) {
            json.field("numeric_step", *e.numeric_step);
        }
        json.key("bounds").begin_object();
        json.field("x", e.bounds.x);
        json.field("y", e.bounds.y);
        json.field("w", e.bounds.w);
        json.field("h", e.bounds.h);
        json.end_object();
        json.end_object();
    }
    json.end_array();
    json.end_object();
}

void Context::finish_semantics() {
    _elements.resize(_element_count); // what an earlier, larger frame left
    for (auto& e : _elements) {
        e.blocked = _modal_blocking && _open_modal && e.modal != _open_modal && e.id != _open_modal;
        if (!_focus_trap || e.id == _focus_trap) {
            continue;
        }
        Id parent = e.parent;
        for (std::size_t guard = 0; parent && parent != _focus_trap && guard < 256; ++guard) {
            const SemanticElement* above = element(parent);
            if (!above) {
                parent = {};
                break;
            }
            parent = above->parent;
        }
        if (parent != _focus_trap) {
            e.blocked = true;
        }
    }
}

void Context::begin_navigation() {
    _controller_accept = _controller_back = false;
    _controller_direction = NavDirection::None;
    _controller_direction_consumed = false;
    if (!_input) {
        return;
    }
    const i32 player = _controller_options.player;
    const auto& pad = player < 0 ? _input->gamepad() : _input->player_gamepad(player);
    // The map's menu actions, held: a direction when the controller gives none.
    NavDirection held_action = NavDirection::None;
    if (_controller_options.enabled && _controller_options.actions) {
        held_action = _input->held(player, "menu_up")      ? NavDirection::Up
                      : _input->held(player, "menu_right") ? NavDirection::Right
                      : _input->held(player, "menu_down")  ? NavDirection::Down
                      : _input->held(player, "menu_left")  ? NavDirection::Left
                                                           : NavDirection::None;
    }
    const bool accept_action = _controller_options.enabled && _controller_options.actions && _input->frame_pressed(player, "accept");
    const bool back_action = _controller_options.enabled && _controller_options.actions && _input->frame_pressed(player, "quit");
    if (_controller_options.enabled && (pad.connected || held_action != NavDirection::None || accept_action || back_action)) {
        const auto pad_pressed = [&](PadControl control) {
            return pad.connected && (player < 0 ? _input->pad_frame_pressed(control) : _input->pad_frame_pressed(control, player));
        };
        const bool pad_accept = pad_pressed(PadControl::A);
        const bool pad_back = pad_pressed(PadControl::B);
        _controller_accept = pad_accept || accept_action;
        _controller_back = pad_back || back_action;
        NavDirection direction = NavDirection::None;
        const f32 dead = std::clamp(_controller_options.dead_zone, 0.1f, 0.99f);
        if (pad.button(GamepadButton::DPadUp)) {
            direction = NavDirection::Up;
        } else if (pad.button(GamepadButton::DPadRight)) {
            direction = NavDirection::Right;
        } else if (pad.button(GamepadButton::DPadDown)) {
            direction = NavDirection::Down;
        } else if (pad.button(GamepadButton::DPadLeft)) {
            direction = NavDirection::Left;
        } else if (pad.connected && std::max(std::abs(pad.left_x), std::abs(pad.left_y)) >= dead) {
            direction = std::abs(pad.left_x) > std::abs(pad.left_y)
                            ? (pad.left_x > 0 ? NavDirection::Right : NavDirection::Left)
                            : (pad.left_y > 0 ? NavDirection::Up : NavDirection::Down);
        }
        const bool by_pad = direction != NavDirection::None;
        if (!by_pad) {
            direction = held_action;
        }
        if (direction != _held_direction) {
            _controller_direction = direction;
            _controller_repeat = std::max(0.0f, _controller_options.repeat_delay);
        } else if (direction != NavDirection::None) {
            _controller_repeat -= std::max(0.0f, _dt);
            if (_controller_repeat <= 0) {
                _controller_direction = direction;
                _controller_repeat = std::max(0.01f, _controller_options.repeat_interval);
            }
        }
        _held_direction = direction;
        if ((by_pad && _controller_direction != NavDirection::None) || pad_accept || pad_back) {
            _controller_active = true;
        } else if ((!by_pad && _controller_direction != NavDirection::None) || accept_action || back_action) {
            _controller_active = false; // the keys (or whatever the actions are bound to) navigate now
        }
    } else {
        _held_direction = NavDirection::None;
    }
    if (_any_press || _input->frame_pressed(Key::Tab) || _input->frame_pressed(Key::Enter)) {
        _controller_active = false;
    }
}

bool Context::controller_key(Key key) const {
    if (key == Key::Enter) {
        return _controller_accept;
    }
    if (key == Key::Escape) {
        return _controller_back;
    }
    const NavDirection direction = key == Key::Up      ? NavDirection::Up
                                   : key == Key::Right ? NavDirection::Right
                                   : key == Key::Down  ? NavDirection::Down
                                   : key == Key::Left  ? NavDirection::Left
                                                       : NavDirection::None;
    if (direction != NavDirection::None && direction == _controller_direction) {
        _controller_direction_consumed = true;
        return true;
    }
    return false;
}

void Context::finish_navigation() {
    if (_controller_direction == NavDirection::None || _controller_direction_consumed || _focus_move ||
        _focus_order.empty()) {
        return;
    }
    const Id focused = _state.focused();
    const SemanticElement* origin = element(focused);
    const bool vertical = _controller_direction == NavDirection::Up || _controller_direction == NavDirection::Down;
    Id best{};
    f32 score = std::numeric_limits<f32>::infinity();
    for (const auto& candidate : _focus_order) {
        const auto* e = element(candidate.id);
        if (!e || !e->available() || e->id == focused) {
            continue;
        }
        if (!origin) {
            best = e->id;
            break;
        }
        const f32 dx = e->bounds.x + e->bounds.w * 0.5f - origin->bounds.x - origin->bounds.w * 0.5f;
        const f32 dy = e->bounds.y + e->bounds.h * 0.5f - origin->bounds.y - origin->bounds.h * 0.5f;
        const f32 forward = _controller_direction == NavDirection::Up      ? -dy
                            : _controller_direction == NavDirection::Right ? dx
                            : _controller_direction == NavDirection::Down  ? dy
                                                                           : -dx;
        if (forward <= 0.01f) {
            continue;
        }
        const f32 side = std::abs(vertical ? dx : dy);
        const f32 distance = forward + side * 2 + side * side / forward;
        if (distance < score) {
            score = distance;
            best = e->id;
        }
    }
    if (best) {
        _state.set_focused(best);
        reveal({best, -1});
        _focus_moved = true;
    }
}

} // namespace kin::ui2
