#include <kin/platform/input.hpp>

#include <kin/assets/content.hpp>
#include <kin/core/utf8.hpp>
#include <kin/platform/log.hpp>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace kin {
namespace {

constexpr i32 INPUT_KEY_COUNT = 512;
constexpr i32 INPUT_MOUSE_BUTTON_COUNT = 6;

bool valid_scancode(i32 key) {
    return key >= 0 && key < INPUT_KEY_COUNT;
}

bool valid_mouse_button(u8 button) {
    return button > 0 && button < INPUT_MOUSE_BUTTON_COUNT;
}

f32 length_squared(Vec2f value) {
    return value.x * value.x + value.y * value.y;
}

SDL_Scancode to_sdl_scancode(Key key) {
    switch (key) {
    case Key::Escape: return SDL_SCANCODE_ESCAPE;
    case Key::Space: return SDL_SCANCODE_SPACE;
    case Key::Enter: return SDL_SCANCODE_RETURN;
    case Key::KeypadEnter: return SDL_SCANCODE_KP_ENTER;
    case Key::Tab: return SDL_SCANCODE_TAB;
    case Key::Backspace: return SDL_SCANCODE_BACKSPACE;
    case Key::Delete: return SDL_SCANCODE_DELETE;
    case Key::Home: return SDL_SCANCODE_HOME;
    case Key::End: return SDL_SCANCODE_END;
    case Key::F1: return SDL_SCANCODE_F1;
    case Key::F2: return SDL_SCANCODE_F2;
    case Key::Num0: return SDL_SCANCODE_0;
    case Key::Num1: return SDL_SCANCODE_1;
    case Key::Num2: return SDL_SCANCODE_2;
    case Key::Num3: return SDL_SCANCODE_3;
    case Key::Num4: return SDL_SCANCODE_4;
    case Key::Num5: return SDL_SCANCODE_5;
    case Key::Num6: return SDL_SCANCODE_6;
    case Key::Num7: return SDL_SCANCODE_7;
    case Key::Num8: return SDL_SCANCODE_8;
    case Key::Num9: return SDL_SCANCODE_9;
    case Key::Up: return SDL_SCANCODE_UP;
    case Key::Down: return SDL_SCANCODE_DOWN;
    case Key::Left: return SDL_SCANCODE_LEFT;
    case Key::Right: return SDL_SCANCODE_RIGHT;
    case Key::A: return SDL_SCANCODE_A;
    case Key::B: return SDL_SCANCODE_B;
    case Key::C: return SDL_SCANCODE_C;
    case Key::D: return SDL_SCANCODE_D;
    case Key::E: return SDL_SCANCODE_E;
    case Key::F: return SDL_SCANCODE_F;
    case Key::G: return SDL_SCANCODE_G;
    case Key::H: return SDL_SCANCODE_H;
    case Key::I: return SDL_SCANCODE_I;
    case Key::J: return SDL_SCANCODE_J;
    case Key::K: return SDL_SCANCODE_K;
    case Key::L: return SDL_SCANCODE_L;
    case Key::M: return SDL_SCANCODE_M;
    case Key::N: return SDL_SCANCODE_N;
    case Key::O: return SDL_SCANCODE_O;
    case Key::P: return SDL_SCANCODE_P;
    case Key::Q: return SDL_SCANCODE_Q;
    case Key::R: return SDL_SCANCODE_R;
    case Key::S: return SDL_SCANCODE_S;
    case Key::T: return SDL_SCANCODE_T;
    case Key::U: return SDL_SCANCODE_U;
    case Key::V: return SDL_SCANCODE_V;
    case Key::W: return SDL_SCANCODE_W;
    case Key::X: return SDL_SCANCODE_X;
    case Key::Y: return SDL_SCANCODE_Y;
    case Key::Z: return SDL_SCANCODE_Z;
    case Key::Comma: return SDL_SCANCODE_COMMA;
    case Key::Period: return SDL_SCANCODE_PERIOD;
    case Key::Minus: return SDL_SCANCODE_MINUS;
    case Key::Equals: return SDL_SCANCODE_EQUALS;
    case Key::Backslash: return SDL_SCANCODE_BACKSLASH;
    case Key::Grave: return SDL_SCANCODE_GRAVE;
    case Key::LeftAlt: return SDL_SCANCODE_LALT;
    case Key::F3: return SDL_SCANCODE_F3;
    case Key::F4: return SDL_SCANCODE_F4;
    case Key::F5: return SDL_SCANCODE_F5;
    case Key::F6: return SDL_SCANCODE_F6;
    case Key::F7: return SDL_SCANCODE_F7;
    case Key::F8: return SDL_SCANCODE_F8;
    case Key::F9: return SDL_SCANCODE_F9;
    case Key::F10: return SDL_SCANCODE_F10;
    case Key::F11: return SDL_SCANCODE_F11;
    case Key::F12: return SDL_SCANCODE_F12;
    case Key::PageUp: return SDL_SCANCODE_PAGEUP;
    case Key::PageDown: return SDL_SCANCODE_PAGEDOWN;
    case Key::LeftShift: return SDL_SCANCODE_LSHIFT;
    case Key::Unknown: return SDL_SCANCODE_UNKNOWN;
    }
    return SDL_SCANCODE_UNKNOWN;
}

u8 to_sdl_mouse_button(MouseButton button) {
    switch (button) {
    case MouseButton::Left: return SDL_BUTTON_LEFT;
    case MouseButton::Middle: return SDL_BUTTON_MIDDLE;
    case MouseButton::Right: return SDL_BUTTON_RIGHT;
    case MouseButton::X1: return SDL_BUTTON_X1;
    case MouseButton::X2: return SDL_BUTTON_X2;
    }
    return 0;
}

Key to_key(i32 code) {
    if (code < static_cast<i32>(Key::Unknown) || code > static_cast<i32>(LastKey)) {
        return Key::Unknown;
    }
    return static_cast<Key>(code);
}

MouseButton to_mouse_button(i32 code) {
    if (code < static_cast<i32>(MouseButton::Left) || code > static_cast<i32>(MouseButton::X2)) {
        return MouseButton::Left;
    }
    return static_cast<MouseButton>(code);
}

InputBinding key_binding(Key key, KeyModifiers modifiers = KeyModifiers::None) {
    return {
        .device = InputBindingDevice::Key,
        .code = static_cast<i32>(key),
        .modifiers = modifiers,
    };
}

std::size_t pad_index(InputBinding binding) {
    return binding.code >= 0 ? static_cast<std::size_t>(binding.code) : static_cast<std::size_t>(-1);
}

InputBinding mouse_binding(MouseButton button) {
    return {
        .device = InputBindingDevice::MouseButton,
        .code = static_cast<i32>(button),
    };
}

SDL_Scancode binding_scancode(InputBinding binding) {
    return to_sdl_scancode(to_key(binding.code));
}

u8 binding_mouse_button(InputBinding binding) {
    return to_sdl_mouse_button(to_mouse_button(binding.code));
}

bool has_modifier(KeyModifiers value, KeyModifiers modifier) {
    return any(value & modifier);
}

std::string lower_copy(std::string_view value) {
    std::string result{value};
    std::ranges::transform(result, result.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return result;
}

std::string trim_comment(std::string line) {
    if (const auto comment = line.find('#'); comment != std::string::npos) {
        line.erase(comment);
    }
    return line;
}

} // namespace

std::string_view key_name(Key key) {
    switch (key) {
    case Key::Unknown: return "Unknown";
    case Key::Escape: return "Escape";
    case Key::Space: return "Space";
    case Key::Enter: return "Enter";
    case Key::KeypadEnter: return "KeypadEnter";
    case Key::Tab: return "Tab";
    case Key::Backspace: return "Backspace";
    case Key::Delete: return "Delete";
    case Key::Home: return "Home";
    case Key::End: return "End";
    case Key::F1: return "F1";
    case Key::F2: return "F2";
    case Key::Num0: return "0";
    case Key::Num1: return "1";
    case Key::Num2: return "2";
    case Key::Num3: return "3";
    case Key::Num4: return "4";
    case Key::Num5: return "5";
    case Key::Num6: return "6";
    case Key::Num7: return "7";
    case Key::Num8: return "8";
    case Key::Num9: return "9";
    case Key::Up: return "Up";
    case Key::Down: return "Down";
    case Key::Left: return "Left";
    case Key::Right: return "Right";
    case Key::A: return "A";
    case Key::B: return "B";
    case Key::C: return "C";
    case Key::D: return "D";
    case Key::E: return "E";
    case Key::F: return "F";
    case Key::G: return "G";
    case Key::H: return "H";
    case Key::I: return "I";
    case Key::J: return "J";
    case Key::K: return "K";
    case Key::L: return "L";
    case Key::M: return "M";
    case Key::N: return "N";
    case Key::O: return "O";
    case Key::P: return "P";
    case Key::Q: return "Q";
    case Key::R: return "R";
    case Key::S: return "S";
    case Key::T: return "T";
    case Key::U: return "U";
    case Key::V: return "V";
    case Key::W: return "W";
    case Key::X: return "X";
    case Key::Y: return "Y";
    case Key::Z: return "Z";
    case Key::Comma: return "Comma";
    case Key::Period: return "Period";
    case Key::Minus: return "Minus";
    case Key::Equals: return "Equals";
    case Key::Backslash: return "Backslash";
    case Key::Grave: return "Grave";
    case Key::LeftAlt: return "LeftAlt";
    case Key::F3: return "F3";
    case Key::F4: return "F4";
    case Key::F5: return "F5";
    case Key::F6: return "F6";
    case Key::F7: return "F7";
    case Key::F8: return "F8";
    case Key::F9: return "F9";
    case Key::F10: return "F10";
    case Key::F11: return "F11";
    case Key::F12: return "F12";
    case Key::PageUp: return "PageUp";
    case Key::PageDown: return "PageDown";
    case Key::LeftShift: return "LeftShift";
    }
    return "Unknown";
}

std::string_view mouse_button_name(MouseButton button) {
    switch (button) {
    case MouseButton::Left: return "MouseLeft";
    case MouseButton::Middle: return "MouseMiddle";
    case MouseButton::Right: return "MouseRight";
    case MouseButton::X1: return "MouseX1";
    case MouseButton::X2: return "MouseX2";
    }
    return "MouseUnknown";
}

std::string_view binding_name(InputBinding binding) {
    if (binding.device == InputBindingDevice::Key) {
        return key_name(to_key(binding.code));
    }
    if (binding.device == InputBindingDevice::Gamepad) {
        return pad_control_name(static_cast<PadControl>(binding.code));
    }
    if (binding.device == InputBindingDevice::Touch) {
        return touch_control_name(static_cast<TouchControl>(binding.code));
    }
    return mouse_button_name(to_mouse_button(binding.code));
}

std::string_view touch_control_name(TouchControl control) {
    switch (control) {
    case TouchControl::Tap: return "tap";
    case TouchControl::DoubleTap: return "double_tap";
    case TouchControl::LongPress: return "long_press";
    case TouchControl::SwipeLeft: return "swipe_left";
    case TouchControl::SwipeRight: return "swipe_right";
    case TouchControl::SwipeUp: return "swipe_up";
    case TouchControl::SwipeDown: return "swipe_down";
    case TouchControl::TwoFingerTap: return "two_finger_tap";
    case TouchControl::Count: break;
    }
    return "unknown";
}

bool parse_touch_control(std::string_view name, TouchControl& out) {
    const std::string target = lower_copy(name);
    for (u8 code = 0; code < static_cast<u8>(TouchControl::Count); ++code) {
        if (touch_control_name(static_cast<TouchControl>(code)) == target) {
            out = static_cast<TouchControl>(code);
            return true;
        }
    }
    return false;
}

std::string_view pad_control_name(PadControl control) {
    switch (control) {
    case PadControl::A: return "a";
    case PadControl::B: return "b";
    case PadControl::X: return "x";
    case PadControl::Y: return "y";
    case PadControl::Back: return "back";
    case PadControl::Start: return "start";
    case PadControl::LeftStick: return "left_stick";
    case PadControl::RightStick: return "right_stick";
    case PadControl::LeftShoulder: return "left_shoulder";
    case PadControl::RightShoulder: return "right_shoulder";
    case PadControl::DPadUp: return "dpad_up";
    case PadControl::DPadDown: return "dpad_down";
    case PadControl::DPadLeft: return "dpad_left";
    case PadControl::DPadRight: return "dpad_right";
    case PadControl::LeftStickLeft: return "left_stick_left";
    case PadControl::LeftStickRight: return "left_stick_right";
    case PadControl::LeftStickUp: return "left_stick_up";
    case PadControl::LeftStickDown: return "left_stick_down";
    case PadControl::RightStickLeft: return "right_stick_left";
    case PadControl::RightStickRight: return "right_stick_right";
    case PadControl::RightStickUp: return "right_stick_up";
    case PadControl::RightStickDown: return "right_stick_down";
    case PadControl::LeftTrigger: return "left_trigger";
    case PadControl::RightTrigger: return "right_trigger";
    case PadControl::Count: break;
    }
    return "unknown";
}

std::string_view pad_axis_name(PadAxis axis) {
    switch (axis) {
    case PadAxis::LeftX: return "left_x";
    case PadAxis::LeftY: return "left_y";
    case PadAxis::RightX: return "right_x";
    case PadAxis::RightY: return "right_y";
    case PadAxis::LeftTrigger: return "left_trigger";
    case PadAxis::RightTrigger: return "right_trigger";
    }
    return "left_x";
}

bool parse_pad_control(std::string_view name, PadControl& out) {
    const std::string target = lower_copy(name);
    for (u8 code = 0; code < static_cast<u8>(PadControl::Count); ++code) {
        if (pad_control_name(static_cast<PadControl>(code)) == target) {
            out = static_cast<PadControl>(code);
            return true;
        }
    }
    return false;
}

bool parse_pad_axis(std::string_view name, PadAxis& out) {
    const std::string target = lower_copy(name);
    for (u8 code = 0; code <= static_cast<u8>(PadAxis::RightTrigger); ++code) {
        if (pad_axis_name(static_cast<PadAxis>(code)) == target) {
            out = static_cast<PadAxis>(code);
            return true;
        }
    }
    return false;
}

std::string binding_text(InputBinding binding) {
    switch (binding.device) {
    case InputBindingDevice::Key: return "key:" + std::string(key_name(to_key(binding.code)));
    case InputBindingDevice::MouseButton: return "mouse:" + std::string(mouse_button_name(to_mouse_button(binding.code)));
    case InputBindingDevice::Gamepad: return "pad:" + std::string(pad_control_name(static_cast<PadControl>(binding.code)));
    case InputBindingDevice::Touch: return "touch:" + std::string(touch_control_name(static_cast<TouchControl>(binding.code)));
    }
    return {};
}

std::optional<InputBinding> parse_binding_text(std::string_view text) {
    const std::size_t colon = text.find(':');
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view device = text.substr(0, colon);
    const std::string_view name = text.substr(colon + 1);
    if (device == "key") {
        Key key = Key::Unknown;
        return parse_key(name, key) ? std::optional(key_binding(key)) : std::nullopt;
    }
    if (device == "mouse") {
        MouseButton button = MouseButton::Left;
        return parse_mouse_button(name, button) ? std::optional(mouse_binding(button)) : std::nullopt;
    }
    if (device == "pad") {
        PadControl control = PadControl::A;
        return parse_pad_control(name, control)
                   ? std::optional(InputBinding{.device = InputBindingDevice::Gamepad, .code = static_cast<i32>(control)})
                   : std::nullopt;
    }
    if (device == "touch") {
        TouchControl control = TouchControl::Tap;
        return parse_touch_control(name, control)
                   ? std::optional(InputBinding{.device = InputBindingDevice::Touch, .code = static_cast<i32>(control)})
                   : std::nullopt;
    }
    return std::nullopt;
}

bool parse_key(std::string_view name, Key& out) {
    const std::string target = lower_copy(name);
    for (i32 code = static_cast<i32>(Key::Unknown); code <= static_cast<i32>(LastKey); ++code) {
        const Key key = static_cast<Key>(code);
        if (lower_copy(key_name(key)) == target) {
            out = key;
            return key != Key::Unknown;
        }
    }
    return false;
}

bool parse_mouse_button(std::string_view name, MouseButton& out) {
    const std::string target = lower_copy(name);
    for (i32 code = static_cast<i32>(MouseButton::Left); code <= static_cast<i32>(MouseButton::X2); ++code) {
        const MouseButton button = static_cast<MouseButton>(code);
        if (lower_copy(mouse_button_name(button)) == target) {
            out = button;
            return true;
        }
    }
    return false;
}

bool parse_modifier(std::string_view name, KeyModifiers& out) {
    const std::string target = lower_copy(name);
    if (target == "shift") {
        out = out | KeyModifiers::Shift;
        return true;
    }
    if (target == "ctrl" || target == "control") {
        out = out | KeyModifiers::Ctrl;
        return true;
    }
    if (target == "alt") {
        out = out | KeyModifiers::Alt;
        return true;
    }
    return false;
}

void write_modifiers(std::ostream& out, KeyModifiers modifiers) {
    if (has_modifier(modifiers, KeyModifiers::Shift)) {
        out << " Shift";
    }
    if (has_modifier(modifiers, KeyModifiers::Ctrl)) {
        out << " Ctrl";
    }
    if (has_modifier(modifiers, KeyModifiers::Alt)) {
        out << " Alt";
    }
}

bool parse_input_map(std::string_view text, InputMap& out, std::string& error) {
    std::istringstream file{std::string(text)};
    InputMap map;
    std::string line;
    i32 line_no = 0;
    const auto fail = [&](std::string_view message) {
        error = "line " + std::to_string(line_no) + ": " + std::string{message};
        return false;
    };
    while (std::getline(file, line)) {
        ++line_no;
        std::istringstream in(trim_comment(std::move(line)));
        std::string kind;
        if (!(in >> kind)) {
            continue;
        }
        if (kind == "key") {
            std::string action;
            std::string key_name_text;
            if (!(in >> action >> key_name_text)) {
                return fail("key requires action and key name");
            }
            Key key = Key::Unknown;
            if (!parse_key(key_name_text, key)) {
                return fail("unknown key");
            }
            KeyModifiers modifiers = KeyModifiers::None;
            for (std::string modifier; in >> modifier;) {
                if (!parse_modifier(modifier, modifiers)) {
                    return fail("unknown key modifier");
                }
            }
            map.bind(action, key, modifiers);
            continue;
        }
        if (kind == "mouse") {
            std::string action;
            std::string button_name_text;
            if (!(in >> action >> button_name_text)) {
                return fail("mouse requires action and button name");
            }
            MouseButton button = MouseButton::Left;
            if (!parse_mouse_button(button_name_text, button)) {
                return fail("unknown mouse button");
            }
            map.bind(action, button);
            continue;
        }
        if (kind == "touch") {
            std::string action;
            std::string control_name;
            if (!(in >> action >> control_name)) {
                return fail("touch requires action and gesture name");
            }
            TouchControl control = TouchControl::Tap;
            if (!parse_touch_control(control_name, control)) {
                return fail("unknown touch gesture");
            }
            map.bind(action, control);
            continue;
        }
        if (kind == "pad") {
            std::string action;
            std::string control_name;
            if (!(in >> action >> control_name)) {
                return fail("pad requires action and control name");
            }
            PadControl control = PadControl::A;
            if (!parse_pad_control(control_name, control)) {
                return fail("unknown pad control");
            }
            map.bind(action, control);
            continue;
        }
        if (kind == "axis") {
            std::string name;
            std::string what;
            std::string value;
            if (!(in >> name >> what >> value)) {
                return fail("axis requires a name, then -, +, stick or deadzone, and its value");
            }
            InputAxis axis = map.axis(name) ? *map.axis(name) : InputAxis{.name = name};
            if (what == "-" || what == "+") {
                const std::optional<InputBinding> binding = parse_binding_text(value);
                if (!binding) {
                    return fail("unknown axis binding (key:<Key>, mouse:<button>, pad:<control>, touch:<gesture>)");
                }
                (what == "-" ? axis.negative : axis.positive).push_back(*binding);
            } else if (what == "stick") {
                InputAxisStick stick;
                stick.inverted = value.starts_with('-');
                if (!parse_pad_axis(stick.inverted ? std::string_view(value).substr(1) : std::string_view(value), stick.axis)) {
                    return fail("unknown pad axis");
                }
                axis.sticks.push_back(stick);
            } else if (what == "deadzone") {
                try {
                    axis.dead_zone = std::clamp(std::stof(value), 0.0f, 0.95f);
                } catch (const std::exception&) {
                    return fail("deadzone requires a number");
                }
            } else {
                return fail("unknown axis part");
            }
            map.set_axis(std::move(axis));
            continue;
        }
        return fail("unknown directive");
    }
    out = std::move(map);
    error.clear();
    return true;
}

std::string serialize_input_map(const InputMap& map) {
    std::ostringstream out;
    out << "# kin input map\n";
    for (const InputAction& action : map.actions()) {
        for (InputBinding binding : action.bindings) {
            if (binding.device == InputBindingDevice::Key) {
                out << "key " << action.name << ' ' << key_name(to_key(binding.code));
                write_modifiers(out, binding.modifiers);
                out << '\n';
            } else if (binding.device == InputBindingDevice::MouseButton) {
                out << "mouse " << action.name << ' ' << mouse_button_name(to_mouse_button(binding.code)) << '\n';
            } else if (binding.device == InputBindingDevice::Touch) {
                out << "touch " << action.name << ' ' << touch_control_name(static_cast<TouchControl>(binding.code)) << '\n';
            } else {
                out << "pad " << action.name << ' ' << pad_control_name(static_cast<PadControl>(binding.code)) << '\n';
            }
        }
    }
    for (const InputAxis& axis : map.axes()) {
        for (InputBinding binding : axis.negative) {
            out << "axis " << axis.name << " - " << binding_text(binding) << '\n';
        }
        for (InputBinding binding : axis.positive) {
            out << "axis " << axis.name << " + " << binding_text(binding) << '\n';
        }
        for (const InputAxisStick& stick : axis.sticks) {
            out << "axis " << axis.name << " stick " << (stick.inverted ? "-" : "") << pad_axis_name(stick.axis) << '\n';
        }
        out << "axis " << axis.name << " deadzone " << axis.dead_zone << '\n';
    }
    return out.str();
}

InputMap load_input_map(const std::filesystem::path& path) {
    const std::optional<std::string> text = read_content_text(path);
    if (!text) {
        KIN_LOG_ERROR_F("input",
                        "input map open failed",
                        (LogFields{{.name = "path", .value = path.string()},
                                   {.name = "error", .value = "open_failed"}}));
        throw std::runtime_error("Failed to open input map: " + path.string());
    }
    InputMap map;
    std::string error;
    if (!parse_input_map(*text, map, error)) {
        KIN_LOG_ERROR_F("input",
                        "input map parse failed",
                        (LogFields{{.name = "path", .value = path.string()},
                                   {.name = "error", .value = error}}));
        throw std::runtime_error(path.string() + ": " + error);
    }
    KIN_LOG_INFO_F("input",
                   "input map loaded",
                   (LogFields{{.name = "path", .value = path.string()},
                              {.name = "count", .value = std::to_string(map.action_count())},
                              {.name = "status", .value = "loaded"}}));
    return map;
}

bool save_input_map(const InputMap& map, const std::filesystem::path& path) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        KIN_LOG_ERROR_F("input",
                        "input map save failed",
                        (LogFields{{.name = "path", .value = path.string()},
                                   {.name = "error", .value = "open_failed"}}));
        return false;
    }
    out << serialize_input_map(map);
    const bool ok = static_cast<bool>(out);
    if (ok) {
        KIN_LOG_INFO_F("input",
                       "input map saved",
                       (LogFields{{.name = "path", .value = path.string()},
                                  {.name = "count", .value = std::to_string(map.action_count())},
                                  {.name = "status", .value = "saved"}}));
    } else {
        KIN_LOG_ERROR_F("input",
                        "input map save failed",
                        (LogFields{{.name = "path", .value = path.string()},
                                   {.name = "error", .value = "write_failed"}}));
    }
    return ok;
}

