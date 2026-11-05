#pragma once

#include <kin/core/types.hpp>
#include <kin/platform/gamepad.hpp>
#include <kin/platform/window.hpp>

#include <array>
#include <filesystem>
#include <initializer_list>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kin {

// A file dropped from the desktop onto one of the app's windows.
struct DroppedFile {
    std::filesystem::path path;
    Vec2f pos{};          // window pixels, like Input::mouse_pos()
    WindowId window = 0;
};

class App;
class Input;
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
    LeftShift, // the left Shift key on its own (a first-person game's dash or sprint)
};

// The last Key: codes run from Key::Unknown to this.
inline constexpr Key LastKey = Key::LeftShift;

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

// A control of a game controller an action can be bound to: its buttons, each
// stick's four ways (pushed past halfway) and its triggers (pulled past halfway).
enum class PadControl : u8 {
    A, // the south face button (Cross)
    B,
    X,
    Y,
    Back,
    Start,
    LeftStick, // pressed in
    RightStick,
    LeftShoulder,
    RightShoulder,
    DPadUp,
    DPadDown,
    DPadLeft,
    DPadRight,
    LeftStickLeft,
    LeftStickRight,
    LeftStickUp,
    LeftStickDown,
    RightStickLeft,
    RightStickRight,
    RightStickUp,
    RightStickDown,
    LeftTrigger,
    RightTrigger,
    Count,
};

// A game controller's analog input, for an axis.
enum class PadAxis : u8 { LeftX, LeftY, RightX, RightY, LeftTrigger, RightTrigger };

enum class InputBindingDevice {
    Key,
    MouseButton,
    Gamepad, // code: a PadControl
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

// A number from -1 to 1 the player sets: keys, buttons or a stick's ways
// for its two ends (held: -1 or 1, both: 0), or a controller's analog input
// (`sticks`; `inverted`: its other way), less a dead zone in the middle; the
// one pushed furthest wins. "move_x" from A and D, the arrows and the left stick.
struct InputAxisStick {
    PadAxis axis = PadAxis::LeftX;
    bool inverted = false;

    friend constexpr bool operator==(InputAxisStick, InputAxisStick) = default;
};

struct InputAxis {
    std::string name;
    std::vector<InputBinding> negative;
    std::vector<InputBinding> positive;
    std::vector<InputAxisStick> sticks;
    f32 dead_zone = 0.2f;

    friend bool operator==(const InputAxis&, const InputAxis&) = default;
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
std::string_view pad_control_name(PadControl control); // "a", "dpad_left", "left_stick_up", "left_trigger"
std::string_view pad_axis_name(PadAxis axis);         // "left_x", "right_trigger"
bool parse_key(std::string_view name, Key& out);
bool parse_mouse_button(std::string_view name, MouseButton& out);
bool parse_pad_control(std::string_view name, PadControl& out);
bool parse_pad_axis(std::string_view name, PadAxis& out);
// Whether a controller's control is down: a button, a stick's way or a
// trigger past halfway (none while it is not connected).
bool pad_control_held(const GamepadState& pad, PadControl control);
// Whether `binding` is held on `input` (its modifiers not asked), and an
// axis's value there (-1 to 1): what Input::held() and Input::axis() read,
// for a binding or an axis that is not in its map (an editor trying one).
// `player` as in Input::held(player, ...): -1 anyone (every device), else that
// player's devices.
bool binding_held(const Input& input, InputBinding binding, i32 player = -1);
f32 axis_value(const Input& input, const InputAxis& axis, i32 player = -1);

// A binding as an input file writes it in an axis: "key:A", "mouse:MouseLeft", "pad:dpad_left".
std::string binding_text(InputBinding binding);
std::optional<InputBinding> parse_binding_text(std::string_view text);

// An input file: "key <action> <Key> [Shift|Ctrl|Alt...]", "mouse <action>
// <MouseButton>", "pad <action> <control>", and an axis's lines, "axis <name>
// - <binding>", "axis <name> + <binding>", "axis <name> stick [-]<pad axis>",
// "axis <name> deadzone <0..1>". Read (false: `error` says which line and why),
// and written, actions and axes by name.
bool parse_input_map(std::string_view text, InputMap& out, std::string& error);
std::string serialize_input_map(const InputMap& map);
InputMap load_input_map(const std::filesystem::path& path);
bool save_input_map(const InputMap& map, const std::filesystem::path& path);

class InputMap {
public:
    void bind(std::string_view action, Key key);
    void bind(std::string_view action, Key key, KeyModifiers modifiers);
    void bind(std::string_view action, std::initializer_list<Key> keys);
    void bind(std::string_view action, MouseButton button);
    void bind(std::string_view action, PadControl control);
    // Its bindings, all of them (none: it is gone).
    void set_bindings(std::string_view action, std::vector<InputBinding> bindings);
    void unbind(std::string_view action);
    void clear();

