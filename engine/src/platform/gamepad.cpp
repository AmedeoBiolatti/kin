#include <kin/platform/gamepad.hpp>

#include <SDL3/SDL.h>

#include <algorithm>
#include <string>

namespace kin {

struct Gamepads::Impl {
    SDL_Gamepad* pad = nullptr;
    bool subsystem = false;
    u32 polls = 0;
    GamepadState state;
    std::string name;

    void close() {
        if (pad != nullptr) {
            SDL_CloseGamepad(pad);
            pad = nullptr;
        }
        name.clear();
        state = {};
    }
    void look() {
        int count = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&count);
        if (ids != nullptr) {
            if (count > 0) {
                pad = SDL_OpenGamepad(ids[0]);
                if (pad != nullptr) {
                    const char* n = SDL_GetGamepadName(pad);
                    name = n != nullptr ? n : "";
                }
            }
            SDL_free(ids);
        }
    }
};

namespace {
f32 axis(SDL_Gamepad* pad, SDL_GamepadAxis a) {
    const f32 v = static_cast<f32>(SDL_GetGamepadAxis(pad, a));
    return std::clamp(v / (v < 0.0f ? 32768.0f : 32767.0f), -1.0f, 1.0f);
}
} // namespace

Gamepads::Gamepads() : _impl(std::make_unique<Impl>()) {
    _impl->subsystem = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
}

Gamepads::~Gamepads() {
    _impl->close();
    if (_impl->subsystem) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}

const char* Gamepads::name() const { return _impl->name.c_str(); }

const GamepadState& Gamepads::poll() {
    Impl& s = *_impl;
    if (!s.subsystem) return s.state;
    if (s.pad != nullptr && !SDL_GamepadConnected(s.pad)) s.close();
    if (s.pad == nullptr && s.polls++ % 60 == 0) s.look();
    if (s.pad == nullptr) return s.state;
    SDL_UpdateGamepads();
    GamepadState g;
    g.connected = true;
    g.left_x = axis(s.pad, SDL_GAMEPAD_AXIS_LEFTX);
    g.left_y = -axis(s.pad, SDL_GAMEPAD_AXIS_LEFTY);
    g.right_x = axis(s.pad, SDL_GAMEPAD_AXIS_RIGHTX);
    g.right_y = -axis(s.pad, SDL_GAMEPAD_AXIS_RIGHTY);
    g.left_trigger = std::max(0.0f, axis(s.pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
    g.right_trigger = std::max(0.0f, axis(s.pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
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
    for (const auto& m : map) g.set_button(m.ours, SDL_GetGamepadButton(s.pad, m.sdl));
    s.state = g;
    return s.state;
}

} // namespace kin