void Input::begin_frame() {
    begin_frame(true);
}

void Input::advance_step_edges() {
    // Consume only what update() reads (the STICKY keyboard edges via pressed(), and
    // the step wheel), so a fast frame running several fixed steps does not re-fire a
    // key or re-apply a wheel notch in later steps. The per-frame keyboard edges (read
    // by render() via frame_pressed(): text-field backspace/arrows/enter, etc.), the
    // frame wheel and all mouse/text edges are deliberately left intact so render-time
    // immediate UI sees this frame's input. (Clearing the frame edges here dropped
    // ~half of render-time keystrokes; clearing the mouse edges dropped ~half of
    // clicks.) Per-frame keyboard edges live exactly one rendered frame; per-frame
    // mouse edges live until the next stepping frame (see begin_frame) so update()-time
    // ui2 can observe a click from a 0-step frame.
    _keys.advance_step();
    for (Buttons<pad_control_count>& pad : _pads) {
        pad.advance_step();
    }
    advance_touch_step(false);
    _mouse_wheel_y = 0.0f;
    _mouse_wheel_x = 0.0f;
    _mouse_motion = {};
}

// What a step reads of touch, used up: the gestures' edges, fingers lifted,
// pinch and pan. At a frame's start (`frame_start`), gestures made before the
// frame before are let go, so a step sees each held, then released.
void Input::advance_touch_step(bool frame_start) {
    _touch_controls.advance_step();
    if (frame_start) {
        for (const std::size_t control : _touch_release_ready) {
            _touch_controls.set(control, false);
        }
        _touch_release_ready = std::exchange(_touch_release, {});
    }
    _touches_ended.clear();
    _pinch = 1.0f;
    _pan = {};
}