    bool empty() const { return _bindings.empty() && _axes.empty(); }
    i32 action_count() const { return static_cast<i32>(_bindings.size()); }
    std::vector<InputAction> actions() const;
    const std::vector<InputBinding>* bindings(std::string_view action) const;

    // Axes, by name: one of that name replaced.
    void set_axis(InputAxis axis);
    bool remove_axis(std::string_view name);
    const InputAxis* axis(std::string_view name) const;
    std::vector<InputAxis> axes() const;

    friend bool operator==(const InputMap&, const InputMap&) = default;

private:
    friend class Input;

    std::unordered_map<std::string, std::vector<InputBinding>> _bindings;
    std::unordered_map<std::string, InputAxis> _axes;
};

// A local player and what they play with (Input::set_players): the keyboard
// and mouse, controllers by slot (Gamepads), and, when they share a keyboard
// with another player (one on WASD, one on the arrows), their own actions and
// axes. Several players may have the keyboard; a controller is one player's.
struct InputPlayer {
    bool keyboard = false; // the keyboard and the mouse
    std::vector<i32> gamepads; // controller slots, 0 to max_gamepads - 1
    std::optional<InputMap> map; // its own bindings; none: the Input's map

    friend bool operator==(const InputPlayer&, const InputPlayer&) = default;
};

// The kind of device a player last used (for the prompt to show them).
enum class InputDevice : u8 { Keyboard, Mouse, Gamepad };

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

    // Consume what update() reads that must reach exactly one fixed step: the STICKY
    // keyboard edges (pressed()/released()) and the step wheel (mouse_wheel_y()).
    // App::run calls it after the first fixed step, so a multi-step frame does not
    // re-fire a key or re-apply a wheel notch in later steps. Per-frame keyboard edges
    // (frame_pressed(), read by render-time UI) and the frame wheel live one rendered
    // frame; per-frame mouse edges are left intact and persist until the next stepping
    // frame (see begin_frame).
    void advance_step_edges();

    // Consume this frame's per-frame edges (keyboard + mouse frame_pressed/released,
    // key repeats, typed text and frame_mouse_wheel_y()).
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
    // When the OS reported the latest mouse motion (SDL_GetTicksNS clock): with
    // the time a frame shows it, the delay between the hand and the screen.
    u64 last_mouse_motion_event_time_ns() const { return _last_mouse_motion_event_time_ns; }
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
    //   * The wheel has a sticky and a per-frame reading too: see mouse_wheel_y().
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
    // The wheel, in notches (positive away from the player). Like pressed() and
    // frame_pressed(), there are two:
    //   * mouse_wheel_y(), for update(): what the wheel turned since a fixed step last
    //     read it. It adds up across 0-step frames and is used up by the first step of
    //     a frame (advance_step_edges), so later steps of that frame see 0.
    //   * frame_mouse_wheel_y(), for render-time UI (ui2 scrolls with it): what the
    //     wheel turned since the last rendered frame. Lives one rendered frame.
    f32 mouse_wheel_y() const { return _mouse_wheel_y; }
    f32 frame_mouse_wheel_y() const { return _frame_mouse_wheel_y; }
    // A wheel turn of `value` notches this frame, as the OS would report it: it adds
    // to both mouse_wheel_y() and frame_mouse_wheel_y().
    void set_mouse_wheel_y(f32 value);
    // The same sideways (tilting wheels, touchpads), positive to the right.
    f32 mouse_wheel_x() const { return _mouse_wheel_x; }
    f32 frame_mouse_wheel_x() const { return _frame_mouse_wheel_x; }
    void set_mouse_wheel_x(f32 value);
    // How far the mouse moved, in window pixels (x right, y down), whether or not
    // the pointer could follow: with the window's relative mouse mode on (a
    // first-person camera; Window::set_relative_mouse) the pointer stays put and
    // only this changes. Two readings, as for the wheel:
    //   * mouse_motion(), for update(): since a fixed step last read it, used up
    //     by the first step of a frame.
    //   * frame_mouse_motion(), for render(): since the last rendered frame. Turn
    //     a camera with this for the least delay between the hand and the screen.
    Vec2f mouse_motion() const { return _mouse_motion; }
    Vec2f frame_mouse_motion() const { return _frame_mouse_motion; }
    // Motion of `delta` this frame, as the OS would report it: it adds to both.
    void set_mouse_motion(Vec2f delta);
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
    // An axis of the map, -1 to 1 (0 when there is no such axis).
    f32 axis(std::string_view name) const;

