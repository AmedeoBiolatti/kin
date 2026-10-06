#include <kin/platform/input.hpp>

#include <kin/core/utf8.hpp>
#include <kin/platform/log.hpp>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
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
    return mouse_button_name(to_mouse_button(binding.code));
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

InputMap load_input_map(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        KIN_LOG_ERROR_F("input",
                        "input map open failed",
                        (LogFields{{.name = "path", .value = path.string()},
                                   {.name = "error", .value = "open_failed"}}));
        throw std::runtime_error("Failed to open input map: " + path.string());
    }

    InputMap map;
    std::string line;
    i32 line_no = 0;
    const auto fail = [&](std::string_view message) {
        KIN_LOG_ERROR_F("input",
                        "input map parse failed",
                        (LogFields{{.name = "path", .value = path.string()},
                                   {.name = "line", .value = std::to_string(line_no)},
                                   {.name = "error", .value = std::string{message}}}));
        throw std::runtime_error(path.string() + ":" + std::to_string(line_no) + ": " + std::string{message});
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
                fail("key requires action and key name");
            }

            Key key = Key::Unknown;
            if (!parse_key(key_name_text, key)) {
                fail("unknown key");
            }

            KeyModifiers modifiers = KeyModifiers::None;
            for (std::string modifier; in >> modifier;) {
                if (!parse_modifier(modifier, modifiers)) {
                    fail("unknown key modifier");
                }
            }

            map.bind(action, key, modifiers);
            continue;
        }

        if (kind == "mouse") {
            std::string action;
            std::string button_name_text;
            if (!(in >> action >> button_name_text)) {
                fail("mouse requires action and button name");
            }

            MouseButton button = MouseButton::Left;
            if (!parse_mouse_button(button_name_text, button)) {
                fail("unknown mouse button");
            }

            map.bind(action, button);
            continue;
        }

        fail("unknown directive");
    }

    KIN_LOG_INFO_F("input",
                   "input map loaded",
                   (LogFields{{.name = "path", .value = path.string()},
                              {.name = "count", .value = std::to_string(map.action_count())},
                              {.name = "status", .value = "loaded"}}));
    return map;
}