void Input::clear_touch_frame() {
    _touch_controls.clear_frame();
    _frame_touches_ended.clear();
    _frame_pinch = 1.0f;
    _frame_pan = {};
}

void Input::consume_frame_edges() {
    _keys.clear_frame();
    _key_frame_repeated.fill(false);
    for (Buttons<pad_control_count>& pad : _pads) {
        pad.clear_frame();
    }
    _mouse.clear_frame();
    clear_touch_frame();
    _frame_mouse_wheel_y = 0.0f;
    _frame_mouse_wheel_x = 0.0f;
    _frame_mouse_motion = {};
    _text_input.clear();
}

void Input::begin_frame(bool advance_transients) {
    if (advance_transients) {
        // A stepping frame ran at least one fixed update() last frame: do the full
        // reset, including the per-frame MOUSE edges. At sim rate every frame steps, so
        // this is the only path taken and behaviour is identical to a plain per-frame
        // clear.
        _keys.advance_step();
        for (Buttons<pad_control_count>& pad : _pads) {
            pad.advance_step();
        }
        _mouse.advance_step();
        _mouse.clear_frame();
        advance_touch_step(true);
        // App::run already used the step wheel up in that step (advance_step_edges);
        // this clears it for callers that step without it (run_for, tests).
        _mouse_wheel_y = 0.0f;
        _mouse_wheel_x = 0.0f;
        _mouse_motion = {};
    } else {
        // A 0-step frame (refresh > sim rate): the per-frame MOUSE edges PERSIST so UI
        // driven from update() can still read a click that landed here on the next
        // stepping frame — otherwise the edge would be wiped before any update() step
        // runs, which is exactly the "press the mouse N times before it registers" bug.
        // Press edges already CLAIMED by a widget this frame are the exception: clear
        // them so a claimed press does not re-fire (e.g. re-capturing a drag origin)
        // while it persists. Release edges are left to persist; their consumers are
        // guarded by active/drag state that clears once, so they do not re-fire.
        // The step wheel keeps adding up until a step reads it.
        for (i32 button = 0; button < MOUSE_BUTTON_COUNT; ++button) {
            if (_mouse_frame_pressed_consumed[button]) {
                _mouse.frame_pressed[button] = false;
            }
        }
    }
    _mouse_frame_pressed_consumed.fill(false);
    // Per-frame KEYBOARD edges are cleared on EVERY rendered frame (unconditionally),
    // so a key press's edge lives exactly one rendered frame. ui2 menu nav reads these
    // via frame_pressed in render(), which runs every frame; a one-frame lifetime means
    // one press == one nav move. (Do NOT persist these across 0-step frames like the
    // mouse edges above: keyboard nav is NOT idempotent — re-reading the edge ~once per
    // frame between fixed steps makes menus jump many entries per keypress at high FPS.
    // Keyboard read from update() must use the sticky pressed() API, which already
    // survives 0-step frames.)
    _keys.clear_frame();
    _key_frame_repeated.fill(false);
    for (Buttons<pad_control_count>& pad : _pads) {
        pad.clear_frame();
    }
    clear_touch_frame();
    _mouse_pos_prev = _mouse_pos;
    _frame_mouse_wheel_y = 0.0f;
    _frame_mouse_wheel_x = 0.0f;
    _frame_mouse_motion = {};
    _text_input.clear();
    apply_pointer_touch();
}