    // Local players. With none set there is one, player 0, who plays with
    // every device: a one-player game sets nothing. Once players are set, each
    // reads only their own devices, with their own map if they have one:
    //
    //     input.set_players({{.keyboard = true}, {.gamepads = {0}}, {.gamepads = {1}}});
    //     if (input.pressed(1, "jump")) ...      // the first controller's player
    //     if (input.pressed("pause")) ...        // anyone, on any device
    //
    // The questions without a player (held(action), axis(name)) and with
    // player -1 ask about anyone: every device, the keyboard's and every
    // controller's, owned or not, with the Input's map, and each player with
    // their own map.
    void set_players(std::vector<InputPlayer> players);
    const std::vector<InputPlayer>& players() const { return _players; }
    // A player joins at the end, or leaves (those after it move down one):
    // the number of the one added (0 when it is the first).
    i32 add_player(InputPlayer player);
    void remove_player(i32 player);
    // 1 with no players set (everyone is player 0).
    i32 player_count() const { return _players.empty() ? 1 : static_cast<i32>(_players.size()); }
    // The player a controller is given to (-1: nobody's, or nobody is set).
    i32 player_of_gamepad(i32 slot) const;
    // Whether the player has something to play with: the keyboard, or a
    // controller plugged in (false: show "reconnect your controller").
    bool player_connected(i32 player) const;

    bool pressed(i32 player, std::string_view action) const;
    bool held(i32 player, std::string_view action) const;
    bool released(i32 player, std::string_view action) const;
    bool frame_pressed(i32 player, std::string_view action) const;
    bool frame_released(i32 player, std::string_view action) const;
    f32 axis(i32 player, std::string_view name) const;

    // What the player (-1: anyone) last pressed something on: the keyboard,
    // the mouse or a controller (before anything: the keyboard if they have
    // it). And their controller pressed last (none yet: their first plugged
    // in; -1: they have none plugged in).
    InputDevice last_device(i32 player = -1) const;
    i32 last_gamepad(i32 player = -1) const;

    // Game controllers' states by slot (0 to max_gamepads - 1), as App passes
    // them each frame when it opens controllers (App::open_gamepads), or as a
    // test or a replay sets them: what actions and axes bound to a controller
    // read. Without a slot: slot 0.
    void set_gamepad(const GamepadState& state) { set_gamepad(0, state); }
    void set_gamepad(i32 slot, const GamepadState& state);
    const GamepadState& gamepad(i32 slot = 0) const;
    // A controller came into `slot`, or left it, since a fixed step last read
    // it (edges that last like pressed()'s).
    bool gamepad_plugged(i32 slot) const;
    bool gamepad_unplugged(i32 slot) const;
    // The lowest slot whose `control` was pressed (as pressed()), or -1;
    // `free_only`: only controllers no player has ("press A to join").
    i32 gamepad_pressed(PadControl control, bool free_only = false) const;

    // Shakes the controller the player (-1: anyone) last used, `low` (the
    // heavy motor) and `high` (the light buzz) 0 to 1, for `seconds`; or a slot's.
    // Asked here and done by App (take_rumble), next frame.
    void rumble(i32 player, f32 low, f32 high, f32 seconds);
    void rumble_gamepad(i32 slot, f32 low, f32 high, f32 seconds);
    std::vector<GamepadRumble> take_rumble() { return std::exchange(_rumble, {}); }

private:
    friend class App;
    // Test-only access to the step-aware begin_frame(bool); the public begin_frame()
    // always advances, so tests need this seam to emulate 0-step (advance=false) frames.
    friend struct InputFrameTestHook;

    static constexpr i32 KEY_COUNT = 512;
    static constexpr i32 MOUSE_BUTTON_COUNT = 6;
    static constexpr f32 DRAG_THRESHOLD = 4.0f;
    static constexpr std::size_t pad_control_count = static_cast<std::size_t>(PadControl::Count);

    // Buttons of one device (keys, mouse buttons, a controller's controls)
    // and their edges, held and changed the same way for all of them.
    enum class Edge : u8 { Held, Pressed, Released, FramePressed, FrameReleased };
    template <std::size_t N>
    struct Buttons {
        std::array<bool, N> cur{};
        std::array<bool, N> prev{};
        std::array<bool, N> pressed{};        // sticky: until a fixed step reads it
        std::array<bool, N> released{};
        std::array<bool, N> frame_pressed{};  // until the next rendered frame
        std::array<bool, N> frame_released{};

