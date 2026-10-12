#pragma once

// A game controller's state, polled (docs/3d_plan.md, P4a). Opt-in: nothing in kin opens a controller until a `Gamepads` is made,
// and `Input` is unchanged. The state is plain data, so that a game's input mapper (which turns device state into commands) can be
// tested with a made-up one and a recording or a network layer can carry it.
//
//     kin::Gamepads pads;                          // opens the first controller SDL finds, now and when one is plugged in
//     ...each frame (after the window's events are pumped):
//     const kin::GamepadState& pad = pads.poll();  // connected false when there is none
//     move = {pad.left_x, pad.left_y};             // -1 to 1, y up is positive (forward)

#include <kin/core/types.hpp>

#include <memory>

namespace kin {

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

// Reads the first connected controller. Starts SDL's gamepad subsystem when made and stops it when destroyed; where there is
// none (or the subsystem cannot start) poll() reports `connected == false` and nothing else.
class Gamepads {
public:
    Gamepads();
    ~Gamepads();
    Gamepads(const Gamepads&) = delete;
    Gamepads& operator=(const Gamepads&) = delete;

    // The state now. Call it once a frame or a fixed step; it looks for a controller about once a second while there is none.
    const GamepadState& poll();
    // The device's name ("" with none).
    const char* name() const;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace kin