void Input::process_native_event(const void* native_event) {
    const auto& event = *static_cast<const SDL_Event*>(native_event);
    // The system's Back key (a phone's Back button or gesture, a keyboard's
    // Back) is Escape: what leaves a menu or pauses a game on a desktop.
    const auto scancode = [&]() -> i32 {
        return event.key.scancode == SDL_SCANCODE_AC_BACK ? SDL_SCANCODE_ESCAPE : event.key.scancode;
    };

    if (event.type == SDL_EVENT_KEY_DOWN && valid_scancode(scancode())) {
        const i32 key = scancode();
        if (!_keys.cur[key]) {
            _last_key_press_event_time_ns = event.key.timestamp;
            _last_key_press_detected_time_ns = SDL_GetTicksNS();
        } else if (event.key.repeat) {
            _key_frame_repeated[key] = true;
        }
        _keys.set(key, true);
        _keyboard_window = static_cast<WindowId>(event.key.windowID);
    } else if (event.type == SDL_EVENT_KEY_UP && valid_scancode(scancode())) {
        _keys.set(scancode(), false);
        _keyboard_window = static_cast<WindowId>(event.key.windowID);
    } else if (event.type == SDL_EVENT_FINGER_DOWN || event.type == SDL_EVENT_FINGER_MOTION ||
               event.type == SDL_EVENT_FINGER_UP || event.type == SDL_EVENT_FINGER_CANCELED) {
        // Where on the window: the system gives 0..1 of its size.
        Vec2f pos{event.tfinger.x, event.tfinger.y};
        if (SDL_Window* window = SDL_GetWindowFromID(event.tfinger.windowID)) {
            int w = 0;
            int h = 0;
            SDL_GetWindowSize(window, &w, &h);
            pos = {event.tfinger.x * static_cast<f32>(w), event.tfinger.y * static_cast<f32>(h)};
        }
        _touch_now = std::max(_touch_now, static_cast<f64>(event.tfinger.timestamp) * 1e-9);
        // One id for a finger whatever screen it is on.
        const u64 id = static_cast<u64>(event.tfinger.fingerID) ^ (static_cast<u64>(event.tfinger.touchID) << 32);
        if (event.type == SDL_EVENT_FINGER_DOWN) {
            touch_down(id, pos, static_cast<WindowId>(event.tfinger.windowID));
        } else if (event.type == SDL_EVENT_FINGER_MOTION) {
            touch_moved(id, pos);
        } else {
            touch_moved(id, pos);
            if (event.type == SDL_EVENT_FINGER_CANCELED) {
                claim_touch(id); // taken away: no tap, no click
            }
            touch_up(id);
        }
    } else if ((event.type == SDL_EVENT_MOUSE_MOTION && event.motion.which == SDL_TOUCH_MOUSEID) ||
               ((event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) &&
                event.button.which == SDL_TOUCH_MOUSEID) ||
               (event.type == SDL_EVENT_MOUSE_WHEEL && event.wheel.which == SDL_TOUCH_MOUSEID)) {
        // The mouse the system makes from a finger: the finger is read itself.
    } else if (event.type == SDL_EVENT_MOUSE_MOTION) {
        _pointer_is_touch = false;
        _last_mouse_motion_event_time_ns = event.motion.timestamp;
        _mouse_motion.x += event.motion.xrel;
        _mouse_motion.y += event.motion.yrel;
        _frame_mouse_motion.x += event.motion.xrel;
        _frame_mouse_motion.y += event.motion.yrel;
        _mouse_pos = {event.motion.x, event.motion.y};
        _mouse_window = static_cast<WindowId>(event.motion.windowID);
        _window_mouse_pos[_mouse_window] = _mouse_pos;
    } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && valid_mouse_button(event.button.button)) {
        _pointer_is_touch = false;
        _mouse.set(event.button.button, true);
        _drag_start[event.button.button] = {event.button.x, event.button.y};
        _mouse_pos = {event.button.x, event.button.y};
        _mouse_window = static_cast<WindowId>(event.button.windowID);
        _window_mouse_pos[_mouse_window] = _mouse_pos;
    } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && valid_mouse_button(event.button.button)) {
        _mouse.set(event.button.button, false);
        _mouse_pos = {event.button.x, event.button.y};
        _mouse_window = static_cast<WindowId>(event.button.windowID);
        _window_mouse_pos[_mouse_window] = _mouse_pos;
    } else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
        _mouse_wheel_y += event.wheel.y;
        _frame_mouse_wheel_y += event.wheel.y;
        _mouse_wheel_x += event.wheel.x;
        _frame_mouse_wheel_x += event.wheel.x;
        _mouse_window = static_cast<WindowId>(event.wheel.windowID);
    } else if (event.type == SDL_EVENT_TEXT_INPUT && event.text.text) {
        _text_input += event.text.text;
        _composition.clear(); // committed
        _composition_cursor = 0;
        _keyboard_window = static_cast<WindowId>(event.text.windowID);
    } else if (event.type == SDL_EVENT_TEXT_EDITING) {
        // SDL gives the cursor in characters; kept as bytes.
        set_text_composition(event.edit.text ? event.edit.text : "", event.edit.start);
        _keyboard_window = static_cast<WindowId>(event.edit.windowID);
    } else if (event.type == SDL_EVENT_DROP_BEGIN || event.type == SDL_EVENT_DROP_POSITION) {
        _drop_position = Vec2f{event.drop.x, event.drop.y};
    } else if (event.type == SDL_EVENT_DROP_FILE && event.drop.data) {
        _dropped_files.push_back({
            .path = std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(event.drop.data))),
            .pos = {event.drop.x, event.drop.y},
            .window = static_cast<WindowId>(event.drop.windowID),
        });
    } else if (event.type == SDL_EVENT_DROP_COMPLETE) {
        _drop_position.reset();
    }
}

std::vector<DroppedFile> Input::take_dropped_files() {
    return std::exchange(_dropped_files, {});
}

