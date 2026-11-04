#include <kin/platform/input_router.hpp>

#include <kin/assets/content.hpp>
#include <kin/core/json.hpp>
#include <kin/core/json_value.hpp>
#include <kin/platform/user_data.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <sstream>
#include <stdexcept>

namespace kin {
namespace {

constexpr std::string_view overrides_schema = "kin.input-overrides/1";

// A stick's (or trigger's) analog input as one physical control: its digital
// ways and the axis claim each other.
constexpr u64 stick_key(PadAxis axis) {
    return (u64{3} << 32) | static_cast<u32>(axis);
}

std::optional<PadAxis> stick_of(PadControl control) {
    switch (control) {
    case PadControl::LeftStickLeft:
    case PadControl::LeftStickRight:
        return PadAxis::LeftX;
    case PadControl::LeftStickUp:
    case PadControl::LeftStickDown:
        return PadAxis::LeftY;
    case PadControl::RightStickLeft:
    case PadControl::RightStickRight:
        return PadAxis::RightX;
    case PadControl::RightStickUp:
    case PadControl::RightStickDown:
        return PadAxis::RightY;
    case PadControl::LeftTrigger:
        return PadAxis::LeftTrigger;
    case PadControl::RightTrigger:
        return PadAxis::RightTrigger;
    default:
        return std::nullopt;
    }
}

u64 physical(InputBinding binding) {
    if (binding.device == InputBindingDevice::Gamepad) {
        if (const auto axis = stick_of(static_cast<PadControl>(binding.code))) {
            return stick_key(*axis);
        }
    }
    return (static_cast<u64>(binding.device) << 32) | static_cast<u32>(binding.code);
}

bool modifiers_match(const Input& input, InputBinding binding) {
    return binding.device != InputBindingDevice::Key || input.modifier_held(binding.modifiers);
}

bool down(const Input& input, InputBinding binding) {
    return modifiers_match(input, binding) && binding_held(input, binding);
}

bool pressed(const Input& input, InputBinding binding) {
    return modifiers_match(input, binding) && binding_pressed(input, binding);
}

bool stick_engaged(const Input& input, const InputAxis& axis, InputAxisStick stick) {
    const GamepadState& pad = input.gamepad();
    return pad.connected && std::abs(pad_axis_raw(pad, stick.axis)) > std::clamp(axis.dead_zone, 0.0f, 0.95f);
}

KeyModifiers modifiers(const Input& input) {
    KeyModifiers result = KeyModifiers::None;
    for (const KeyModifiers value : {KeyModifiers::Shift, KeyModifiers::Ctrl, KeyModifiers::Alt}) {
        if (input.modifier_held(value)) {
            result = result | value;
        }
    }
    return result;
}

// Every key with a name, the mouse buttons and the controller's controls.
const std::vector<InputBinding>& bindable_controls() {
    static const std::vector<InputBinding> controls = [] {
        std::vector<InputBinding> result;
        for (i32 key = 1; key <= static_cast<i32>(LastKey); ++key) {
            const auto name = key_name(static_cast<Key>(key));
            if (!name.empty() && name != "Unknown") {
                result.push_back({InputBindingDevice::Key, key});
            }
        }
        for (i32 button = 0; button <= static_cast<i32>(MouseButton::X2); ++button) {
            result.push_back({InputBindingDevice::MouseButton, button});
        }
        for (i32 pad = 0; pad < static_cast<i32>(PadControl::Count); ++pad) {
            result.push_back({InputBindingDevice::Gamepad, pad});
        }
        return result;
    }();
    return controls;
}

// Drops the results of actions or axes no longer in the map.
template <class Results, class Map> void forget_missing(Results& results, const Map& map) {
    if (results.size() != map.size()) {
        std::erase_if(results, [&](const auto& entry) { return !map.contains(entry.first); });
    }
}

template <class T, class Results> T& result_for(Results& results, const std::string& name) {
    auto found = results.find(name);
    if (found == results.end()) {
        found = results.emplace(name, T{}).first;
    }
    return found->second;
}

} // namespace

InputRouter::Routed* InputRouter::find(std::string_view name) {
    const auto it = std::ranges::find_if(_layers, [&](const Routed& routed) { return routed.layer.name == name; });
    return it == _layers.end() ? nullptr : &*it;
}

const InputRouter::Routed* InputRouter::find(std::string_view name) const {
    const auto it = std::ranges::find_if(_layers, [&](const Routed& routed) { return routed.layer.name == name; });
    return it == _layers.end() ? nullptr : &*it;
}

void InputRouter::push(Layer layer) {
    if (layer.name.empty() || find(layer.name)) {
        throw std::invalid_argument("input layer name must be unique and nonempty");
    }
    layer.defaults = layer.map;
    _layers.push_back({.layer = std::move(layer)});
    ++_revision;
}

bool InputRouter::remove(std::string_view name) {
    const bool removed = std::erase_if(_layers, [&](const Routed& routed) { return routed.layer.name == name; }) != 0;
    if (removed) {
        ++_revision;
    }
    return removed;
}

const InputRouter::Layer* InputRouter::layer(std::string_view name) const {
    const Routed* routed = find(name);
    return routed ? &routed->layer : nullptr;
}

bool InputRouter::set_enabled(std::string_view name, bool enabled) {
    Routed* routed = find(name);
    if (!routed) {
        return false;
    }
    routed->layer.enabled = enabled;
    return true;
}

bool InputRouter::set_map(std::string_view name, InputMap map) {
    Routed* routed = find(name);
    if (!routed) {
        return false;
    }
    routed->layer.map = std::move(map);
    ++_revision;
    return true;
}

RoutedAction InputRouter::action(std::string_view layer, std::string_view action) const {
    const Routed* routed = find(layer);
    if (!routed) {
        return {};
    }
    const auto it = routed->actions.find(action);
    return it == routed->actions.end() ? RoutedAction{} : it->second;
}

f32 InputRouter::axis(std::string_view layer, std::string_view axis) const {
    const Routed* routed = find(layer);
    if (!routed) {
        return 0.0f;
    }
    const auto it = routed->axes.find(axis);
    return it == routed->axes.end() ? 0.0f : it->second;
}

void InputRouter::update(const Input& input) {
    _claimed.clear();
    const auto claimed = [&](u64 control) { return std::ranges::find(_claimed, control) != _claimed.end(); };
    bool suspended = false;
    for (auto it = _layers.rbegin(); it != _layers.rend(); ++it) {
        Routed& routed = *it;
        const Layer& layer = routed.layer;
        const bool live = layer.enabled && !suspended;
        _used.clear();
        for (const auto& [name, bindings] : layer.map._bindings) {
            bool held = false;
            bool edge = false;
            if (live) {
                for (const InputBinding binding : bindings) {
                    const u64 control = physical(binding);
                    if (claimed(control)) {
                        continue;
                    }
                    const bool active = down(input, binding);
                    const bool press = pressed(input, binding);
                    held |= active;
                    edge |= press;
                    if (active || press) {
                        _used.push_back(control);
                    }
                }
            }
            RoutedAction& result = result_for<RoutedAction>(routed.actions, name);
            const bool was = result.held;
            result = {.held = held, .pressed = edge && !was, .released = was && !held};
        }
        forget_missing(routed.actions, layer.map._bindings);

        for (const auto& [name, axis] : layer.map._axes) {
            f32 value = 0.0f;
            if (live) {
                const auto binding_claimed = [&](InputBinding binding) { return claimed(physical(binding)); };
                const auto stick_claimed = [&](InputAxisStick stick) { return claimed(stick_key(stick.axis)); };
                if (std::ranges::none_of(axis.negative, binding_claimed) &&
                    std::ranges::none_of(axis.positive, binding_claimed) &&
                    std::ranges::none_of(axis.sticks, stick_claimed)) {
                    value = axis_value(input, axis);
                } else {
                    InputAxis open = axis; // what a higher layer left of it
                    std::erase_if(open.negative, binding_claimed);
                    std::erase_if(open.positive, binding_claimed);
                    std::erase_if(open.sticks, stick_claimed);
                    value = axis_value(input, open);
                }
                for (const auto* side : {&axis.negative, &axis.positive}) {
                    for (const InputBinding binding : *side) {
                        if (!binding_claimed(binding) && down(input, binding)) {
                            _used.push_back(physical(binding));
                        }
                    }
                }
                for (const InputAxisStick stick : axis.sticks) {
                    if (!stick_claimed(stick) && stick_engaged(input, axis, stick)) {
                        _used.push_back(stick_key(stick.axis));
                    }
                }
            }
            result_for<f32>(routed.axes, name) = value;
        }
        forget_missing(routed.axes, layer.map._axes);

        if (live && layer.consume) {
            _claimed.insert(_claimed.end(), _used.begin(), _used.end());
        }
        if (layer.enabled && layer.modal) {
            suspended = true;
        }
    }
}

void InputRebinder::begin(const Input& input, std::string action, f32 timeout) {
    if (action.empty() || !std::isfinite(timeout) || timeout < 0) {
        throw std::invalid_argument("invalid rebinding action/timeout");
    }
    _action = std::move(action);
    _remaining = timeout;
    _candidate.reset();
    _blocked.clear();
    _state = RebindState::Listening;
    // What is down, or went down this step, belongs to whatever started the capture.
    for (const InputBinding binding : bindable_controls()) {
        if (binding_held(input, binding) || binding_pressed(input, binding)) {
            _blocked.push_back(binding);
        }
    }
}

RebindState InputRebinder::update(const Input& input, f32 dt) {
    if (!std::isfinite(dt) || dt < 0) {
        throw std::invalid_argument("invalid rebinding dt");
    }
    if (_state != RebindState::Listening) {
        return _state;
    }
    if (input.pressed(Key::Escape)) {
        return _state = RebindState::Cancelled;
    }
    for (InputBinding binding : bindable_controls()) {
        if (!binding_pressed(input, binding) || std::ranges::find(_blocked, binding) != _blocked.end()) {
            continue;
        }
        if (binding.device == InputBindingDevice::Key) {
            binding.modifiers = modifiers(input);
        }
        _candidate = binding;
        return _state = RebindState::Captured;
    }
    std::erase_if(_blocked, [&](InputBinding binding) { return !binding_held(input, binding); });
    _remaining -= dt;
    if (_remaining <= 0) {
        _state = RebindState::TimedOut;
    }
    return _state;
}

std::vector<std::string> InputRebinder::conflicts(const InputMap& map) const {
    std::vector<std::string> result;
    if (_candidate) {
        for (const auto& action : map.actions()) {
            if (action.name != _action && std::ranges::find(action.bindings, *_candidate) != action.bindings.end()) {
                result.push_back(action.name);
            }
        }
    }
    return result;
}

bool InputRebinder::apply(InputMap& map, RebindConflict policy, std::string& error) {
    if (!_candidate || _state != RebindState::Captured) {
        error = "no captured binding";
        return false;
    }
    const auto other = conflicts(map);
    if (!other.empty() && policy == RebindConflict::Reject) {
        error = "binding conflicts with " + other.front();
        return false;
    }
    if (policy == RebindConflict::Replace) {
        for (const auto& name : other) {
            auto bindings = *map.bindings(name);
            std::erase(bindings, *_candidate);
            map.set_bindings(name, std::move(bindings));
        }
    }
    std::vector<InputBinding> bindings = map.bindings(_action) ? *map.bindings(_action) : std::vector<InputBinding>{};
    std::erase_if(bindings, [&](InputBinding binding) { return binding.device == _candidate->device; });
    bindings.push_back(*_candidate);
    map.set_bindings(_action, std::move(bindings));
    error.clear();
    return true;
}

std::string serialize_input_overrides(const InputMap& defaults, const InputMap& effective) {
    std::ostringstream out;
    JsonWriter json(out, false);
    json.begin_object().field("schema", overrides_schema);
    // Each name either map has whose entry differs, as a one-entry input file
    // (none when the effective map dropped it).
    const auto write = [&](std::string_view kind, const auto& names_of, const auto& find, const auto& put) {
        std::set<std::string> names;
        for (const InputMap* map : {&defaults, &effective}) {
            for (const auto& item : names_of(*map)) {
                names.insert(item.name);
            }
        }
        json.key(kind).begin_array();
        for (const auto& name : names) {
            const auto* before = find(defaults, name);
            const auto* after = find(effective, name);
            if ((!before && !after) || (before && after && *before == *after)) {
                continue;
            }
            InputMap one;
            if (after) {
                put(one, name, *after);
            }
            json.begin_object().field("name", name).field("map", serialize_input_map(one)).end_object();
        }
        json.end_array();
    };
    write(
        "actions", [](const InputMap& map) { return map.actions(); },
        [](const InputMap& map, const std::string& name) { return map.bindings(name); },
        [](InputMap& map, const std::string& name, const std::vector<InputBinding>& bindings) {
            map.set_bindings(name, bindings);
        });
    write(
        "axes", [](const InputMap& map) { return map.axes(); },
        [](const InputMap& map, const std::string& name) { return map.axis(name); },
        [](InputMap& map, const std::string&, const InputAxis& axis) { map.set_axis(axis); });
    json.end_object();
    return out.str();
}

bool apply_input_overrides(std::string_view text, const InputMap& defaults, InputMap& output, std::string& error) {
    const auto parsed = parse_json(text);
    if (!parsed.ok() || parsed.value->string_at("schema") != overrides_schema) {
        error = "invalid input override document";
        return false;
    }
    InputMap result = defaults;
    for (const bool actions : {true, false}) {
        const std::string kind = actions ? "actions" : "axes";
        const auto* entries = parsed.value->find(kind);
        if (!entries || !entries->is_array()) {
            error = "override arrays are required";
            return false;
        }
        std::set<std::string> seen;
        for (const auto& entry : entries->items()) {
            const auto name = entry.string_at("name");
            InputMap one;
            if (name.empty() || !seen.insert(name).second || !entry.find("map") || !entry.find("map")->is_string() ||
                !parse_input_map(entry.string_at("map"), one, error)) {
                error = "invalid/duplicate override: " + name;
                return false;
            }
            // Nothing but this one entry, of this kind (or nothing: it is dropped).
            const std::size_t axis_count = one.axes().size();
            const std::size_t action_count = static_cast<std::size_t>(one.action_count());
            const bool fits = actions
                                  ? axis_count == 0 && action_count <= 1 && (action_count == 0 || one.bindings(name))
                                  : action_count == 0 && axis_count <= 1 && (axis_count == 0 || one.axis(name));
            if (!fits) {
                error = "unexpected " + std::string(actions ? "action" : "axis") + " override contents";
                return false;
            }
            if (actions) {
                result.set_bindings(name, one.bindings(name) ? *one.bindings(name) : std::vector<InputBinding>{});
            } else if (one.axis(name)) {
                result.set_axis(*one.axis(name));
            } else {
                result.remove_axis(name);
            }
        }
    }
    output = std::move(result);
    error.clear();
    return true;
}

bool save_input_overrides(const InputMap& defaults, const InputMap& effective, const std::filesystem::path& path) {
    return write_user_file(path, serialize_input_overrides(defaults, effective));
}

bool load_input_overrides(const InputMap& defaults, InputMap& output, const std::filesystem::path& path,
                          std::string& error) {
    const auto text = read_content_text(path);
    if (!text) {
        error = "cannot read input overrides";
        return false;
    }
    return apply_input_overrides(*text, defaults, output, error);
}

std::filesystem::path input_profile_path(std::string_view profile, std::string& error) {
    if (profile.empty() || profile.size() > 80 ||
        !std::ranges::all_of(profile, [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '-'; })) {
        error = "invalid input profile name";
        return {};
    }
    const std::string& game = user_data_game_id();
    if (game.empty()) {
        error = "input profiles need the game's id (set_user_data_game_id)";
        return {};
    }
    return game_data_dir(game) / "input" / (std::string(profile) + ".json");
}

} // namespace kin
