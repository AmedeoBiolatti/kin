#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <kin/assets/content.hpp>
#include <kin/core/json.hpp>
#include <kin/core/json_value.hpp>
#include <kin/platform/input_router.hpp>
#include <set>
#include <sstream>
#include <stdexcept>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace kin {
namespace {
u64 physical(InputBinding b) {
    if (b.device == InputBindingDevice::Gamepad) {
        const auto control = static_cast<PadControl>(b.code);
        std::optional<PadAxis> axis;
        switch (control) {
        case PadControl::LeftStickLeft:
        case PadControl::LeftStickRight:
            axis = PadAxis::LeftX;
            break;
        case PadControl::LeftStickUp:
        case PadControl::LeftStickDown:
            axis = PadAxis::LeftY;
            break;
        case PadControl::RightStickLeft:
        case PadControl::RightStickRight:
            axis = PadAxis::RightX;
            break;
        case PadControl::RightStickUp:
        case PadControl::RightStickDown:
            axis = PadAxis::RightY;
            break;
        case PadControl::LeftTrigger:
            axis = PadAxis::LeftTrigger;
            break;
        case PadControl::RightTrigger:
            axis = PadAxis::RightTrigger;
            break;
        default:
            break;
        }
        if (axis)
            return (3ull << 32) | static_cast<u32>(*axis);
    }
    return (static_cast<u64>(b.device) << 32) | static_cast<u32>(b.code);
}
bool down(const Input& input, InputBinding b) {
    return (b.device != InputBindingDevice::Key || input.modifier_held(b.modifiers)) && binding_held(input, b);
}
bool pressed(const Input& input, InputBinding b) {
    switch (b.device) {
    case InputBindingDevice::Key:
        return input.modifier_held(b.modifiers) && input.pressed(static_cast<Key>(b.code));
    case InputBindingDevice::MouseButton:
        return input.mouse_pressed(static_cast<MouseButton>(b.code));
    case InputBindingDevice::Gamepad:
        return input.pad_pressed(static_cast<PadControl>(b.code));
    }
    return false;
}
KeyModifiers modifiers(const Input& input) {
    KeyModifiers result = KeyModifiers::None;
    for (auto value : {KeyModifiers::Shift, KeyModifiers::Ctrl, KeyModifiers::Alt})
        if (input.modifier_held(value))
            result = result | value;
    return result;
}
std::vector<InputBinding> controls() {
    std::vector<InputBinding> result;
    for (i32 key = 1; key <= static_cast<i32>(LastKey); ++key) {
        const auto name = key_name(static_cast<Key>(key));
        if (!name.empty() && name != "Unknown")
            result.push_back({InputBindingDevice::Key, key});
    }
    for (i32 button = 0; button < 5; ++button)
        result.push_back({InputBindingDevice::MouseButton, button});
    for (i32 pad = 0; pad < static_cast<i32>(PadControl::Count); ++pad)
        result.push_back({InputBindingDevice::Gamepad, pad});
    return result;
}
} // namespace