void Input::add_dropped_file(DroppedFile file) {
    _dropped_files.push_back(std::move(file));
}

bool Input::pressed(Key key) const {
    const SDL_Scancode scancode = to_sdl_scancode(key);
    return valid_scancode(scancode) && _keys.test(scancode, Edge::Pressed);
}

bool Input::held(Key key) const {
    const SDL_Scancode scancode = to_sdl_scancode(key);
    return valid_scancode(scancode) && _keys.cur[scancode];
}

bool Input::released(Key key) const {
    const SDL_Scancode scancode = to_sdl_scancode(key);
    return valid_scancode(scancode) && _keys.test(scancode, Edge::Released);
}

bool Input::frame_pressed(Key key) const {
    const SDL_Scancode scancode = to_sdl_scancode(key);
    return valid_scancode(scancode) && _keys.frame_pressed[scancode];
}

bool Input::frame_repeated(Key key) const {
    const SDL_Scancode scancode = to_sdl_scancode(key);
    return valid_scancode(scancode) && _key_frame_repeated[scancode];
}

bool Input::frame_released(Key key) const {
    const SDL_Scancode scancode = to_sdl_scancode(key);
    return valid_scancode(scancode) && _keys.frame_released[scancode];
}

bool Input::modifier_held(KeyModifiers modifiers) const {
    if (!any(modifiers)) {
        return true;
    }
    const bool shift = _keys.cur[SDL_SCANCODE_LSHIFT] || _keys.cur[SDL_SCANCODE_RSHIFT];
    const bool ctrl = _keys.cur[SDL_SCANCODE_LCTRL] || _keys.cur[SDL_SCANCODE_RCTRL];
    const bool alt = _keys.cur[SDL_SCANCODE_LALT] || _keys.cur[SDL_SCANCODE_RALT];
    if (has_modifier(modifiers, KeyModifiers::Shift) && !shift) {
        return false;
    }
    if (has_modifier(modifiers, KeyModifiers::Ctrl) && !ctrl) {
        return false;
    }
    if (has_modifier(modifiers, KeyModifiers::Alt) && !alt) {
        return false;
    }
    return true;
}

Vec2f Input::mouse_pos(WindowId window_id) const {
    const auto found = _window_mouse_pos.find(window_id);
    return found == _window_mouse_pos.end() ? Vec2f{} : found->second;
}

bool Input::mouse_pressed(MouseButton button) const {
    const u8 raw_button = to_sdl_mouse_button(button);
    return valid_mouse_button(raw_button) && _mouse.test(raw_button, Edge::Pressed);
}

bool Input::mouse_held(MouseButton button) const {
    const u8 raw_button = to_sdl_mouse_button(button);
    return valid_mouse_button(raw_button) && _mouse.cur[raw_button];
}

bool Input::mouse_released(MouseButton button) const {
    const u8 raw_button = to_sdl_mouse_button(button);
    return valid_mouse_button(raw_button) && _mouse.test(raw_button, Edge::Released);
}

bool Input::mouse_frame_pressed(MouseButton button) const {
    const u8 raw_button = to_sdl_mouse_button(button);
    return valid_mouse_button(raw_button) && _mouse.frame_pressed[raw_button];
}

bool Input::mouse_frame_released(MouseButton button) const {
    const u8 raw_button = to_sdl_mouse_button(button);
    return valid_mouse_button(raw_button) && _mouse.frame_released[raw_button];
}

void Input::consume_mouse_frame_pressed(MouseButton button) {
    const u8 raw_button = to_sdl_mouse_button(button);
    if (valid_mouse_button(raw_button)) {
        // Leave the edge readable for the rest of this frame; flag it so the next
        // begin_frame clears it even on a 0-step frame instead of letting it persist.
        _mouse_frame_pressed_consumed[raw_button] = true;
    }
}

bool Input::mouse_dragging(MouseButton button) const {
    const u8 raw_button = to_sdl_mouse_button(button);
    if (!valid_mouse_button(raw_button) || !_mouse.cur[raw_button]) {
        return false;
    }

    return length_squared(drag_delta(button)) > DRAG_THRESHOLD * DRAG_THRESHOLD;
}

Vec2f Input::drag_delta(MouseButton button) const {
    const u8 raw_button = to_sdl_mouse_button(button);
    if (!valid_mouse_button(raw_button)) {
        return {};
    }

    return {
        _mouse_pos.x - _drag_start[raw_button].x,
        _mouse_pos.y - _drag_start[raw_button].y,
    };
}

void Input::set_mouse_motion(Vec2f delta) {
    _mouse_motion.x += delta.x;
    _mouse_motion.y += delta.y;
    _frame_mouse_motion.x += delta.x;
    _frame_mouse_motion.y += delta.y;
}

void Input::set_mouse_wheel_y(f32 value) {
    _mouse_wheel_y += value;
    _frame_mouse_wheel_y += value;
}

void Input::set_mouse_wheel_x(f32 value) {
    _mouse_wheel_x += value;
    _frame_mouse_wheel_x += value;
}

void Input::set_mouse_pos(Vec2f pos, WindowId window_id) {
    _mouse_pos = pos;
    _mouse_window = window_id;
    _window_mouse_pos[window_id] = pos;
}

void Input::set_mouse_held(MouseButton button, bool held) {
    const u8 raw_button = to_sdl_mouse_button(button);
    if (!valid_mouse_button(raw_button)) {
        return;
    }

    _mouse.set(raw_button, held);
    if (held) {
        _drag_start[raw_button] = _mouse_pos;
    }
}

void Input::set_mouse_pressed(MouseButton button) {
    const u8 raw_button = to_sdl_mouse_button(button);
    if (!valid_mouse_button(raw_button)) {
        return;
    }

    _mouse.press(raw_button);
    _drag_start[raw_button] = _mouse_pos;
}

void Input::set_text_composition(std::string_view text, i32 cursor) {
    _composition = text;
    if (cursor < 0) {
        _composition_cursor = static_cast<i32>(_composition.size());
        return;
    }
    std::size_t at = 0;
    for (i32 i = 0; i < cursor && at < _composition.size(); ++i) {
        at = utf8_next(_composition, at);
    }
    _composition_cursor = static_cast<i32>(at);
}

void Input::set_text_input(std::string_view text) {
    _text_input = text;
}

void Input::set_key_pressed(Key key) {
    const SDL_Scancode scancode = to_sdl_scancode(key);
    if (!valid_scancode(scancode)) {
        return;
    }
    _keys.press(scancode);
}

void Input::set_key_released(Key key) {
    const SDL_Scancode scancode = to_sdl_scancode(key);
    if (!valid_scancode(scancode) || !_keys.cur[scancode]) {
        return;
    }
    _keys.set(scancode, false);
}

void Input::set_key_repeated(Key key) {
    const SDL_Scancode scancode = to_sdl_scancode(key);
    if (!valid_scancode(scancode) || !_keys.cur[scancode]) {
        return;
    }
    _key_frame_repeated[scancode] = true;
}

void Input::set_modifier_held(KeyModifiers modifiers, bool held) {
    if (has_modifier(modifiers, KeyModifiers::Shift)) {
        _keys.cur[SDL_SCANCODE_LSHIFT] = held;
    }
    if (has_modifier(modifiers, KeyModifiers::Ctrl)) {
        _keys.cur[SDL_SCANCODE_LCTRL] = held;
    }
    if (has_modifier(modifiers, KeyModifiers::Alt)) {
        _keys.cur[SDL_SCANCODE_LALT] = held;
    }
}

void Input::set_clipboard_text(std::string_view text) {
    _clipboard_text = std::string{text};
    SDL_SetClipboardText(_clipboard_text.c_str());
}

std::string Input::clipboard_text() const {
    char* text = SDL_GetClipboardText();
    if (!text) {
        return _clipboard_text;
    }
    std::string result{text};
    SDL_free(text);
    return result.empty() ? _clipboard_text : result;
}

void InputMap::bind(std::string_view action, Key key) {
    _bindings[std::string(action)].push_back(key_binding(key));
    KIN_LOG_DEBUG_F("input",
                    "input binding added",
                    (LogFields{{.name = "action", .value = std::string{action}},
                               {.name = "device", .value = "key"},
                               {.name = "binding", .value = std::string{key_name(key)}}}));
}

void InputMap::bind(std::string_view action, Key key, KeyModifiers modifiers) {
    _bindings[std::string(action)].push_back(key_binding(key, modifiers));
    KIN_LOG_DEBUG_F("input",
                    "input binding added",
                    (LogFields{{.name = "action", .value = std::string{action}},
                               {.name = "device", .value = "key"},
                               {.name = "binding", .value = std::string{key_name(key)}},
                               {.name = "modifiers", .value = std::to_string(static_cast<int>(modifiers))}}));
}

