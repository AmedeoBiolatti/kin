#pragma once

#include <kin/core/types.hpp>
#include <kin/platform/window.hpp>

#include <array>
#include <filesystem>
#include <initializer_list>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kin {

// A file dropped from the desktop onto one of the app's windows.
struct DroppedFile {
    std::filesystem::path path;
    Vec2f pos{};          // window pixels, like Input::mouse_pos()
    WindowId window = 0;
};

class App;
class InputMap;

enum class Key {
    Unknown,
    Escape,
    Space,
    Enter,
    Tab,
    Backspace,
    Delete,
    Home,
    End,
    F1,
    F2,
    Num0,
    Num1,
    Num2,
    Num3,
    Num4,
    Num5,
    Num6,
    Num7,
    Num8,
    Num9,
    Up,
    Down,
    Left,
    Right,
    A,
    B,
    C,
    D,
    E,
    F,
    G,
    H,
    I,
    J,
    K,
    L,
    M,
    N,
    O,
    P,
    Q,
    R,
    S,
    T,
    U,
    V,
    W,
    X,
    Y,
    Z,
    Comma,
    Period,
    Minus,
    Equals,
    KeypadEnter,
    Backslash,  // the US \ key's place (scancodes are places, not characters)
    Grave,      // the key left of 1: ` on US layouts, \ on Italian ones
    LeftAlt,    // the left Alt key on its own (AltGr, the right one, types characters on many layouts)
    // Added after the rest so the codes above keep their values.
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,
    PageUp,
    PageDown,
};

// The last Key: codes run from Key::Unknown to this.
inline constexpr Key LastKey = Key::PageDown;

enum class KeyModifiers : u8 {
    None = 0,
    Shift = 1 << 0,
    Ctrl = 1 << 1,
    Alt = 1 << 2,
};

constexpr KeyModifiers operator|(KeyModifiers a, KeyModifiers b) {
    return static_cast<KeyModifiers>(static_cast<u8>(a) | static_cast<u8>(b));
}

constexpr KeyModifiers operator&(KeyModifiers a, KeyModifiers b) {
    return static_cast<KeyModifiers>(static_cast<u8>(a) & static_cast<u8>(b));
}

constexpr bool any(KeyModifiers modifiers) {
    return static_cast<u8>(modifiers) != 0;
}

enum class MouseButton : u8 {
    Left,
    Middle,
    Right,
    X1,
    X2,
};

enum class InputBindingDevice {
    Key,
    MouseButton,
};

struct InputBinding {
    InputBindingDevice device = InputBindingDevice::Key;
    i32 code = 0;
    KeyModifiers modifiers = KeyModifiers::None;

    friend constexpr bool operator==(InputBinding, InputBinding) = default;
};

struct InputAction {
    std::string name;
    std::vector<InputBinding> bindings;
};

struct InputActionHint {
    std::string name;
    std::string label;
    std::string description;
};

struct AvailableInputAction {
    std::string name;
    std::string label;
    std::string description;
    std::vector<InputBinding> bindings;
};

std::string_view key_name(Key key);
std::string_view mouse_button_name(MouseButton button);
std::string_view binding_name(InputBinding binding);

InputMap load_input_map(const std::filesystem::path& path);
bool save_input_map(const InputMap& map, const std::filesystem::path& path);

class InputMap {
public:
    void bind(std::string_view action, Key key);
    void bind(std::string_view action, Key key, KeyModifiers modifiers);
    void bind(std::string_view action, std::initializer_list<Key> keys);
    void bind(std::string_view action, MouseButton button);
    void unbind(std::string_view action);
    void clear();

    bool empty() const { return _bindings.empty(); }
    i32 action_count() const { return static_cast<i32>(_bindings.size()); }
    std::vector<InputAction> actions() const;
    const std::vector<InputBinding>* bindings(std::string_view action) const;

private:
    friend class Input;

    std::unordered_map<std::string, std::vector<InputBinding>> _bindings;
};

class InputActionContext {
public:
    explicit InputActionContext(std::string_view name = {});

    const std::string& name() const { return _name; }
    const std::vector<InputActionHint>& actions() const { return _actions; }
    const std::vector<InputActionContext>& children() const { return _children; }

    bool empty() const { return _actions.empty() && _children.empty(); }
    void clear();

    void add(std::string_view action, std::string_view label = {}, std::string_view description = {});
    void include(const InputActionContext& context);

