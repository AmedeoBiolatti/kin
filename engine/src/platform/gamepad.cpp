#include <kin/platform/gamepad.hpp>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace kin {

namespace {

// How often (in polls) to look for controllers plugged in: a few times a second at 60 Hz.
constexpr u32 look_every = 15;

f32 axis(SDL_Gamepad* pad, SDL_GamepadAxis a) {
    const f32 v = static_cast<f32>(SDL_GetGamepadAxis(pad, a));
    return std::clamp(v / (v < 0.0f ? 32768.0f : 32767.0f), -1.0f, 1.0f);
}

bool same_guid(const SDL_GUID& a, const SDL_GUID& b) {
    return std::memcmp(a.data, b.data, sizeof(a.data)) == 0;
}

GamepadState read(SDL_Gamepad* pad) {
    GamepadState g;
    g.connected = true;
    g.left_x = axis(pad, SDL_GAMEPAD_AXIS_LEFTX);
    g.left_y = -axis(pad, SDL_GAMEPAD_AXIS_LEFTY);
    g.right_x = axis(pad, SDL_GAMEPAD_AXIS_RIGHTX);
    g.right_y = -axis(pad, SDL_GAMEPAD_AXIS_RIGHTY);
    g.left_trigger = std::max(0.0f, axis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
    g.right_trigger = std::max(0.0f, axis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
    const struct {
        SDL_GamepadButton sdl;
        GamepadButton ours;
    } map[] = {{SDL_GAMEPAD_BUTTON_SOUTH, GamepadButton::A},
               {SDL_GAMEPAD_BUTTON_EAST, GamepadButton::B},
               {SDL_GAMEPAD_BUTTON_WEST, GamepadButton::X},
               {SDL_GAMEPAD_BUTTON_NORTH, GamepadButton::Y},
               {SDL_GAMEPAD_BUTTON_BACK, GamepadButton::Back},
               {SDL_GAMEPAD_BUTTON_START, GamepadButton::Start},
               {SDL_GAMEPAD_BUTTON_LEFT_STICK, GamepadButton::LeftStick},
               {SDL_GAMEPAD_BUTTON_RIGHT_STICK, GamepadButton::RightStick},
               {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, GamepadButton::LeftShoulder},
               {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, GamepadButton::RightShoulder},
               {SDL_GAMEPAD_BUTTON_DPAD_UP, GamepadButton::DPadUp},
               {SDL_GAMEPAD_BUTTON_DPAD_DOWN, GamepadButton::DPadDown},
               {SDL_GAMEPAD_BUTTON_DPAD_LEFT, GamepadButton::DPadLeft},
               {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, GamepadButton::DPadRight}};
    for (const auto& m : map) g.set_button(m.ours, SDL_GetGamepadButton(pad, m.sdl));
    return g;
}

} // namespace

struct Gamepads::Impl {
    // A slot: the controller in it, or the one it had (`guid`, `had`) to give it back to.
    struct Slot {
        SDL_Gamepad* pad = nullptr;
        SDL_JoystickID id = 0;
        SDL_GUID guid{};
        bool had = false;
        std::string name;
    };
    std::array<Slot, max_gamepads> slots{};
    std::array<GamepadState, max_gamepads> states{};
    bool subsystem = false;
    u32 polls = 0;

    static void close(Slot& slot) {
        if (slot.pad != nullptr) {
            SDL_CloseGamepad(slot.pad);
            slot.pad = nullptr;
        }
        slot.id = 0;
        slot.name.clear();
    }

    bool open_already(SDL_JoystickID id) const {
        return std::ranges::any_of(slots, [id](const Slot& s) { return s.pad != nullptr && s.id == id; });
    }

    // Where a controller goes: back to the free slot that last had one like it, or the lowest free slot never used, or any free one.
    Slot* slot_for(const SDL_GUID& guid) {
        for (Slot& s : slots) {
            if (s.pad == nullptr && s.had && same_guid(s.guid, guid)) return &s;
        }
        for (Slot& s : slots) {
            if (s.pad == nullptr && !s.had) return &s;
        }
        for (Slot& s : slots) {
            if (s.pad == nullptr) return &s;
        }
        return nullptr;
    }

    void look() {
        int count = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&count);
        if (ids == nullptr) return;
        for (int i = 0; i < count; ++i) {
            if (open_already(ids[i])) continue;
            const SDL_GUID guid = SDL_GetGamepadGUIDForID(ids[i]);
            Slot* slot = slot_for(guid);
            if (slot == nullptr) break; // all taken
            SDL_Gamepad* pad = SDL_OpenGamepad(ids[i]);
            if (pad == nullptr) continue;
            slot->pad = pad;
            slot->id = ids[i];
            slot->guid = guid;
            slot->had = true;
            const char* n = SDL_GetGamepadName(pad);
            slot->name = n != nullptr ? n : "";
        }
        SDL_free(ids);
    }
};

Gamepads::Gamepads() : _impl(std::make_unique<Impl>()) {
    _impl->subsystem = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
}

Gamepads::~Gamepads() {
    for (Impl::Slot& slot : _impl->slots) Impl::close(slot);
    if (_impl->subsystem) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}

const char* Gamepads::name(i32 slot) const {
    if (slot < 0 || slot >= max_gamepads) return "";
    const Impl::Slot& s = _impl->slots[static_cast<std::size_t>(slot)];
    return s.pad != nullptr ? s.name.c_str() : "";
}

const char* Gamepads::name() const {
    for (const Impl::Slot& s : _impl->slots) {
        if (s.pad != nullptr) return s.name.c_str();
    }
    return "";
}

i32 Gamepads::connected_count() const {
    return static_cast<i32>(std::ranges::count_if(_impl->slots, [](const Impl::Slot& s) { return s.pad != nullptr; }));
}

const std::array<GamepadState, max_gamepads>& Gamepads::poll_all() {
    Impl& s = *_impl;
    if (!s.subsystem) return s.states;
    SDL_UpdateGamepads();
    for (Impl::Slot& slot : s.slots) {
        if (slot.pad != nullptr && !SDL_GamepadConnected(slot.pad)) Impl::close(slot);
    }
    if (s.polls++ % look_every == 0) s.look();
    for (std::size_t i = 0; i < s.slots.size(); ++i) {
        s.states[i] = s.slots[i].pad != nullptr ? read(s.slots[i].pad) : GamepadState{};
    }
    return s.states;
}

const GamepadState& Gamepads::poll() {
    const std::array<GamepadState, max_gamepads>& all = poll_all();
    for (const GamepadState& state : all) {
        if (state.connected) return state;
    }
    return all[0];
}

bool Gamepads::rumble(i32 slot, f32 low, f32 high, f32 seconds) {
    if (slot < 0 || slot >= max_gamepads) return false;
    SDL_Gamepad* pad = _impl->slots[static_cast<std::size_t>(slot)].pad;
    if (pad == nullptr) return false;
    const auto strength = [](f32 v) { return static_cast<Uint16>(std::lround(std::clamp(v, 0.0f, 1.0f) * 65535.0f)); };
    const auto ms = static_cast<Uint32>(std::lround(std::clamp(seconds, 0.0f, 60.0f) * 1000.0f));
    return SDL_RumbleGamepad(pad, strength(low), strength(high), ms);
}

} // namespace kin