void InputMap::bind(std::string_view action, std::initializer_list<Key> keys) {
    auto& bound = _bindings[std::string(action)];
    for (Key key : keys) {
        bound.push_back(key_binding(key));
    }
    KIN_LOG_DEBUG_F("input",
                    "input bindings added",
                    (LogFields{{.name = "action", .value = std::string{action}},
                               {.name = "device", .value = "key"},
                               {.name = "count", .value = std::to_string(keys.size())}}));
}

void InputMap::bind(std::string_view action, MouseButton button) {
    _bindings[std::string(action)].push_back(mouse_binding(button));
    KIN_LOG_DEBUG_F("input",
                    "input binding added",
                    (LogFields{{.name = "action", .value = std::string{action}},
                               {.name = "device", .value = "mouse"},
                               {.name = "binding", .value = std::string{mouse_button_name(button)}}}));
}

void InputMap::bind(std::string_view action, TouchControl control) {
    _bindings[std::string(action)].push_back({.device = InputBindingDevice::Touch, .code = static_cast<i32>(control)});
}

void InputMap::bind(std::string_view action, PadControl control) {
    _bindings[std::string(action)].push_back({.device = InputBindingDevice::Gamepad, .code = static_cast<i32>(control)});
}

void InputMap::set_bindings(std::string_view action, std::vector<InputBinding> bindings) {
    if (bindings.empty()) {
        _bindings.erase(std::string(action));
    } else {
        _bindings[std::string(action)] = std::move(bindings);
    }
}

void InputMap::set_axis(InputAxis axis) {
    std::string name = axis.name;
    _axes[std::move(name)] = std::move(axis);
}

bool InputMap::remove_axis(std::string_view name) {
    return _axes.erase(std::string(name)) > 0;
}

const InputAxis* InputMap::axis(std::string_view name) const {
    const auto found = _axes.find(std::string(name));
    return found == _axes.end() ? nullptr : &found->second;
}

std::vector<InputAxis> InputMap::axes() const {
    std::vector<InputAxis> result;
    result.reserve(_axes.size());
    for (const auto& [name, axis] : _axes) {
        result.push_back(axis);
    }
    std::ranges::sort(result, {}, &InputAxis::name);
    return result;
}

void InputMap::unbind(std::string_view action) {
    const std::size_t erased = _bindings.erase(std::string(action));
    KIN_LOG_DEBUG_F("input",
                    "input bindings removed",
                    (LogFields{{.name = "action", .value = std::string{action}},
                               {.name = "count", .value = std::to_string(erased)}}));
}

void InputMap::clear() {
    const std::size_t count = _bindings.size();
    _bindings.clear();
    _axes.clear();
    KIN_LOG_DEBUG_F("input",
                    "input bindings cleared",
                    (LogFields{{.name = "count", .value = std::to_string(count)}}));
}

std::vector<InputAction> InputMap::actions() const {
    std::vector<InputAction> result;
    result.reserve(_bindings.size());

    for (const auto& [name, bindings] : _bindings) {
        result.push_back({
            .name = name,
            .bindings = bindings,
        });
    }

    std::ranges::sort(result, {}, &InputAction::name);
    return result;
}

const std::vector<InputBinding>* InputMap::bindings(std::string_view action) const {
    const auto found = _bindings.find(std::string(action));
    return found == _bindings.end() ? nullptr : &found->second;
}

InputActionContext::InputActionContext(std::string_view name)
    : _name(name) {
}

void InputActionContext::clear() {
    const std::size_t action_count = _actions.size();
    const std::size_t child_count = _children.size();
    _actions.clear();
    _children.clear();
    KIN_LOG_DEBUG_F("input",
                    "input action context cleared",
                    (LogFields{{.name = "context", .value = _name},
                               {.name = "count", .value = std::to_string(action_count)},
                               {.name = "children", .value = std::to_string(child_count)}}));
}

void InputActionContext::add(std::string_view action, std::string_view label, std::string_view description) {
    const std::string action_name{action};
    _actions.push_back({
        .name = action_name,
        .label = label.empty() ? action_name : std::string(label),
        .description = std::string(description),
    });
    KIN_LOG_DEBUG_F("input",
                    "input action hint added",
                    (LogFields{{.name = "context", .value = _name}, {.name = "action", .value = action_name}}));
}

void InputActionContext::include(const InputActionContext& context) {
    const std::string child_name = context.name();
    _children.push_back(context);
    KIN_LOG_DEBUG_F("input",
                    "input action context included",
                    (LogFields{{.name = "context", .value = _name}, {.name = "child", .value = child_name}}));
}

namespace {

void append_flattened_actions(const InputActionContext& context,
                              std::vector<InputActionHint>& result,
                              std::unordered_map<std::string, std::size_t>& index_by_name) {
    for (const InputActionHint& action : context.actions()) {
        const auto found = index_by_name.find(action.name);
        if (found == index_by_name.end()) {
            index_by_name[action.name] = result.size();
            result.push_back(action);
        } else {
            result[found->second] = action;
        }
    }

    for (const InputActionContext& child : context.children()) {
        append_flattened_actions(child, result, index_by_name);
    }
}

} // namespace

std::vector<InputActionHint> InputActionContext::flattened_actions() const {
    std::vector<InputActionHint> result;
    std::unordered_map<std::string, std::size_t> index_by_name;
    append_flattened_actions(*this, result, index_by_name);
    return result;
}

std::vector<AvailableInputAction> InputActionContext::resolve(const InputMap& map) const {
    std::vector<AvailableInputAction> result;
    const std::vector<InputActionHint> actions = flattened_actions();
    result.reserve(actions.size());

    for (const InputActionHint& action : actions) {
        std::vector<InputBinding> bindings;
        if (const std::vector<InputBinding>* found = map.bindings(action.name)) {
            bindings = *found;
        }

        result.push_back({
            .name = action.name,
            .label = action.label,
            .description = action.description,
            .bindings = std::move(bindings),
        });
    }

    KIN_LOG_DEBUG_F("input",
                    "input action context resolved",
                    (LogFields{{.name = "context", .value = _name},
                               {.name = "count", .value = std::to_string(result.size())}}));
    return result;
}

void Input::bind(std::string_view action, Key key) {
    _map.bind(action, key);
}

void Input::bind(std::string_view action, Key key, KeyModifiers modifiers) {
    _map.bind(action, key, modifiers);
}

void Input::bind(std::string_view action, std::initializer_list<Key> keys) {
    _map.bind(action, keys);
}

void Input::bind(std::string_view action, MouseButton button) {
    _map.bind(action, button);
}

void Input::unbind(std::string_view action) {
    _map.unbind(action);
}

void Input::clear_bindings() {
    _map.clear();
}

void Input::set_map(const InputMap& map) {
    _map = map;
}

void Input::set_action_held(std::string_view action, bool is_held) {
    const std::vector<InputBinding>* bindings = _map.bindings(action);
    if (!bindings) {
        return;
    }

    for (InputBinding binding : *bindings) {
        const SDL_Scancode scancode = binding_scancode(binding);
        if (binding.device == InputBindingDevice::Key && valid_scancode(scancode)) {
            set_modifier_held(binding.modifiers, is_held);
            _keys.set(scancode, is_held);
        }
    }
}

void Input::set_action_pressed(std::string_view action) {
    const std::vector<InputBinding>* bindings = _map.bindings(action);
    if (!bindings) {
        return;
    }

    for (InputBinding binding : *bindings) {
        const SDL_Scancode scancode = binding_scancode(binding);
        if (binding.device != InputBindingDevice::Key || !valid_scancode(scancode)) {
            continue;
        }

        set_modifier_held(binding.modifiers, true);
        _keys.press(scancode);
    }
}

Input::Reach Input::everything() const {
    return {.keyboard = true, .gamepads = (1u << pad_count) - 1u, .map = &_map};
}

// A player's devices and map: their own controller; the keyboard as
// set_players says (with none set, player 0 has it, and, while no player has
// a controller of their own, set_gamepad's: one player who never joined).
Input::Reach Input::reach_of_player(i32 player) const {
    if (player < 0) {
        return everything();
    }
    if (player >= max_input_players) {
        return {.map = &_map};
    }
    const u32 own = 1u << (player + 1);
    if (_players.empty()) {
        if (player != 0) {
            return {.gamepads = own, .map = &_map};
        }
        const bool joined = std::any_of(_pad_states.begin() + 1, _pad_states.end(), [](const GamepadState& pad) { return pad.connected; });
        return {.keyboard = true, .gamepads = own | (joined ? 0u : 1u), .map = &_map};
    }
    if (player >= static_cast<i32>(_players.size())) {
        return {.gamepads = own, .map = &_map};
    }
    const InputPlayer& p = _players[static_cast<std::size_t>(player)];
    return {.keyboard = p.keyboard, .gamepads = own, .map = p.map ? &*p.map : &_map};
}