void InputRouter::push(Layer layer) {
    if (layer.name.empty() || this->layer(layer.name))
        throw std::invalid_argument("input layer name must be unique and nonempty");
    _layers.push_back(std::move(layer));
}
bool InputRouter::remove(std::string_view name) {
    const bool removed = std::erase_if(_layers, [&](const auto& value) { return value.name == name; }) != 0;
    std::erase_if(_actions, [&](const auto& value) { return value.first.first == name; });
    std::erase_if(_axes, [&](const auto& value) { return value.first.first == name; });
    return removed;
}
InputRouter::Layer* InputRouter::layer(std::string_view name) {
    const auto it = std::ranges::find(_layers, name, &Layer::name);
    return it == _layers.end() ? nullptr : &*it;
}
RoutedAction InputRouter::action(std::string_view layer, std::string_view action) const {
    const auto it = _actions.find({std::string(layer), std::string(action)});
    return it == _actions.end() ? RoutedAction{} : it->second;
}
f32 InputRouter::axis(std::string_view layer, std::string_view axis) const {
    const auto it = _axes.find({std::string(layer), std::string(axis)});
    return it == _axes.end() ? 0.0f : it->second;
}
void InputRouter::update(const Input& input) {
    auto previous = std::move(_actions);
    _actions.clear();
    _axes.clear();
    std::unordered_set<u64> claimed;
    bool suspended = false;
    for (auto it = _layers.rbegin(); it != _layers.rend(); ++it) {
        const auto& layer = *it;
        std::vector<u64> used;
        for (const auto& action : layer.map.actions()) {
            const auto key = std::pair{layer.name, action.name};
            bool held = false, edge = false;
            if (layer.enabled && !suspended)
                for (const auto b : action.bindings) {
                    if (claimed.contains(physical(b)))
                        continue;
                    const bool active = down(input, b);
                    const bool press = pressed(input, b);
                    held |= active;
                    edge |= press;
                    if (active || press)
                        used.push_back(physical(b));
                }
            const bool was = previous.contains(key) && previous.at(key).held;
            _actions[key] = {held, edge && !was, was && !held};
        }
        for (auto axis : layer.map.axes()) {
            const auto key = std::pair{layer.name, axis.name};
            f32 value = 0;
            if (layer.enabled && !suspended) {
                const auto filtered = [&](auto& bindings) {
                    std::erase_if(bindings, [&](auto b) { return claimed.contains(physical(b)); });
                    for (auto b : bindings)
                        if (down(input, b))
                            used.push_back(physical(b));
                };
                filtered(axis.negative);
                filtered(axis.positive);
                std::erase_if(axis.sticks, [&](auto stick) {
                    return claimed.contains((3ull << 32) | static_cast<u32>(stick.axis));
                });
                value = axis_value(input, axis);
                for (auto stick : axis.sticks) {
                    InputAxis single = axis;
                    single.negative.clear();
                    single.positive.clear();
                    single.sticks = {stick};
                    if (axis_value(input, single) != 0)
                        used.push_back((3ull << 32) | static_cast<u32>(stick.axis));
                }
            }
            _axes[key] = value;
        }
        if (layer.enabled && !suspended && layer.consume)
            claimed.insert(used.begin(), used.end());
        if (layer.enabled && layer.modal)
            suspended = true;
    }
}

void InputRebinder::begin(const Input& input, std::string action, f32 timeout) {
    if (action.empty() || !std::isfinite(timeout) || timeout < 0)
        throw std::invalid_argument("invalid rebinding action/timeout");
    _action = std::move(action);
    _remaining = timeout;
    _candidate.reset();
    _blocked.clear();
    _state = RebindState::Listening;
    _previous_pad = input.gamepad();
    for (auto b : controls())
        if (binding_held(input, b))
            _blocked.push_back(b);
}
RebindState InputRebinder::update(const Input& input, f32 dt) {
    if (!std::isfinite(dt) || dt < 0)
        throw std::invalid_argument("invalid rebinding dt");
    if (_state != RebindState::Listening)
        return _state;
    std::erase_if(_blocked, [&](auto b) { return !binding_held(input, b); });
    if (input.frame_pressed(Key::Escape))
        return _state = RebindState::Cancelled;
    for (auto b : controls()) {
        if (std::ranges::find(_blocked, b) != _blocked.end())
            continue;
        bool edge = b.device == InputBindingDevice::Key ? input.frame_pressed(static_cast<Key>(b.code))
                    : b.device == InputBindingDevice::MouseButton
                        ? input.mouse_frame_pressed(static_cast<MouseButton>(b.code))
                        : pad_control_held(input.gamepad(), static_cast<PadControl>(b.code)) &&
                              !pad_control_held(_previous_pad, static_cast<PadControl>(b.code));
        if (!edge)
            continue;
        if (b.device == InputBindingDevice::Key)
            b.modifiers = modifiers(input);
        _candidate = b;
        return _state = RebindState::Captured;
    }
    _previous_pad = input.gamepad();
    _remaining -= dt;
    if (_remaining <= 0)
        _state = RebindState::TimedOut;
    return _state;
}
std::vector<std::string> InputRebinder::conflicts(const InputMap& map) const {
    std::vector<std::string> result;
    if (_candidate)
        for (const auto& action : map.actions())
            if (action.name != _action && std::ranges::find(action.bindings, *_candidate) != action.bindings.end())
                result.push_back(action.name);
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
    if (policy == RebindConflict::Replace)
        for (const auto& name : other) {
            auto bindings = *map.bindings(name);
            std::erase(bindings, *_candidate);
            map.set_bindings(name, std::move(bindings));
        }
    map.set_bindings(_action, {*_candidate});
    error.clear();
    return true;
}

