#pragma once

// Game controllers' state, polled (docs/input.md). Opt-in: nothing in kin opens a controller until a `Gamepads` is made (App::open_gamepads
// makes one), and the state is plain data, so that a game's input mapper (which turns device state into commands) can be tested with a
// made-up one and a recording or a network layer can carry it.
//
//     kin::Gamepads pads;                          // opens every controller SDL finds, now and when one is plugged in
//     ...each frame (after the window's events are pumped):
//     const auto& all = pads.poll_all();           // by slot; connected false where there is none
//     move = {all[0].left_x, all[0].left_y};       // -1 to 1, y up is positive (forward)
//
// Each controller has a slot (0 to max_gamepads - 1) while it is plugged in: the lowest free one when it comes, or the one it had last
// time when it comes back (a cable pulled out by mistake keeps its player's place). Slots are what Input and its players name.

#include <kin/core/types.hpp>

#include <array>
#include <memory>

namespace kin {

// As many controllers as are read at once (local multiplayer seldom has more than four).
inline constexpr i32 max_gamepads = 8;

enum class GamepadButton : u8 {
    A, // the south face button (Cross)
    B, // east (Circle)
    X, // west (Square)
    Y, // north (Triangle)
    Back,
    Start,
    LeftStick,  // pressed in
    RightStick,
    LeftShoulder,
    RightShoulder,
    DPadUp,
    DPadDown,
    DPadLeft,
    DPadRight,
    Count,
};

struct GamepadState {
    bool connected = false;
    // Sticks, -1 to 1 (x right, y up), as the device reports them: no dead zone is applied here.
    f32 left_x = 0.0f, left_y = 0.0f;
    f32 right_x = 0.0f, right_y = 0.0f;
    // Triggers, 0 to 1.
    f32 left_trigger = 0.0f, right_trigger = 0.0f;
    // One bit for each GamepadButton.
    u32 buttons = 0;

    bool button(GamepadButton b) const { return (buttons >> static_cast<u32>(b)) & 1u; }
    void set_button(GamepadButton b, bool down) {
        const u32 bit = 1u << static_cast<u32>(b);
        buttons = down ? (buttons | bit) : (buttons & ~bit);
    }
    friend bool operator==(const GamepadState&, const GamepadState&) = default;
};

// A rumble asked for: both motors, 0 to 1 (`low`: the heavy one, `high`: the light buzz), for `seconds`. A new one replaces the one going.
struct GamepadRumble {
    i32 slot = 0;
    f32 low = 0.0f;
    f32 high = 0.0f;
    f32 seconds = 0.0f;
    friend bool operator==(const GamepadRumble&, const GamepadRumble&) = default;
};

// Reads every connected controller, each in its slot. Starts SDL's gamepad subsystem when made and stops it when destroyed; where there
// is none (or the subsystem cannot start) every slot reports `connected == false` and nothing else.
class Gamepads {
public:
    Gamepads();
    ~Gamepads();
    Gamepads(const Gamepads&) = delete;
    Gamepads& operator=(const Gamepads&) = delete;

    // Every slot's state now. Call it once a frame or a fixed step; it looks for newly plugged controllers a few times a second.
    const std::array<GamepadState, max_gamepads>& poll_all();
    // The same, given back as the first connected controller's state (none: a disconnected one): for a one-player game.
    const GamepadState& poll();
    // A slot's device name ("" with none there); without a slot, the first connected one's.
    const char* name(i32 slot) const;
    const char* name() const;
    i32 connected_count() const;
    // Shakes the controller in `slot` (false: none there, or it cannot rumble).
    bool rumble(i32 slot, f32 low, f32 high, f32 seconds);
    bool rumble(const GamepadRumble& rumble) { return this->rumble(rumble.slot, rumble.low, rumble.high, rumble.seconds); }

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace kin