        // Held now or not, with an edge when that changed.
        void set(std::size_t i, bool now) {
            if (now && !cur[i]) {
                pressed[i] = true;
                frame_pressed[i] = true;
            } else if (!now && cur[i]) {
                released[i] = true;
                frame_released[i] = true;
            }
            cur[i] = now;
        }
        // A press edge even if it was held already (tests, driven input).
        void press(std::size_t i) {
            prev[i] = false;
            cur[i] = true;
            pressed[i] = true;
            frame_pressed[i] = true;
        }
        bool test(std::size_t i, Edge edge) const {
            switch (edge) {
            case Edge::Held: return cur[i];
            case Edge::Pressed: return pressed[i] || (cur[i] && !prev[i]);
            case Edge::Released: return released[i] || (!cur[i] && prev[i]);
            case Edge::FramePressed: return frame_pressed[i];
            case Edge::FrameReleased: return frame_released[i];
            }
            return false;
        }
        void advance_step() {
            prev = cur;
            pressed.fill(false);
            released.fill(false);
        }
        void clear_frame() {
            frame_pressed.fill(false);
            frame_released.fill(false);
        }
    };

    // The devices a question reads and the map it reads them with.
    struct Reach {
        bool keyboard = false;
        u32 gamepads = 0; // bit per slot
        const InputMap* map = nullptr;
    };
    Reach reach_of(const InputPlayer& player) const;
    Reach everything() const;
    bool binding_edge(const Reach& reach, InputBinding binding, Edge edge) const;
    bool map_edge(const Reach& reach, std::string_view action, Edge edge) const;
    bool action_edge(i32 player, std::string_view action, Edge edge) const;
    f32 reach_axis(const Reach& reach, const InputAxis& axis) const;
    f32 player_axis(i32 player, std::string_view name) const;
    friend bool binding_held(const Input&, InputBinding, i32);
    friend f32 axis_value(const Input&, const InputAxis&, i32);

    void begin_frame(bool advance_transients);
    void process_native_event(const void* event);

    Buttons<KEY_COUNT> _keys;
    std::array<bool, KEY_COUNT> _key_frame_repeated = {};

    Buttons<MOUSE_BUTTON_COUNT> _mouse;
    // Press edges claimed by a widget this frame: cleared at the next begin_frame even
    // on a 0-step frame, so a claimed press does not re-fire while it persists.
    std::array<bool, MOUSE_BUTTON_COUNT> _mouse_frame_pressed_consumed = {};
    std::array<Vec2f, MOUSE_BUTTON_COUNT> _drag_start = {};

    Vec2f _mouse_pos = {};
    Vec2f _mouse_pos_prev = {};
    WindowId _mouse_window = 0;
    WindowId _keyboard_window = 0;
    f32 _mouse_wheel_y = 0.0f;       // since a fixed step last read it
    f32 _frame_mouse_wheel_y = 0.0f; // since the last rendered frame
    f32 _mouse_wheel_x = 0.0f;
    f32 _frame_mouse_wheel_x = 0.0f;
    Vec2f _mouse_motion{};           // since a fixed step last read it
    Vec2f _frame_mouse_motion{};     // since the last rendered frame
    u64 _last_key_press_event_time_ns = 0;
    u64 _last_mouse_motion_event_time_ns = 0;
    u64 _last_key_press_detected_time_ns = 0;
    std::string _text_input;
    std::string _composition;
    i32 _composition_cursor = 0;
    std::vector<DroppedFile> _dropped_files;
    std::optional<Vec2f> _drop_position;
    std::string _clipboard_text;

    std::unordered_map<WindowId, Vec2f> _window_mouse_pos;
    InputMap _map;
    std::vector<InputPlayer> _players;

    // The controllers by slot: as last set, and each control as a button.
    std::array<GamepadState, max_gamepads> _pad_states{};
    std::array<Buttons<pad_control_count>, max_gamepads> _pads{};
    std::array<bool, max_gamepads> _pad_plugged{};   // sticky, as pressed
    std::array<bool, max_gamepads> _pad_unplugged{};
    std::vector<GamepadRumble> _rumble;

    // When each device was last pressed (a count of presses), for last_device().
    u64 _presses = 0;
    u64 _keyboard_pressed_at = 0;
    u64 _mouse_pressed_at = 0;
    std::array<u64, max_gamepads> _pad_pressed_at{};
    void note_keyboard() { _keyboard_pressed_at = ++_presses; }
    void note_mouse() { _mouse_pressed_at = ++_presses; }
};

} // namespace kin