bool Input::binding_edge(const Reach& reach, InputBinding binding, Edge edge) const {
    switch (binding.device) {
    case InputBindingDevice::Key: {
        const SDL_Scancode scancode = binding_scancode(binding);
        return reach.keyboard && valid_scancode(scancode) && modifier_held(binding.modifiers) && _keys.test(scancode, edge);
    }
    case InputBindingDevice::MouseButton: {
        const u8 button = binding_mouse_button(binding);
        return reach.keyboard && valid_mouse_button(button) && _mouse.test(button, edge);
    }
    case InputBindingDevice::Touch: {
        const auto control = static_cast<std::size_t>(binding.code);
        return reach.keyboard && binding.code >= 0 && control < touch_control_count && _touch_controls.test(control, edge);
    }
    case InputBindingDevice::Gamepad: {
        const std::size_t control = pad_index(binding);
        if (control >= pad_control_count) {
            return false;
        }
        for (std::size_t at = 0; at < pad_count; ++at) {
            if (((reach.gamepads >> at) & 1u) != 0 && _pads[at].test(control, edge)) {
                return true;
            }
        }
        return false;
    }
    }
    return false;
}

bool Input::map_edge(const Reach& reach, std::string_view action, Edge edge) const {
    const std::vector<InputBinding>* bindings = reach.map->bindings(action);
    return bindings && std::ranges::any_of(*bindings, [&](InputBinding binding) { return binding_edge(reach, binding, edge); });
}

bool Input::action_edge(i32 player, std::string_view action, Edge edge) const {
    if (player >= 0) {
        return map_edge(reach_of_player(player), action, edge);
    }
    if (map_edge(everything(), action, edge)) {
        return true;
    }
    // Players with maps of their own bind actions the Input's map may not have.
    for (std::size_t p = 0; p < _players.size(); ++p) {
        if (_players[p].map && map_edge(reach_of_player(static_cast<i32>(p)), action, edge)) {
            return true;
        }
    }
    return false;
}

bool Input::pressed(std::string_view action) const {
    return action_edge(-1, action, Edge::Pressed);
}

bool Input::held(std::string_view action) const {
    return action_edge(-1, action, Edge::Held);
}

bool Input::released(std::string_view action) const {
    return action_edge(-1, action, Edge::Released);
}

bool Input::frame_pressed(std::string_view action) const {
    return action_edge(-1, action, Edge::FramePressed);
}

bool Input::frame_released(std::string_view action) const {
    return action_edge(-1, action, Edge::FrameReleased);
}

bool Input::pressed(i32 player, std::string_view action) const {
    return action_edge(player, action, Edge::Pressed);
}

bool Input::held(i32 player, std::string_view action) const {
    return action_edge(player, action, Edge::Held);
}

bool Input::released(i32 player, std::string_view action) const {
    return action_edge(player, action, Edge::Released);
}

bool Input::frame_pressed(i32 player, std::string_view action) const {
    return action_edge(player, action, Edge::FramePressed);
}

bool Input::frame_released(i32 player, std::string_view action) const {
    return action_edge(player, action, Edge::FrameReleased);
}

bool pad_control_held(const GamepadState& pad, PadControl control) {
    if (!pad.connected) {
        return false;
    }
    constexpr f32 half = 0.5f;
    const auto code = static_cast<u8>(control);
    if (code < static_cast<u8>(GamepadButton::Count)) {
        return pad.button(static_cast<GamepadButton>(code));
    }
    switch (control) {
    case PadControl::LeftStickLeft: return pad.left_x < -half;
    case PadControl::LeftStickRight: return pad.left_x > half;
    case PadControl::LeftStickUp: return pad.left_y > half;
    case PadControl::LeftStickDown: return pad.left_y < -half;
    case PadControl::RightStickLeft: return pad.right_x < -half;
    case PadControl::RightStickRight: return pad.right_x > half;
    case PadControl::RightStickUp: return pad.right_y > half;
    case PadControl::RightStickDown: return pad.right_y < -half;
    case PadControl::LeftTrigger: return pad.left_trigger > half;
    case PadControl::RightTrigger: return pad.right_trigger > half;
    default: return false;
    }
}

bool binding_pressed(const Input& input, InputBinding binding, i32 player) {
    return input.binding_edge(input.reach_of_player(player), binding, Input::Edge::Pressed);
}

f32 pad_axis_raw(const GamepadState& pad, PadAxis axis) {
    switch (axis) {
    case PadAxis::LeftX: return pad.left_x;
    case PadAxis::LeftY: return pad.left_y;
    case PadAxis::RightX: return pad.right_x;
    case PadAxis::RightY: return pad.right_y;
    case PadAxis::LeftTrigger: return pad.left_trigger;
    case PadAxis::RightTrigger: return pad.right_trigger;
    }
    return 0.0f;
}

bool binding_held(const Input& input, InputBinding binding, i32 player) {
    return input.binding_edge(input.reach_of_player(player), binding, Input::Edge::Held);
}

f32 Input::reach_axis(const Reach& reach, const InputAxis& axis) const {
    const auto held_any = [&](const std::vector<InputBinding>& bindings) {
        return std::ranges::any_of(bindings, [&](InputBinding binding) { return binding_edge(reach, binding, Edge::Held); });
    };
    f32 value = (held_any(axis.positive) ? 1.0f : 0.0f) - (held_any(axis.negative) ? 1.0f : 0.0f);
    // A stick pushed further wins (on any of the controllers reached); the
    // dead zone taken off and the rest stretched to the end.
    const f32 dead = std::clamp(axis.dead_zone, 0.0f, 0.95f);
    for (std::size_t at = 0; at < pad_count; ++at) {
        const GamepadState& pad = _pad_states[at];
        if (((reach.gamepads >> at) & 1u) == 0 || !pad.connected) {
            continue;
        }
        for (const InputAxisStick& stick : axis.sticks) {
            f32 raw = pad_axis_raw(pad, stick.axis);
            if (stick.inverted) {
                raw = -raw;
            }
            const f32 past = std::abs(raw) <= dead ? 0.0f : std::copysign((std::abs(raw) - dead) / (1.0f - dead), raw);
            if (std::abs(past) > std::abs(value)) {
                value = past;
            }
        }
    }
    return std::clamp(value, -1.0f, 1.0f);
}

f32 axis_value(const Input& input, const InputAxis& axis, i32 player) {
    return input.reach_axis(input.reach_of_player(player), axis);
}

f32 Input::player_axis(i32 player, std::string_view name) const {
    if (player >= 0) {
        const Reach reach = reach_of_player(player);
        const InputAxis* axis = reach.map->axis(name);
        return axis ? reach_axis(reach, *axis) : 0.0f;
    }
    const auto furthest = [](f32 a, f32 b) { return std::abs(b) > std::abs(a) ? b : a; };
    f32 value = 0.0f;
    if (const InputAxis* axis = _map.axis(name)) {
        value = reach_axis(everything(), *axis);
    }
    for (std::size_t p = 0; p < _players.size(); ++p) {
        if (_players[p].map) {
            if (const InputAxis* axis = _players[p].map->axis(name)) {
                value = furthest(value, reach_axis(reach_of_player(static_cast<i32>(p)), *axis));
            }
        }
    }
    return value;
}

f32 Input::axis(std::string_view name) const {
    return player_axis(-1, name);
}

f32 Input::axis(i32 player, std::string_view name) const {
    return player_axis(player, name);
}

void Input::set_pad(std::size_t at, const GamepadState& state) {
    _pad_states[at] = state;
    for (std::size_t control = 0; control < pad_control_count; ++control) {
        _pads[at].set(control, pad_control_held(state, static_cast<PadControl>(control)));
    }
}

void Input::set_gamepad(const GamepadState& state) {
    set_pad(0, state);
}

void Input::set_player_gamepad(i32 player, const GamepadState& state) {
    if (player >= 0 && player < max_input_players) {
        set_pad(static_cast<std::size_t>(player) + 1, state);
    }
}

const GamepadState& Input::player_gamepad(i32 player) const {
    static const GamepadState none{};
    return player >= 0 && player < max_input_players ? _pad_states[static_cast<std::size_t>(player) + 1] : none;
}

bool Input::pad_frame_pressed(PadControl control) const {
    const auto index = static_cast<std::size_t>(control);
    return index < pad_control_count && _pads[0].frame_pressed[index];
}

bool Input::pad_frame_pressed(PadControl control, i32 player) const {
    const auto index = static_cast<std::size_t>(control);
    if (index >= pad_control_count) {
        return false;
    }
    if (player < 0) {
        return std::ranges::any_of(_pads, [index](const Buttons<pad_control_count>& pad) { return pad.frame_pressed[index]; });
    }
    return player < max_input_players && _pads[static_cast<std::size_t>(player) + 1].frame_pressed[index];
}

// --- touch ---------------------------------------------------------------------------