std::string serialize_input_overrides(const InputMap& defaults, const InputMap& effective) {
    std::ostringstream out;
    JsonWriter json(out, false);
    json.begin_object().field("schema", "kin.input-overrides/1");
    json.key("actions").begin_array();
    std::set<std::string> names;
    for (const auto& a : defaults.actions())
        names.insert(a.name);
    for (const auto& a : effective.actions())
        names.insert(a.name);
    for (const auto& name : names) {
        const auto* before = defaults.bindings(name);
        const auto* after = effective.bindings(name);
        if ((!before && !after) || (before && after && *before == *after))
            continue;
        InputMap one;
        if (after)
            one.set_bindings(name, *after);
        json.begin_object().field("name", name).field("map", serialize_input_map(one)).end_object();
    }
    json.end_array().key("axes").begin_array();
    names.clear();
    for (const auto& a : defaults.axes())
        names.insert(a.name);
    for (const auto& a : effective.axes())
        names.insert(a.name);
    for (const auto& name : names) {
        const auto* before = defaults.axis(name);
        const auto* after = effective.axis(name);
        if ((!before && !after) || (before && after && *before == *after))
            continue;
        InputMap one;
        if (after)
            one.set_axis(*after);
        json.begin_object().field("name", name).field("map", serialize_input_map(one)).end_object();
    }
    json.end_array().end_object();
    return out.str();
}
bool apply_input_overrides(std::string_view text, const InputMap& defaults, InputMap& output, std::string& error) {
    const auto parsed = parse_json(text);
    if (!parsed.ok() || parsed.value->string_at("schema") != "kin.input-overrides/1") {
        error = "invalid input override document";
        return false;
    }
    InputMap result = defaults;
    for (const std::string kind : {"actions", "axes"}) {
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
            if (kind == "actions") {
                if (!one.axes().empty() || one.actions().size() > 1 ||
                    (!one.actions().empty() && one.actions().front().name != name)) {
                    error = "unexpected action override contents";
                    return false;
                }
                result.set_bindings(name, one.bindings(name) ? *one.bindings(name) : std::vector<InputBinding>{});
            } else {
                if (!one.actions().empty() || one.axes().size() > 1 ||
                    (!one.axes().empty() && one.axes().front().name != name)) {
                    error = "unexpected axis override contents";
                    return false;
                }
                if (one.axis(name))
                    result.set_axis(*one.axis(name));
                else
                    result.remove_axis(name);
            }
        }
    }
    output = std::move(result);
    error.clear();
    return true;
}
bool save_input_overrides(const InputMap& defaults, const InputMap& effective, const std::filesystem::path& path) {
    std::error_code error;
    if (!path.parent_path().empty())
        std::filesystem::create_directories(path.parent_path(), error);
    if (error)
        return false;
    static std::atomic<u64> sequence{0};
    auto temporary = path;
    temporary += ".tmp." + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "." +
                 std::to_string(sequence.fetch_add(1));
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << serialize_input_overrides(defaults, effective);
        out.flush();
        out.close();
        if (!out) {
            std::filesystem::remove(temporary, error);
            return false;
        }
    }
#if defined(_WIN32)
    if (MoveFileExW(temporary.native().c_str(), path.native().c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return true;
#else
    std::filesystem::rename(temporary, path, error);
    if (!error)
        return true;
#endif
    std::filesystem::remove(temporary, error);
    return false;
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
} // namespace kin
