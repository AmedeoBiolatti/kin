#include <kin/platform/gamepad.hpp>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace kin {
namespace {

f32 axis(SDL_Gamepad* pad, SDL_GamepadAxis a) {
    const f32 value = static_cast<f32>(SDL_GetGamepadAxis(pad, a));
    return std::clamp(value / (value < 0 ? 32768.0f : 32767.0f), -1.0f, 1.0f);
}

GamepadFamily family(SDL_Gamepad* pad) {
    switch (SDL_GetGamepadType(pad)) {
    case SDL_GAMEPAD_TYPE_XBOX360:
    case SDL_GAMEPAD_TYPE_XBOXONE:
        return GamepadFamily::Xbox;
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5:
        return GamepadFamily::PlayStation;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
    case SDL_GAMEPAD_TYPE_GAMECUBE:
        return GamepadFamily::Nintendo;
    default:
        return GamepadFamily::Generic;
    }
}

GamepadState read(SDL_Gamepad* pad, u32 id) {
    GamepadState result;
    result.connected = true;
    result.device = id;
    result.family = family(pad);
    result.left_x = axis(pad, SDL_GAMEPAD_AXIS_LEFTX);
    result.left_y = -axis(pad, SDL_GAMEPAD_AXIS_LEFTY);
    result.right_x = axis(pad, SDL_GAMEPAD_AXIS_RIGHTX);
    result.right_y = -axis(pad, SDL_GAMEPAD_AXIS_RIGHTY);
    result.left_trigger = std::max(0.0f, axis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
    result.right_trigger = std::max(0.0f, axis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
    constexpr SDL_GamepadButton buttons[] = {
        SDL_GAMEPAD_BUTTON_SOUTH,          SDL_GAMEPAD_BUTTON_EAST,        SDL_GAMEPAD_BUTTON_WEST,
        SDL_GAMEPAD_BUTTON_NORTH,          SDL_GAMEPAD_BUTTON_BACK,        SDL_GAMEPAD_BUTTON_START,
        SDL_GAMEPAD_BUTTON_LEFT_STICK,     SDL_GAMEPAD_BUTTON_RIGHT_STICK, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
        SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_DPAD_UP,     SDL_GAMEPAD_BUTTON_DPAD_DOWN,
        SDL_GAMEPAD_BUTTON_DPAD_LEFT,      SDL_GAMEPAD_BUTTON_DPAD_RIGHT};
    for (u32 i = 0; i < static_cast<u32>(GamepadButton::Count); ++i) {
        result.set_button(static_cast<GamepadButton>(i), SDL_GetGamepadButton(pad, buttons[i]));
    }
    return result;
}

} // namespace

std::string_view gamepad_button_label(GamepadFamily family, GamepadButton button) {
    const auto index = static_cast<std::size_t>(button);
    constexpr std::string_view generic[] = {"A",
                                            "B",
                                            "X",
                                            "Y",
                                            "Back",
                                            "Start",
                                            "Left Stick",
                                            "Right Stick",
                                            "Left Shoulder",
                                            "Right Shoulder",
                                            "D-pad Up",
                                            "D-pad Down",
                                            "D-pad Left",
                                            "D-pad Right"};
    constexpr std::string_view sony[] = {"Cross", "Circle", "Square", "Triangle"};
    constexpr std::string_view nintendo[] = {"B", "A", "Y", "X"};
    if (index >= static_cast<std::size_t>(GamepadButton::Count)) {
        return {};
    }
    if (index < 4 && family == GamepadFamily::PlayStation) {
        return sony[index];
    }
    if (index < 4 && family == GamepadFamily::Nintendo) {
        return nintendo[index];
    }
    return generic[index];
}

GamepadPlayers::GamepadPlayers(std::size_t max_players) : _players(max_players) {
    if (max_players == 0 || max_players > 64) {
        throw std::invalid_argument("player capacity must be 1..64");
    }
}

bool GamepadPlayers::join(u32 id, std::size_t slot) {
    if (!id || slot >= _players.size()) {
        return false;
    }
    const auto found = std::ranges::find(_devices, id, &GamepadDevice::id);
    if (found == _devices.end() || !found->state.connected) {
        return false;
    }
    if (std::ranges::any_of(_players, [&](const auto& player) { return player.joined && player.device == id; })) {
        return false;
    }
    if (_players[slot].joined) {
        return false;
    }
    _players[slot] = {id, found->identity, found->state, true};
    return true;
}

void GamepadPlayers::leave(std::size_t player) {
    if (player < _players.size()) {
        _players[player] = {};
    }
}

void GamepadPlayers::update(std::span<const GamepadDevice> devices, bool join_on_start) {
    _previous.clear();
    for (const auto& device : _devices) {
        _previous.push_back({device.id, device.state.button(GamepadButton::Start)});
    }
    _devices.assign(devices.begin(), devices.end()); // assigned in place: the strings keep their storage
    const auto connected = [&](u32 id) -> const GamepadDevice* {
        const auto found = std::ranges::find(_devices, id, &GamepadDevice::id);
        return found != _devices.end() && found->state.connected ? &*found : nullptr;
    };
    const auto previous = [&](u32 id) { return std::ranges::find(_previous, id, &Seen::id); };
    const auto listed = [](const std::vector<u32>& ids, u32 id) { return std::ranges::find(ids, id) != ids.end(); };
    _reserved.clear();
    _owned.clear();
    for (const auto& player : _players) {
        if (player.joined && connected(player.device)) {
            _reserved.push_back(player.device);
        }
    }
    for (auto& player : _players) {
        if (!player.joined) {
            continue;
        }
        player.state = {};
        const GamepadDevice* current = connected(player.device);
        if (!current && !player.identity.empty()) {
            // Back under a new id: the one newly seen device with its identity that nobody holds.
            const GamepadDevice* match = nullptr;
            std::size_t matches = 0;
            for (const auto& device : _devices) {
                if (device.identity == player.identity && device.state.connected && !listed(_reserved, device.id) &&
                    previous(device.id) == _previous.end()) {
                    match = &device;
                    ++matches;
                }
            }
            if (matches == 1) {
                player.device = match->id;
                _reserved.push_back(player.device);
                current = match;
            }
        }
        if (current && !listed(_owned, current->id)) {
            player.state = current->state;
            player.state.device = current->id; // replayed inventories may leave it 0
            _owned.push_back(current->id);
        }
    }
    if (!join_on_start) {
        return;
    }
    for (const auto& device : _devices) {
        const auto before = previous(device.id);
        if (!device.state.connected || listed(_owned, device.id) || !device.state.button(GamepadButton::Start) ||
            (before != _previous.end() && before->start)) {
            continue;
        }
        for (std::size_t slot = 0; slot < _players.size(); ++slot) {
            if (!_players[slot].joined) {
                if (join(device.id, slot)) {
                    _owned.push_back(device.id);
                }
                break;
            }
        }
    }
}

const GamepadState& GamepadPlayers::state(std::size_t player) const {
    static const GamepadState empty;
    return player < _players.size() ? _players[player].state : empty;
}

u32 GamepadPlayers::device(std::size_t player) const {
    return player < _players.size() ? _players[player].device : 0;
}

bool GamepadPlayers::joined(std::size_t player) const {
    return player < _players.size() && _players[player].joined;
}

struct Gamepads::Impl {
    // Open controllers by SDL id, beside `devices` (same order, ascending id);
    // a device's name and identity are read once, when it opens.
    std::vector<SDL_Gamepad*> handles;
    std::vector<GamepadDevice> devices;
    GamepadPlayers players;
    GamepadState primary;
    u32 polls = 0;
    bool subsystem = false;
    bool join_on_start = true;

    ~Impl() {
        for (SDL_Gamepad* handle : handles) {
            SDL_CloseGamepad(handle);
        }
        if (subsystem) {
            SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        }
    }

    void close_disconnected() {
        for (std::size_t i = 0; i < handles.size();) {
            if (SDL_GamepadConnected(handles[i])) {
                ++i;
                continue;
            }
            SDL_CloseGamepad(handles[i]);
            handles.erase(handles.begin() + static_cast<std::ptrdiff_t>(i));
            devices.erase(devices.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }

    void open_new() {
        int count = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&count);
        for (int i = 0; ids && i < count; ++i) {
            const u32 id = ids[i];
            const auto at = std::ranges::lower_bound(devices, id, {}, &GamepadDevice::id);
            if (at != devices.end() && at->id == id) {
                continue;
            }
            SDL_Gamepad* pad = SDL_OpenGamepad(id);
            if (!pad) {
                continue;
            }
            char guid[33];
            SDL_GUIDToString(SDL_GetJoystickGUID(SDL_GetGamepadJoystick(pad)), guid, sizeof(guid));
            const char* serial = SDL_GetGamepadSerial(pad);
            const char* name = SDL_GetGamepadName(pad);
            const auto index = at - devices.begin();
            handles.insert(handles.begin() + index, pad);
            devices.insert(at, {id, name ? name : "", std::string(guid) + ":" + (serial ? serial : ""), {}});
        }
        SDL_free(ids);
    }
};

Gamepads::Gamepads() : _impl(std::make_unique<Impl>()) {
    _impl->subsystem = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
}

Gamepads::~Gamepads() = default;

const char* Gamepads::name() const {
    const auto found = std::ranges::find(_impl->devices, _impl->primary.device, &GamepadDevice::id);
    return _impl->primary.connected && found != _impl->devices.end() ? found->name.c_str() : "";
}

std::span<const GamepadDevice> Gamepads::devices() const {
    return _impl->devices;
}

GamepadPlayers& Gamepads::players() {
    return _impl->players;
}

void Gamepads::set_join_on_start(bool join) {
    _impl->join_on_start = join;
}

const GamepadState& Gamepads::poll() {
    auto& s = *_impl;
    s.primary = {};
    if (!s.subsystem) {
        return s.primary;
    }
    s.close_disconnected();
    if (s.polls++ % 60 == 0) {
        s.open_new();
    }
    SDL_UpdateGamepads();
    for (std::size_t i = 0; i < s.handles.size(); ++i) {
        s.devices[i].state = read(s.handles[i], s.devices[i].id);
    }
    s.players.update(s.devices, s.join_on_start);
    if (s.players.joined(0)) {
        s.primary = s.players.state(0);
    } else if (!s.devices.empty()) {
        s.primary = s.devices.front().state;
    }
    return s.primary;
}

bool Gamepads::rumble(u32 id, f32 low, f32 high, u32 duration_ms) {
    const auto found = std::ranges::find(_impl->devices, id, &GamepadDevice::id);
    if (found == _impl->devices.end() || !std::isfinite(low) || !std::isfinite(high)) {
        return false;
    }
    SDL_Gamepad* pad = _impl->handles[static_cast<std::size_t>(found - _impl->devices.begin())];
    return SDL_RumbleGamepad(pad, static_cast<Uint16>(std::clamp(low, 0.0f, 1.0f) * 65535),
                             static_cast<Uint16>(std::clamp(high, 0.0f, 1.0f) * 65535), duration_ms);
}

bool Gamepads::rumble_player(std::size_t player, f32 low, f32 high, u32 duration_ms) {
    return rumble(_impl->players.device(player), low, high, duration_ms);
}

} // namespace kin