namespace {
// How gestures are told apart (window pixels, seconds).
constexpr f32 touch_slop = 12.0f;           // moving less is holding still
constexpr f64 tap_seconds = 0.3;
constexpr f64 double_tap_seconds = 0.35;    // between two taps
constexpr f32 double_tap_distance = 40.0f;
constexpr f64 long_press_seconds = 0.5;
constexpr f64 swipe_seconds = 0.5;
constexpr f32 swipe_distance = 60.0f;

f32 distance(Vec2f a, Vec2f b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y));
}

// The two unclaimed fingers when exactly two are down (for pinch and pan).
std::optional<std::pair<const TouchPoint*, const TouchPoint*>> two_fingers(const std::vector<TouchPoint>& touches) {
    const TouchPoint* a = nullptr;
    const TouchPoint* b = nullptr;
    for (const TouchPoint& t : touches) {
        if (t.claimed) continue;
        if (!a) a = &t;
        else if (!b) b = &t;
        else return std::nullopt;
    }
    if (!a || !b) return std::nullopt;
    return std::pair{a, b};
}
} // namespace

const TouchPoint* Input::touch(u64 id) const {
    const auto found = std::ranges::find(_touches, id, &TouchPoint::id);
    return found != _touches.end() ? &*found : nullptr;
}

bool Input::touch_pressed(TouchControl control) const {
    const auto index = static_cast<std::size_t>(control);
    return index < touch_control_count && _touch_controls.test(index, Edge::Pressed);
}

bool Input::touch_frame_pressed(TouchControl control) const {
    const auto index = static_cast<std::size_t>(control);
    return index < touch_control_count && _touch_controls.frame_pressed[index];
}

void Input::make_gesture(TouchControl control) {
    const auto index = static_cast<std::size_t>(control);
    _touch_controls.press(index);
    if (std::ranges::find(_touch_release, index) == _touch_release.end()) {
        _touch_release.push_back(index);
    }
    std::erase(_touch_release_ready, index); // made again: held on
}

void Input::set_touch_down(u64 id, Vec2f pos, WindowId window) {
    touch_down(id, pos, window);
}

void Input::set_touch_moved(u64 id, Vec2f pos) {
    touch_moved(id, pos);
}

void Input::set_touch_up(u64 id) {
    touch_up(id);
}

void Input::touch_down(u64 id, Vec2f pos, WindowId window) {
    if (touch(id)) {
        touch_up(id); // never saw it lifted
    }
    _touches.push_back({.id = id, .pos = pos, .start = pos, .window = window, .down_at = _touch_now});
    _strokes.push_back({.id = id, .down_at = _touch_now, .start = pos});
    const auto unclaimed = std::ranges::count_if(_touches, [](const TouchPoint& t) { return !t.claimed; });
    if (unclaimed >= 2) {
        for (TouchStroke& stroke : _strokes) {
            stroke.multi = true; // no tap or swipe of one finger now
        }
        _two_down_at = unclaimed == 2 ? _touch_now : -1.0;
        _two_moved = false;
        // A second finger: a pinch, not a press on what the first is over.
        if (_pointer_touch) {
            cancel_pointer_press();
        }
        return;
    }
    if (!_pointer_touch && unclaimed == 1) { // the only finger no control has
        // The pointer goes there now and presses a frame later, so UI sees it
        // over what it touches first, as a mouse is before it clicks.
        _pointer_touch = id;
        _pointer_is_touch = true;
        set_mouse_pos(pos, window);
        _pointer_press_pending = true;
    }
}

void Input::touch_moved(u64 id, Vec2f pos) {
    const auto found = std::ranges::find(_touches, id, &TouchPoint::id);
    if (found == _touches.end()) {
        return;
    }
    const Vec2f was = found->pos;
    // Two fingers: how their spread and middle change.
    if (const auto two = two_fingers(_touches); two && !found->claimed) {
        const TouchPoint& other = two->first->id == id ? *two->second : *two->first;
        const f32 before = distance(was, other.pos);
        const f32 after = distance(pos, other.pos);
        if (before > 1.0f && after > 1.0f) {
            _pinch *= after / before;
            _frame_pinch *= after / before;
        }
        const Vec2f moved{(pos.x - was.x) * 0.5f, (pos.y - was.y) * 0.5f};
        _pan = {_pan.x + moved.x, _pan.y + moved.y};
        _frame_pan = {_frame_pan.x + moved.x, _frame_pan.y + moved.y};
    }
    found->pos = pos;
    if (const auto stroke = std::ranges::find(_strokes, id, &TouchStroke::id); stroke != _strokes.end()) {
        stroke->travel = std::max(stroke->travel, distance(pos, stroke->start));
        if (stroke->multi && stroke->travel > touch_slop) {
            _two_moved = true;
        }
    }
    if (_pointer_touch == id) {
        set_mouse_motion({pos.x - was.x, pos.y - was.y});
        set_mouse_pos(pos, found->window);
    }
}

void Input::touch_up(u64 id) {
    const auto found = std::ranges::find(_touches, id, &TouchPoint::id);
    if (found == _touches.end()) {
        return;
    }
    const TouchPoint point = *found;
    _touches.erase(found);
    _touches_ended.push_back(point);
    _frame_touches_ended.push_back(point);
    if (_pointer_touch == id) {
        if (_pointer_press_pending) {
            _pointer_release_pending = true; // a tap within a frame: pressed next frame, let go the one after
        } else {
            _mouse.set(to_sdl_mouse_button(MouseButton::Left), false); // the click, if it was one
        }
        _pointer_touch.reset();
    }
    const auto stroke = std::ranges::find(_strokes, id, &TouchStroke::id);
    if (stroke == _strokes.end()) {
        return; // claimed
    }
    const TouchStroke s = *stroke;
    _strokes.erase(stroke);
    const f64 held = _touch_now - s.down_at;
    if (!s.multi && !s.long_pressed) {
        const Vec2f d{point.pos.x - s.start.x, point.pos.y - s.start.y};
        if (held <= tap_seconds && s.travel <= touch_slop) {
            make_gesture(TouchControl::Tap);
            if (_last_tap && _touch_now - _last_tap->first <= double_tap_seconds &&
                distance(_last_tap->second, point.pos) <= double_tap_distance) {
                make_gesture(TouchControl::DoubleTap);
                _last_tap.reset();
            } else {
                _last_tap = std::pair{_touch_now, point.pos};
            }
        } else if (held <= swipe_seconds && std::max(std::abs(d.x), std::abs(d.y)) >= swipe_distance) {
            if (std::abs(d.x) >= 2.0f * std::abs(d.y)) {
                make_gesture(d.x < 0 ? TouchControl::SwipeLeft : TouchControl::SwipeRight);
            } else if (std::abs(d.y) >= 2.0f * std::abs(d.x)) {
                make_gesture(d.y < 0 ? TouchControl::SwipeUp : TouchControl::SwipeDown);
            }
        }
    }
    // Both of two fingers up, soon and still: a two-finger tap.
    if (_strokes.empty()) {
        if (_two_down_at >= 0.0 && !_two_moved && _touch_now - _two_down_at <= tap_seconds) {
            make_gesture(TouchControl::TwoFingerTap);
        }
        _two_down_at = -1.0;
        _two_moved = false;
    }
}

void Input::cancel_pointer_press() {
    if (_pointer_press_pending) {
        _pointer_press_pending = _pointer_release_pending = false;
    } else {
        const u8 left = to_sdl_mouse_button(MouseButton::Left);
        _mouse.cur[left] = _mouse.prev[left] = _mouse.pressed[left] = _mouse.frame_pressed[left] = false;
    }
    _pointer_touch.reset();
}

// At a frame's start: the pointer's press a frame after its finger came, and
// a quick tap's release the frame after that.
void Input::apply_pointer_touch() {
    const u8 left = to_sdl_mouse_button(MouseButton::Left);
    if (_pointer_release_pending && !_pointer_press_pending) {
        _mouse.set(left, false);
        _pointer_release_pending = false;
    }
    if (_pointer_press_pending) {
        _mouse.set(left, true);
        _drag_start[left] = _mouse_pos;
        _pointer_press_pending = false;
    }
}

void Input::claim_touch(u64 id) {
    const auto found = std::ranges::find(_touches, id, &TouchPoint::id);
    if (found == _touches.end() || found->claimed) {
        return;
    }
    found->claimed = true;
    std::erase_if(_strokes, [id](const TouchStroke& s) { return s.id == id; });
    if (_pointer_touch == id) {
        cancel_pointer_press(); // let go of what it pressed without clicking it
    }
}

void Input::update_touch(f64 now) {
    _touch_now = std::max(_touch_now, now);
    for (TouchStroke& stroke : _strokes) {
        if (!stroke.multi && !stroke.long_pressed && stroke.travel <= touch_slop && _touch_now - stroke.down_at >= long_press_seconds) {
            stroke.long_pressed = true;
            make_gesture(TouchControl::LongPress);
        }
    }
}

} // namespace kin