    std::vector<InputActionHint> flattened_actions() const;
    std::vector<AvailableInputAction> resolve(const InputMap& map) const;

private:
    std::string _name;
    std::vector<InputActionHint> _actions;
    std::vector<InputActionContext> _children;
};

class Input {
public:
    void begin_frame();

    // Consume the STICKY keyboard edges (read by update() via pressed()) between
    // the first fixed step and render, so a multi-step frame does not re-fire a key
    // in later steps. Per-frame keyboard edges (frame_pressed(), read by render-time
    // UI) live one rendered frame; per-frame mouse edges are left intact and persist
    // until the next stepping frame (see begin_frame). Render-time UI reads keyboard
    // via frame_pressed().
    void advance_keyboard_edges();

    // Consume this frame's per-frame edges (keyboard + mouse frame_pressed/released,
    // key repeats, typed text and the wheel).
    // A scene that fully handles its input in update() calls this so its own render()
    // redraw does not re-read the same edges (double nav / double click). Only the
    // calling scene is affected — edges already read are simply cleared early; the
    // next begin_frame would clear them anyway.
    void consume_frame_edges();

    bool pressed(Key key) const;
    bool held(Key key) const;
    bool released(Key key) const;
    bool frame_pressed(Key key) const;
    bool frame_released(Key key) const;
    // The OS's auto-repeat while `key` is held (not the first press): text editing
    // reads frame_pressed() || frame_repeated() so held keys repeat; menus that read
    // frame_pressed() alone move once per press. Lives one rendered frame.
    bool frame_repeated(Key key) const;
    bool modifier_held(KeyModifiers modifiers) const;
    u64 last_key_press_event_time_ns() const { return _last_key_press_event_time_ns; }
    u64 last_key_press_detected_time_ns() const { return _last_key_press_detected_time_ns; }

    Vec2f mouse_pos() const { return _mouse_pos; }
    Vec2f mouse_pos(WindowId window_id) const;
    WindowId mouse_window() const { return _mouse_window; }
    WindowId keyboard_window() const { return _keyboard_window; }

    // Input-edge contract (matters at refresh > sim rate, where most rendered frames
    // run zero fixed update() steps):
    //   * STICKY edges — mouse_pressed()/mouse_released(), pressed()/released() — survive
    //     0-step frames (cleared only on a stepping frame). Read these from update().
    //   * PER-FRAME MOUSE edges — mouse_frame_pressed()/mouse_frame_released() — also
    //     survive 0-step frames now, so ui2 (which reads clicks through these) works
    //     whether its widgets run in update() or render(). They are cleared on the next
    //     stepping frame, or earlier via consume_mouse_frame_pressed() once a widget
    //     claims the press (keeps `pressed` a single-frame edge for drag/grab capture).
    //   * PER-FRAME KEYBOARD edges — frame_pressed()/frame_released() — live exactly one
    //     rendered frame. Keyboard read from update() must use the sticky pressed() API.
    bool mouse_pressed(MouseButton button) const;
    bool mouse_held(MouseButton button) const;
    bool mouse_released(MouseButton button) const;
    bool mouse_frame_pressed(MouseButton button) const;
    bool mouse_frame_released(MouseButton button) const;
    // Mark this button's per-frame press edge as consumed: it stays readable for the
    // rest of THIS frame (so other same-frame widgets see consistent state) but is
    // cleared at the next begin_frame instead of persisting across 0-step frames.
    // ui2's Context::region() calls this when it claims a press.
    void consume_mouse_frame_pressed(MouseButton button);
    bool mouse_dragging(MouseButton button) const;
    Vec2f drag_delta(MouseButton button) const;
    f32 mouse_wheel_y() const { return _mouse_wheel_y; }
    void set_mouse_wheel_y(f32 value);
    void set_mouse_pos(Vec2f pos, WindowId window_id = 0);
    void set_mouse_held(MouseButton button, bool held);
    void set_mouse_pressed(MouseButton button);
    void set_text_input(std::string_view text);
    // Test/scripting hooks: a press edge of `key` this frame (and it held), or an
    // auto-repeat of a key already held.
    void set_key_pressed(Key key);
    void set_key_released(Key key);
    void set_key_repeated(Key key);
    void set_modifier_held(KeyModifiers modifiers, bool held);
    void set_clipboard_text(std::string_view text);

