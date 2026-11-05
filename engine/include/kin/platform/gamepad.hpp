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
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kin {

enum class GamepadFamily { Generic, Xbox, PlayStation, Nintendo };

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
    u32 device = 0; // connection identity; zero for synthetic/legacy input
    GamepadFamily family = GamepadFamily::Generic;

    bool button(GamepadButton b) const { return (buttons >> static_cast<u32>(b)) & 1u; }
    void set_button(GamepadButton b, bool down) {
        const u32 bit = 1u << static_cast<u32>(b);
        buttons = down ? (buttons | bit) : (buttons & ~bit);
    }
    friend bool operator==(const GamepadState&, const GamepadState&) = default;
};

struct GamepadDevice {
    u32 id = 0;
    std::string name;
    std::string identity; // GUID + serial, for reconnect matching
    GamepadState state;
};

std::string_view gamepad_button_label(GamepadFamily family, GamepadButton button);

// Deterministic device ownership, usable with hardware inventories or replayed
// inventories. A disconnected player's slot remains reserved until leave().
class GamepadPlayers {
public:
    explicit GamepadPlayers(std::size_t max_players = 4);
    void update(std::span<const GamepadDevice> devices, bool join_on_start = true);
    bool join(u32 device, std::size_t player);
    void leave(std::size_t player);
    const GamepadState& state(std::size_t player) const;
    u32 device(std::size_t player) const;
    bool joined(std::size_t player) const;
    std::size_t capacity() const { return _players.size(); }

private:
    struct Player {
        u32 device = 0;
        std::string identity;
        GamepadState state;
        bool joined = false;
    };
    // What the last update() saw of a device: whether it held Start.
    struct Seen {
        u32 id = 0;
        bool start = false;
    };
    std::vector<Player> _players;
    std::vector<GamepadDevice> _devices;
    std::vector<Seen> _previous;        // the update before
    std::vector<u32> _reserved, _owned; // update()'s scratch
};

// Reads the connected controllers: player 1's, else the first, as poll()'s state. Starts SDL's gamepad subsystem when
// made and stops it when destroyed; where there is none (or the subsystem cannot start) poll() reports `connected ==
// false`.
class Gamepads {
public:
    Gamepads();
    ~Gamepads();
    Gamepads(const Gamepads&) = delete;
    Gamepads& operator=(const Gamepads&) = delete;

    // The state now. Call it once a frame or a fixed step; it looks for newly connected controllers about once a second
    // (every 60 polls), and lets go of disconnected ones at once.
    const GamepadState& poll();
    // The device's name ("" with none).
    const char* name() const;
    std::span<const GamepadDevice> devices() const;
    GamepadPlayers& players();
    // Whether a fresh Start press joins its controller to the first free player slot (on by default); off, only
    // players().join() assigns them.
    void set_join_on_start(bool join);
    // Starts both motors for duration_ms; zero duration stops. False when the
    // device is disconnected, lacks rumble, or the strengths are non-finite.
    bool rumble(u32 device, f32 low, f32 high, u32 duration_ms);
    bool rumble_player(std::size_t player, f32 low, f32 high, u32 duration_ms);

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace kin
