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
    return mouse_button_name(to_mouse_button(binding.code));
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
                    return fail("unknown axis binding (key:<Key>, mouse:<button>, pad:<control>)");
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
    _pad_plugged.fill(false);
    _pad_unplugged.fill(false);
    _mouse_wheel_y = 0.0f;
    _mouse_wheel_x = 0.0f;
    _mouse_motion = {};
}

void Input::consume_frame_edges() {
    _keys.clear_frame();
    _key_frame_repeated.fill(false);
    for (Buttons<pad_control_count>& pad : _pads) {
        pad.clear_frame();
    }
    _mouse.clear_frame();
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
        _pad_plugged.fill(false);
        _pad_unplugged.fill(false);
        _mouse.advance_step();
        _mouse.clear_frame();
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
    _mouse_pos_prev = _mouse_pos;
    _frame_mouse_wheel_y = 0.0f;
    _frame_mouse_wheel_x = 0.0f;
    _frame_mouse_motion = {};
    _text_input.clear();
}

void Input::process_native_event(const void* native_event) {
    const auto& event = *static_cast<const SDL_Event*>(native_event);

    if (event.type == SDL_EVENT_KEY_DOWN && valid_scancode(event.key.scancode)) {
        if (!_keys.cur[event.key.scancode]) {
            _last_key_press_event_time_ns = event.key.timestamp;
            _last_key_press_detected_time_ns = SDL_GetTicksNS();
            note_keyboard();
        } else if (event.key.repeat) {
            _key_frame_repeated[event.key.scancode] = true;
        }
        _keys.set(event.key.scancode, true);
        _keyboard_window = static_cast<WindowId>(event.key.windowID);
    } else if (event.type == SDL_EVENT_KEY_UP && valid_scancode(event.key.scancode)) {
        _keys.set(event.key.scancode, false);
        _keyboard_window = static_cast<WindowId>(event.key.windowID);
    } else if (event.type == SDL_EVENT_MOUSE_MOTION) {
        _last_mouse_motion_event_time_ns = event.motion.timestamp;
        _mouse_motion.x += event.motion.xrel;
        _mouse_motion.y += event.motion.yrel;
        _frame_mouse_motion.x += event.motion.xrel;
        _frame_mouse_motion.y += event.motion.yrel;
        _mouse_pos = {event.motion.x, event.motion.y};
        _mouse_window = static_cast<WindowId>(event.motion.windowID);
        _window_mouse_pos[_mouse_window] = _mouse_pos;
    } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && valid_mouse_button(event.button.button)) {
        if (!_mouse.cur[event.button.button]) {
            note_mouse();
        }
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

    if (held && !_mouse.cur[raw_button]) {
        note_mouse();
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

    note_mouse();
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
    note_keyboard();
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
    return {.keyboard = true, .gamepads = (1u << max_gamepads) - 1u, .map = &_map};
}

Input::Reach Input::reach_of(const InputPlayer& player) const {
    Reach reach{.keyboard = player.keyboard, .map = player.map ? &*player.map : &_map};
    for (const i32 slot : player.gamepads) {
        if (slot >= 0 && slot < max_gamepads) {
            reach.gamepads |= 1u << slot;
        }
    }
    return reach;
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
    case InputBindingDevice::Gamepad: {
        const std::size_t control = pad_index(binding);
        if (control >= pad_control_count) {
            return false;
        }
        for (i32 slot = 0; slot < max_gamepads; ++slot) {
            if (((reach.gamepads >> slot) & 1u) != 0 && _pads[static_cast<std::size_t>(slot)].test(control, edge)) {
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
    if (player < 0 || (_players.empty() && player == 0)) {
        if (map_edge(everything(), action, edge)) {
            return true;
        }
        // Players with maps of their own bind actions the Input's map may not have.
        return std::ranges::any_of(_players, [&](const InputPlayer& p) {
            return p.map && map_edge(reach_of(p), action, edge);
        });
    }
    if (player >= static_cast<i32>(_players.size())) {
        return false;
    }
    return map_edge(reach_of(_players[static_cast<std::size_t>(player)]), action, edge);
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

bool binding_held(const Input& input, InputBinding binding, i32 player) {
    if (player < 0 || (input._players.empty() && player == 0)) {
        return input.binding_edge(input.everything(), binding, Input::Edge::Held);
    }
    if (player >= static_cast<i32>(input._players.size())) {
        return false;
    }
    return input.binding_edge(input.reach_of(input._players[static_cast<std::size_t>(player)]), binding, Input::Edge::Held);
}

f32 Input::reach_axis(const Reach& reach, const InputAxis& axis) const {
    const auto held_any = [&](const std::vector<InputBinding>& bindings) {
        return std::ranges::any_of(bindings, [&](InputBinding binding) { return binding_edge(reach, binding, Edge::Held); });
    };
    f32 value = (held_any(axis.positive) ? 1.0f : 0.0f) - (held_any(axis.negative) ? 1.0f : 0.0f);
    // A stick pushed further wins (on any of the controllers reached); the
    // dead zone taken off and the rest stretched to the end.
    const f32 dead = std::clamp(axis.dead_zone, 0.0f, 0.95f);
    for (i32 slot = 0; slot < max_gamepads; ++slot) {
        const GamepadState& pad = _pad_states[static_cast<std::size_t>(slot)];
        if (((reach.gamepads >> slot) & 1u) == 0 || !pad.connected) {
            continue;
        }
        for (const InputAxisStick& stick : axis.sticks) {
            f32 raw = 0.0f;
            switch (stick.axis) {
            case PadAxis::LeftX: raw = pad.left_x; break;
            case PadAxis::LeftY: raw = pad.left_y; break;
            case PadAxis::RightX: raw = pad.right_x; break;
            case PadAxis::RightY: raw = pad.right_y; break;
            case PadAxis::LeftTrigger: raw = pad.left_trigger; break;
            case PadAxis::RightTrigger: raw = pad.right_trigger; break;
            }
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
    if (player < 0 || (input._players.empty() && player == 0)) {
        return input.reach_axis(input.everything(), axis);
    }
    if (player >= static_cast<i32>(input._players.size())) {
        return 0.0f;
    }
    return input.reach_axis(input.reach_of(input._players[static_cast<std::size_t>(player)]), axis);
}

f32 Input::player_axis(i32 player, std::string_view name) const {
    const auto furthest = [](f32 a, f32 b) { return std::abs(b) > std::abs(a) ? b : a; };
    if (player < 0 || (_players.empty() && player == 0)) {
        f32 value = 0.0f;
        if (const InputAxis* axis = _map.axis(name)) {
            value = reach_axis(everything(), *axis);
        }
        for (const InputPlayer& p : _players) {
            if (p.map) {
                if (const InputAxis* axis = p.map->axis(name)) {
                    value = furthest(value, reach_axis(reach_of(p), *axis));
                }
            }
        }
        return value;
    }
    if (player >= static_cast<i32>(_players.size())) {
        return 0.0f;
    }
    const Reach reach = reach_of(_players[static_cast<std::size_t>(player)]);
    const InputAxis* axis = reach.map->axis(name);
    return axis ? reach_axis(reach, *axis) : 0.0f;
}

f32 Input::axis(std::string_view name) const {
    return player_axis(-1, name);
}

f32 Input::axis(i32 player, std::string_view name) const {
    return player_axis(player, name);
}

void Input::set_gamepad(i32 slot, const GamepadState& state) {
    if (slot < 0 || slot >= max_gamepads) {
        return;
    }
    const auto at = static_cast<std::size_t>(slot);
    GamepadState& was = _pad_states[at];
    if (state.connected && !was.connected) {
        _pad_plugged[at] = true;
    } else if (!state.connected && was.connected) {
        _pad_unplugged[at] = true;
    }
    was = state;
    Buttons<pad_control_count>& buttons = _pads[at];
    for (std::size_t control = 0; control < pad_control_count; ++control) {
        const bool now = pad_control_held(state, static_cast<PadControl>(control));
        if (now && !buttons.cur[control]) {
            _pad_pressed_at[at] = ++_presses;
        }
        buttons.set(control, now);
    }
}

const GamepadState& Input::gamepad(i32 slot) const {
    static const GamepadState none{};
    return slot >= 0 && slot < max_gamepads ? _pad_states[static_cast<std::size_t>(slot)] : none;
}

bool Input::gamepad_plugged(i32 slot) const {
    return slot >= 0 && slot < max_gamepads && _pad_plugged[static_cast<std::size_t>(slot)];
}

bool Input::gamepad_unplugged(i32 slot) const {
    return slot >= 0 && slot < max_gamepads && _pad_unplugged[static_cast<std::size_t>(slot)];
}

i32 Input::gamepad_pressed(PadControl control, bool free_only) const {
    const auto index = static_cast<std::size_t>(control);
    if (index >= pad_control_count) {
        return -1;
    }
    for (i32 slot = 0; slot < max_gamepads; ++slot) {
        if (_pads[static_cast<std::size_t>(slot)].test(index, Edge::Pressed) && (!free_only || player_of_gamepad(slot) < 0)) {
            return slot;
        }
    }
    return -1;
}

void Input::set_players(std::vector<InputPlayer> players) {
    _players = std::move(players);
}

i32 Input::add_player(InputPlayer player) {
    _players.push_back(std::move(player));
    return static_cast<i32>(_players.size()) - 1;
}

void Input::remove_player(i32 player) {
    if (player >= 0 && player < static_cast<i32>(_players.size())) {
        _players.erase(_players.begin() + player);
    }
}

i32 Input::player_of_gamepad(i32 slot) const {
    for (std::size_t p = 0; p < _players.size(); ++p) {
        if (std::ranges::find(_players[p].gamepads, slot) != _players[p].gamepads.end()) {
            return static_cast<i32>(p);
        }
    }
    return -1;
}

bool Input::player_connected(i32 player) const {
    if (player < 0 || (_players.empty() && player == 0)) {
        return true; // anyone: the keyboard is always there
    }
    if (player >= static_cast<i32>(_players.size())) {
        return false;
    }
    const InputPlayer& p = _players[static_cast<std::size_t>(player)];
    return p.keyboard || std::ranges::any_of(p.gamepads, [this](i32 slot) { return gamepad(slot).connected; });
}

i32 Input::last_gamepad(i32 player) const {
    const Reach reach = player < 0 || (_players.empty() && player == 0)
                            ? everything()
                            : player < static_cast<i32>(_players.size()) ? reach_of(_players[static_cast<std::size_t>(player)]) : Reach{};
    i32 best = -1;
    u64 best_at = 0;
    for (i32 slot = 0; slot < max_gamepads; ++slot) {
        const auto at = static_cast<std::size_t>(slot);
        if (((reach.gamepads >> slot) & 1u) == 0 || !_pad_states[at].connected) {
            continue;
        }
        if (best < 0 || _pad_pressed_at[at] > best_at) {
            best = slot;
            best_at = _pad_pressed_at[at];
        }
    }
    return best;
}

InputDevice Input::last_device(i32 player) const {
    const Reach reach = player < 0 || (_players.empty() && player == 0)
                            ? everything()
                            : player < static_cast<i32>(_players.size()) ? reach_of(_players[static_cast<std::size_t>(player)]) : Reach{};
    const i32 pad = last_gamepad(player);
    const u64 pad_at = pad >= 0 ? _pad_pressed_at[static_cast<std::size_t>(pad)] : 0;
    if (!reach.keyboard) {
        return InputDevice::Gamepad;
    }
    if (pad_at > _keyboard_pressed_at && pad_at > _mouse_pressed_at) {
        return InputDevice::Gamepad;
    }
    return _mouse_pressed_at > _keyboard_pressed_at ? InputDevice::Mouse : InputDevice::Keyboard;
}

void Input::rumble_gamepad(i32 slot, f32 low, f32 high, f32 seconds) {
    if (slot < 0 || slot >= max_gamepads || !_pad_states[static_cast<std::size_t>(slot)].connected) {
        return;
    }
    // A new rumble replaces the one going, so only the latest of each slot is kept.
    std::erase_if(_rumble, [slot](const GamepadRumble& r) { return r.slot == slot; });
    _rumble.push_back({.slot = slot,
                       .low = std::clamp(low, 0.0f, 1.0f),
                       .high = std::clamp(high, 0.0f, 1.0f),
                       .seconds = std::max(0.0f, seconds)});
}

void Input::rumble(i32 player, f32 low, f32 high, f32 seconds) {
    rumble_gamepad(last_gamepad(player), low, high, seconds);
}

} // namespace kin