    std::string_view text_input() const { return _text_input; }
    // Text an input method (IME) is composing, not yet typed: what a Japanese,
    // Chinese or Korean player is spelling before they pick it. Shown at the
    // caret, underlined; text_input() receives it once committed. Empty when
    // nothing is being composed. `cursor` is the byte offset of the IME's own
    // cursor within it.
    std::string_view text_composition() const { return _composition; }
    i32 text_composition_cursor() const { return _composition_cursor; }
    void set_text_composition(std::string_view text, i32 cursor = -1);
    std::string clipboard_text() const;

    // Files dropped since they were last taken, oldest first. Taking clears
    // them, so each drop is handled once however frames and updates interleave.
    std::vector<DroppedFile> take_dropped_files();
    bool has_dropped_files() const { return !_dropped_files.empty(); }
    // Where a drag from the desktop is over a window while it is in progress, to
    // highlight a drop target; nullopt otherwise. Window pixels.
    std::optional<Vec2f> drop_position() const { return _drop_position; }
    // A drop as if the desktop made it: for tests and agents.
    void add_dropped_file(DroppedFile file);

    void bind(std::string_view action, Key key);
    void bind(std::string_view action, Key key, KeyModifiers modifiers);
    void bind(std::string_view action, std::initializer_list<Key> keys);
    void bind(std::string_view action, MouseButton button);
    void unbind(std::string_view action);
    void clear_bindings();
    void set_map(const InputMap& map);
    const InputMap& map() const { return _map; }

    void set_action_held(std::string_view action, bool held);
    void set_action_pressed(std::string_view action);

    bool pressed(std::string_view action) const;
    bool held(std::string_view action) const;
    bool released(std::string_view action) const;
    bool frame_pressed(std::string_view action) const;
    bool frame_released(std::string_view action) const;

private:
    friend class App;
    // Test-only access to the step-aware begin_frame(bool); the public begin_frame()
    // always advances, so tests need this seam to emulate 0-step (advance=false) frames.
    friend struct InputFrameTestHook;

    static constexpr i32 KEY_COUNT = 512;
    static constexpr i32 MOUSE_BUTTON_COUNT = 6;
    static constexpr f32 DRAG_THRESHOLD = 4.0f;

    void begin_frame(bool advance_transients);
    void process_native_event(const void* event);

    std::array<bool, KEY_COUNT> _key_cur = {};
    std::array<bool, KEY_COUNT> _key_prev = {};
    std::array<bool, KEY_COUNT> _key_pressed = {};
    std::array<bool, KEY_COUNT> _key_released = {};
    std::array<bool, KEY_COUNT> _key_frame_pressed = {};
    std::array<bool, KEY_COUNT> _key_frame_repeated = {};
    std::array<bool, KEY_COUNT> _key_frame_released = {};

    std::array<bool, MOUSE_BUTTON_COUNT> _mouse_cur = {};
    std::array<bool, MOUSE_BUTTON_COUNT> _mouse_prev = {};
    std::array<bool, MOUSE_BUTTON_COUNT> _mouse_pressed = {};
    std::array<bool, MOUSE_BUTTON_COUNT> _mouse_released = {};
    std::array<bool, MOUSE_BUTTON_COUNT> _mouse_frame_pressed = {};
    std::array<bool, MOUSE_BUTTON_COUNT> _mouse_frame_released = {};
    // Press edges claimed by a widget this frame: cleared at the next begin_frame even
    // on a 0-step frame, so a claimed press does not re-fire while it persists.
    std::array<bool, MOUSE_BUTTON_COUNT> _mouse_frame_pressed_consumed = {};
    std::array<Vec2f, MOUSE_BUTTON_COUNT> _drag_start = {};

    Vec2f _mouse_pos = {};
    Vec2f _mouse_pos_prev = {};
    WindowId _mouse_window = 0;
    WindowId _keyboard_window = 0;
    f32 _mouse_wheel_y = 0.0f;
    u64 _last_key_press_event_time_ns = 0;
    u64 _last_key_press_detected_time_ns = 0;
    std::string _text_input;
    std::string _composition;
    i32 _composition_cursor = 0;
    std::vector<DroppedFile> _dropped_files;
    std::optional<Vec2f> _drop_position;
    std::string _clipboard_text;

    std::unordered_map<WindowId, Vec2f> _window_mouse_pos;
    InputMap _map;
};

} // namespace kin
