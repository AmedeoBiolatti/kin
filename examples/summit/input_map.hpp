#pragma once

// Summit's input mapper (docs/3d_plan.md, P4a and P5a): raw device state in, a PlayerCommand out. Nothing else in the game reads a
// device. A rebinding table, dead zones and sensitivities live here; the simulation only ever sees the command.
//
//     summit::RawInput raw = summit::read_raw_input(input, pad);       // once a fixed step
//     const summit::PlayerCommand c = summit::map_input(raw, bindings);
//
// RawInput is plain data (a bitset of held keys, the mouse's motion, a GamepadState), so a test can make one, a recording can
// keep it, and the same table maps a keyboard, a mouse and a controller: a stick pushed fully forward and the W key are the same
// command.

#include "game.hpp"

#include <kin/platform/gamepad.hpp>
#include <kin/platform/input.hpp>

#include <array>
#include <bitset>
#include <string>
#include <vector>

namespace summit {
using namespace kin;

struct RawInput {
    std::bitset<128> keys;   // by kin::Key
    std::bitset<8> mouse;    // by kin::MouseButton
    Vec2f mouse_delta{};     // pixels since the last step (x right, y down)
    GamepadState pad{};

    bool key(Key k) const { return keys.test(static_cast<std::size_t>(k)); }
    RawInput& set_key(Key k, bool down = true) {
        keys.set(static_cast<std::size_t>(k), down);
        return *this;
    }
    bool button(MouseButton b) const { return mouse.test(static_cast<std::size_t>(b)); }
    RawInput& set_button(MouseButton b, bool down = true) {
        mouse.set(static_cast<std::size_t>(b), down);
        return *this;
    }
    friend bool operator==(const RawInput&, const RawInput&) = default;
};

// What is held now (keys and buttons), and the mouse's motion since a fixed step last read it (Input::mouse_motion).
RawInput read_raw_input(const Input& input, const GamepadState* pad = nullptr);

enum class Action : u8 { Forward, Back, Left, Right, Jump, Sprint, GrabLeft, GrabRight, Release, PullUp, Interact, Count };
inline constexpr std::size_t action_count = static_cast<std::size_t>(Action::Count);
const char* action_name(Action a);

struct Binding {
    enum class Device : u8 { Key, Mouse, PadButton, PadTrigger };
    Device device = Device::Key;
    // Key: a kin::Key; Mouse: a kin::MouseButton; PadButton: a GamepadButton; PadTrigger: 0 left, 1 right.
    i32 code = 0;

    static Binding key(Key k) { return {Device::Key, static_cast<i32>(k)}; }
    static Binding mouse(MouseButton b) { return {Device::Mouse, static_cast<i32>(b)}; }
    static Binding pad(GamepadButton b) { return {Device::PadButton, static_cast<i32>(b)}; }
    static Binding trigger(bool right) { return {Device::PadTrigger, right ? 1 : 0}; }
    friend bool operator==(const Binding&, const Binding&) = default;
};

struct InputBindings {
    std::array<std::vector<Binding>, action_count> table;
    // Sticks: inside this radius nothing, outside it the rest of the travel is 0 to 1.
    f32 dead_zone = 0.18f;
    // A trigger counts as a button past this.
    f32 trigger_threshold = 0.5f;
    // Mouse: degrees a pixel. Stick: degrees a second at full deflection.
    f32 mouse_sensitivity = 0.10f;
    f32 stick_look_speed = 150.0f;
    bool invert_y = false;

    static InputBindings defaults();
    // Replaces what the action was bound to.
    void set(Action a, std::vector<Binding> bindings) { table[static_cast<std::size_t>(a)] = std::move(bindings); }
    // Adds one (rebinding with set() first is the usual way; two keys for one action are allowed).
    void add(Action a, Binding b) { table[static_cast<std::size_t>(a)].push_back(b); }
    void clear(Action a) { table[static_cast<std::size_t>(a)].clear(); }
    const std::vector<Binding>& bindings(Action a) const { return table[static_cast<std::size_t>(a)]; }
    // "forward = key:W key:Up" a line each, then the numbers; from_text returns false (and changes nothing) for a name it does
    // not know.
    std::string to_text() const;
    bool from_text(const std::string& text);
};

// A stick through a radial dead zone: inside `dead_zone` zero, outside it rescaled so that full travel is still 1.
Vec2f apply_dead_zone(Vec2f stick, f32 dead_zone);

// One step's command from the device state (`dt` is the step: the sticks turn the view at degrees a second).
PlayerCommand map_input(const RawInput& raw, const InputBindings& bindings, f32 dt = fixed_step);

} // namespace summit