bool save_input_map(const InputMap& map, const std::filesystem::path& path) {
    std::ofstream out(path);
    if (!out) {
        KIN_LOG_ERROR_F("input",
                        "input map save failed",
                        (LogFields{{.name = "path", .value = path.string()},
                                   {.name = "error", .value = "open_failed"}}));
        return false;
    }

    out << "# kin input map\n";
    for (const InputAction& action : map.actions()) {
        for (InputBinding binding : action.bindings) {
            if (binding.device == InputBindingDevice::Key) {
                out << "key " << action.name << ' ' << key_name(to_key(binding.code));
                write_modifiers(out, binding.modifiers);
                out << '\n';
            } else if (binding.device == InputBindingDevice::MouseButton) {
                out << "mouse " << action.name << ' ' << mouse_button_name(to_mouse_button(binding.code)) << '\n';
            }
        }
    }

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

void Input::advance_keyboard_edges() {
    // Consume only the STICKY keyboard edges (read by update() via pressed()), so a
    // fast frame running several fixed steps does not re-fire a key in later steps.
    // The per-frame keyboard edges (read by render() via frame_pressed(): text-field
    // backspace/arrows/enter, etc.) and all mouse/text/wheel edges are deliberately
    // left intact so render-time immediate UI sees this frame's input. (Clearing the
    // frame edges here dropped ~half of render-time keystrokes; clearing the mouse
    // edges dropped ~half of clicks.) Per-frame keyboard edges live exactly one
    // rendered frame; per-frame mouse edges live until the next stepping frame (see
    // begin_frame) so update()-time ui2 can observe a click from a 0-step frame.
    _key_prev = _key_cur;
    _key_pressed.fill(false);
    _key_released.fill(false);
}

void Input::consume_frame_edges() {
    _key_frame_pressed.fill(false);
    _key_frame_repeated.fill(false);
    _key_frame_released.fill(false);
    _mouse_frame_pressed.fill(false);
    _mouse_frame_released.fill(false);
    _mouse_wheel_y = 0.0f;
    _text_input.clear();
}

void Input::begin_frame(bool advance_transients) {
    if (advance_transients) {
        // A stepping frame ran at least one fixed update() last frame: do the full
        // reset, including the per-frame MOUSE edges. At sim rate every frame steps, so
        // this is the only path taken and behaviour is identical to a plain per-frame
        // clear.
        _key_prev = _key_cur;
        _key_pressed.fill(false);
        _key_released.fill(false);
        _mouse_prev = _mouse_cur;
        _mouse_pressed.fill(false);
        _mouse_released.fill(false);
        _mouse_frame_pressed.fill(false);
        _mouse_frame_released.fill(false);
    } else {
        // A 0-step frame (refresh > sim rate): the per-frame MOUSE edges PERSIST so UI
        // driven from update() can still read a click that landed here on the next
        // stepping frame — otherwise the edge would be wiped before any update() step
        // runs, which is exactly the "press the mouse N times before it registers" bug.
        // Press edges already CLAIMED by a widget this frame are the exception: clear
        // them so a claimed press does not re-fire (e.g. re-capturing a drag origin)
        // while it persists. Release edges are left to persist; their consumers are
        // guarded by active/drag state that clears once, so they do not re-fire.
        for (i32 button = 0; button < MOUSE_BUTTON_COUNT; ++button) {
            if (_mouse_frame_pressed_consumed[button]) {
                _mouse_frame_pressed[button] = false;
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
    _key_frame_pressed.fill(false);
    _key_frame_repeated.fill(false);
    _key_frame_released.fill(false);
    _mouse_pos_prev = _mouse_pos;
    _mouse_wheel_y = 0.0f;
    _text_input.clear();
}

void Input::process_native_event(const void* native_event) {
    const auto& event = *static_cast<const SDL_Event*>(native_event);

    if (event.type == SDL_EVENT_KEY_DOWN && valid_scancode(event.key.scancode)) {
        if (!_key_cur[event.key.scancode]) {
            _key_pressed[event.key.scancode] = true;
            _key_frame_pressed[event.key.scancode] = true;
            _last_key_press_event_time_ns = event.key.timestamp;
            _last_key_press_detected_time_ns = SDL_GetTicksNS();
        } else if (event.key.repeat) {
            _key_frame_repeated[event.key.scancode] = true;
        }
        _key_cur[event.key.scancode] = true;
        _keyboard_window = static_cast<WindowId>(event.key.windowID);
    } else if (event.type == SDL_EVENT_KEY_UP && valid_scancode(event.key.scancode)) {
        if (_key_cur[event.key.scancode]) {
            _key_released[event.key.scancode] = true;
            _key_frame_released[event.key.scancode] = true;
        }
        _key_cur[event.key.scancode] = false;
        _keyboard_window = static_cast<WindowId>(event.key.windowID);
    } else if (event.type == SDL_EVENT_MOUSE_MOTION) {
        _mouse_pos = {event.motion.x, event.motion.y};
        _mouse_window = static_cast<WindowId>(event.motion.windowID);
        _window_mouse_pos[_mouse_window] = _mouse_pos;
    } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && valid_mouse_button(event.button.button)) {
        if (!_mouse_cur[event.button.button]) {
            _mouse_pressed[event.button.button] = true;
            _mouse_frame_pressed[event.button.button] = true;
        }
        _mouse_cur[event.button.button] = true;
        _drag_start[event.button.button] = {event.button.x, event.button.y};
        _mouse_pos = {event.button.x, event.button.y};
        _mouse_window = static_cast<WindowId>(event.button.windowID);
        _window_mouse_pos[_mouse_window] = _mouse_pos;
    } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && valid_mouse_button(event.button.button)) {
        if (_mouse_cur[event.button.button]) {
            _mouse_released[event.button.button] = true;
            _mouse_frame_released[event.button.button] = true;
        }
        _mouse_cur[event.button.button] = false;
        _mouse_pos = {event.button.x, event.button.y};
        _mouse_window = static_cast<WindowId>(event.button.windowID);
        _window_mouse_pos[_mouse_window] = _mouse_pos;
    } else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
        _mouse_wheel_y += event.wheel.y;
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
    return valid_scancode(scancode) && (_key_pressed[scancode] || (_key_cur[scancode] && !_key_prev[scancode]));
}

bool Input::held(Key key) const {
    const SDL_Scancode scancode = to_sdl_scancode(key);
    return valid_scancode(scancode) && _key_cur[scancode];
}

bool Input::released(Key key) const {
    const SDL_Scancode scancode = to_sdl_scancode(key);
    return valid_scancode(scancode) && (_key_released[scancode] || (!_key_cur[scancode] && _key_prev[scancode]));
}

bool Input::frame_pressed(Key key) const {
    const SDL_Scancode scancode = to_sdl_scancode(key);
    return valid_scancode(scancode) && _key_frame_pressed[scancode];
}

bool Input::frame_repeated(Key key) const {
    const SDL_Scancode scancode = to_sdl_scancode(key);
    return valid_scancode(scancode) && _key_frame_repeated[scancode];
}

bool Input::frame_released(Key key) const {
    const SDL_Scancode scancode = to_sdl_scancode(key);
    return valid_scancode(scancode) && _key_frame_released[scancode];
}

bool Input::modifier_held(KeyModifiers modifiers) const {
    if (!any(modifiers)) {
        return true;
    }
    const bool shift = _key_cur[SDL_SCANCODE_LSHIFT] || _key_cur[SDL_SCANCODE_RSHIFT];
    const bool ctrl = _key_cur[SDL_SCANCODE_LCTRL] || _key_cur[SDL_SCANCODE_RCTRL];
    const bool alt = _key_cur[SDL_SCANCODE_LALT] || _key_cur[SDL_SCANCODE_RALT];
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
    return valid_mouse_button(raw_button) && (_mouse_pressed[raw_button] || (_mouse_cur[raw_button] && !_mouse_prev[raw_button]));
}

bool Input::mouse_held(MouseButton button) const {
    const u8 raw_button = to_sdl_mouse_button(button);
    return valid_mouse_button(raw_button) && _mouse_cur[raw_button];
}

bool Input::mouse_released(MouseButton button) const {
    const u8 raw_button = to_sdl_mouse_button(button);
    return valid_mouse_button(raw_button) && (_mouse_released[raw_button] || (!_mouse_cur[raw_button] && _mouse_prev[raw_button]));
}

bool Input::mouse_frame_pressed(MouseButton button) const {
    const u8 raw_button = to_sdl_mouse_button(button);
    return valid_mouse_button(raw_button) && _mouse_frame_pressed[raw_button];
}

bool Input::mouse_frame_released(MouseButton button) const {
    const u8 raw_button = to_sdl_mouse_button(button);
    return valid_mouse_button(raw_button) && _mouse_frame_released[raw_button];
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
    if (!valid_mouse_button(raw_button) || !_mouse_cur[raw_button]) {
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

void Input::set_mouse_wheel_y(f32 value) {
    _mouse_wheel_y = value;
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

    if (held && !_mouse_cur[raw_button]) {
        _mouse_pressed[raw_button] = true;
        _mouse_frame_pressed[raw_button] = true;
    } else if (!held && _mouse_cur[raw_button]) {
        _mouse_released[raw_button] = true;
        _mouse_frame_released[raw_button] = true;
    }
    _mouse_cur[raw_button] = held;
    if (held) {
        _drag_start[raw_button] = _mouse_pos;
    }
}

void Input::set_mouse_pressed(MouseButton button) {
    const u8 raw_button = to_sdl_mouse_button(button);
    if (!valid_mouse_button(raw_button)) {
        return;
    }

    _mouse_prev[raw_button] = false;
    _mouse_cur[raw_button] = true;
    _mouse_pressed[raw_button] = true;
    _mouse_frame_pressed[raw_button] = true;
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
    _key_prev[scancode] = false;
    _key_cur[scancode] = true;
    _key_pressed[scancode] = true;
    _key_frame_pressed[scancode] = true;
}

void Input::set_key_released(Key key) {
    const SDL_Scancode scancode = to_sdl_scancode(key);
    if (!valid_scancode(scancode) || !_key_cur[scancode]) {
        return;
    }
    _key_cur[scancode] = false;
    _key_released[scancode] = true;
    _key_frame_released[scancode] = true;
}

void Input::set_key_repeated(Key key) {
    const SDL_Scancode scancode = to_sdl_scancode(key);
    if (!valid_scancode(scancode) || !_key_cur[scancode]) {
        return;
    }
    _key_frame_repeated[scancode] = true;
}

void Input::set_modifier_held(KeyModifiers modifiers, bool held) {
    if (has_modifier(modifiers, KeyModifiers::Shift)) {
        _key_cur[SDL_SCANCODE_LSHIFT] = held;
    }
    if (has_modifier(modifiers, KeyModifiers::Ctrl)) {
        _key_cur[SDL_SCANCODE_LCTRL] = held;
    }
    if (has_modifier(modifiers, KeyModifiers::Alt)) {
        _key_cur[SDL_SCANCODE_LALT] = held;
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
            if (is_held && !_key_cur[scancode]) {
                _key_pressed[scancode] = true;
                _key_frame_pressed[scancode] = true;
            } else if (!is_held && _key_cur[scancode]) {
                _key_released[scancode] = true;
                _key_frame_released[scancode] = true;
            }
            _key_cur[scancode] = is_held;
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
        _key_prev[scancode] = false;
        _key_cur[scancode] = true;
        _key_pressed[scancode] = true;
        _key_frame_pressed[scancode] = true;
    }
}

bool Input::pressed(std::string_view action) const {
    const std::vector<InputBinding>* bindings = _map.bindings(action);
    if (!bindings) {
        return false;
    }

    return std::ranges::any_of(*bindings, [this](InputBinding binding) {
        if (binding.device == InputBindingDevice::Key) {
            const SDL_Scancode scancode = binding_scancode(binding);
            return valid_scancode(scancode) && modifier_held(binding.modifiers) &&
                (_key_pressed[scancode] || (_key_cur[scancode] && !_key_prev[scancode]));
        }
        const u8 button = binding_mouse_button(binding);
        return valid_mouse_button(button) && (_mouse_pressed[button] || (_mouse_cur[button] && !_mouse_prev[button]));
    });
}

bool Input::held(std::string_view action) const {
    const std::vector<InputBinding>* bindings = _map.bindings(action);
    if (!bindings) {
        return false;
    }

    return std::ranges::any_of(*bindings, [this](InputBinding binding) {
        if (binding.device == InputBindingDevice::Key) {
            const SDL_Scancode scancode = binding_scancode(binding);
            return valid_scancode(scancode) && modifier_held(binding.modifiers) && _key_cur[scancode];
        }
        const u8 button = binding_mouse_button(binding);
        return valid_mouse_button(button) && _mouse_cur[button];
    });
}

bool Input::released(std::string_view action) const {
    const std::vector<InputBinding>* bindings = _map.bindings(action);
    if (!bindings) {
        return false;
    }

    return std::ranges::any_of(*bindings, [this](InputBinding binding) {
        if (binding.device == InputBindingDevice::Key) {
            const SDL_Scancode scancode = binding_scancode(binding);
            return valid_scancode(scancode) && modifier_held(binding.modifiers) &&
                (_key_released[scancode] || (!_key_cur[scancode] && _key_prev[scancode]));
        }
        const u8 button = binding_mouse_button(binding);
        return valid_mouse_button(button) && (_mouse_released[button] || (!_mouse_cur[button] && _mouse_prev[button]));
    });
}

bool Input::frame_pressed(std::string_view action) const {
    const std::vector<InputBinding>* bindings = _map.bindings(action);
    if (!bindings) {
        return false;
    }

    return std::ranges::any_of(*bindings, [this](InputBinding binding) {
        if (binding.device == InputBindingDevice::Key) {
            const SDL_Scancode scancode = binding_scancode(binding);
            return valid_scancode(scancode) && modifier_held(binding.modifiers) && _key_frame_pressed[scancode];
        }
        const u8 button = binding_mouse_button(binding);
        return valid_mouse_button(button) && _mouse_frame_pressed[button];
    });
}

bool Input::frame_released(std::string_view action) const {
    const std::vector<InputBinding>* bindings = _map.bindings(action);
    if (!bindings) {
        return false;
    }

    return std::ranges::any_of(*bindings, [this](InputBinding binding) {
        if (binding.device == InputBindingDevice::Key) {
            const SDL_Scancode scancode = binding_scancode(binding);
            return valid_scancode(scancode) && modifier_held(binding.modifiers) && _key_frame_released[scancode];
        }
        const u8 button = binding_mouse_button(binding);
        return valid_mouse_button(button) && _mouse_frame_released[button];
    });
}

} // namespace kin
