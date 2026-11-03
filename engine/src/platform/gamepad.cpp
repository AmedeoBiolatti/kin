#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <kin/platform/gamepad.hpp>
#include <map>
#include <set>
#include <stdexcept>

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
    const SDL_GamepadButton buttons[] = {
        SDL_GAMEPAD_BUTTON_SOUTH,          SDL_GAMEPAD_BUTTON_EAST,        SDL_GAMEPAD_BUTTON_WEST,
        SDL_GAMEPAD_BUTTON_NORTH,          SDL_GAMEPAD_BUTTON_BACK,        SDL_GAMEPAD_BUTTON_START,
        SDL_GAMEPAD_BUTTON_LEFT_STICK,     SDL_GAMEPAD_BUTTON_RIGHT_STICK, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
        SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_DPAD_UP,     SDL_GAMEPAD_BUTTON_DPAD_DOWN,
        SDL_GAMEPAD_BUTTON_DPAD_LEFT,      SDL_GAMEPAD_BUTTON_DPAD_RIGHT};
    for (u32 i = 0; i < static_cast<u32>(GamepadButton::Count); ++i)
        result.set_button(static_cast<GamepadButton>(i), SDL_GetGamepadButton(pad, buttons[i]));
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
    if (index >= static_cast<std::size_t>(GamepadButton::Count))
        return {};
    if (index < 4 && family == GamepadFamily::PlayStation)
        return sony[index];
    if (index < 4 && family == GamepadFamily::Nintendo)
        return nintendo[index];
    return generic[index];
}

GamepadPlayers::GamepadPlayers(std::size_t max_players) : _players(max_players) {
    if (max_players == 0 || max_players > 64)
        throw std::invalid_argument("player capacity must be 1..64");
}
bool GamepadPlayers::join(u32 id, std::size_t slot) {
    if (!id || slot >= _players.size())
        return false;
    const auto found = std::ranges::find(_devices, id, &GamepadDevice::id);
    if (found == _devices.end() || !found->state.connected)
        return false;
    if (std::ranges::any_of(_players, [&](const auto& player) { return player.joined && player.device == id; }))
        return false;
    if (_players[slot].joined)
        return false;
    _players[slot] = {id, found->identity, found->state, true};
    return true;
}
void GamepadPlayers::leave(std::size_t player) {
    if (player < _players.size())
        _players[player] = {};
}
void GamepadPlayers::update(std::span<const GamepadDevice> devices, bool join_on_start) {
    const auto old = std::move(_devices);
    _devices.assign(devices.begin(), devices.end());
    std::set<u32> owned;
    std::set<u32> reserved;
    for (const auto& player : _players)
        if (player.joined)
            if (std::ranges::any_of(_devices,
                                    [&](const auto& d) { return d.id == player.device && d.state.connected; }))
                reserved.insert(player.device);
    for (auto& player : _players) {
        if (!player.joined)
            continue;
        player.state = {};
        auto current = std::ranges::find(_devices, player.device, &GamepadDevice::id);
        if ((current == _devices.end() || !current->state.connected) && !player.identity.empty()) {
            std::vector<const GamepadDevice*> matches;
            for (const auto& d : _devices)
                if (d.identity == player.identity && d.state.connected && !reserved.contains(d.id) &&
                    std::ranges::find(old, d.id, &GamepadDevice::id) == old.end())
                    matches.push_back(&d);
            if (matches.size() == 1) {
                player.device = matches.front()->id;
                reserved.insert(player.device);
                current = std::ranges::find(_devices, player.device, &GamepadDevice::id);
            }
        }
        if (current != _devices.end() && current->state.connected && !owned.contains(current->id)) {
            player.state = current->state;
            player.state.device = current->id;
            owned.insert(current->id);
        }
    }
    if (join_on_start)
        for (const auto& d : _devices) {
            const auto previous = std::ranges::find(old, d.id, &GamepadDevice::id);
            if (!d.state.connected || owned.contains(d.id) || !d.state.button(GamepadButton::Start) ||
                (previous != old.end() && previous->state.button(GamepadButton::Start)))
                continue;
            for (std::size_t p = 0; p < _players.size(); ++p)
                if (!_players[p].joined) {
                    if (join(d.id, p))
                        owned.insert(d.id);
                    break;
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
    std::map<u32, SDL_Gamepad*> handles;
    std::vector<GamepadDevice> devices;
    GamepadPlayers players;
    GamepadState primary;
    std::string name;
    bool subsystem = false;
    ~Impl() {
        for (const auto& [id, handle] : handles)
            SDL_CloseGamepad(handle);
        if (subsystem)
            SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    }
};
Gamepads::Gamepads() : _impl(std::make_unique<Impl>()) {
    _impl->subsystem = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
}
Gamepads::~Gamepads() = default;
const char* Gamepads::name() const {
    return _impl->name.c_str();
}
std::span<const GamepadDevice> Gamepads::devices() const {
    return _impl->devices;
}
GamepadPlayers& Gamepads::players() {
    return _impl->players;
}
const GamepadState& Gamepads::poll() {
    auto& s = *_impl;
    s.devices.clear();
    s.primary = {};
    s.name.clear();
    if (!s.subsystem)
        return s.primary;
    int count = 0;
    auto* ids = SDL_GetGamepads(&count);
    std::set<u32> connected;
    for (int i = 0; ids && i < count; ++i)
        connected.insert(ids[i]);
    SDL_free(ids);
    for (auto it = s.handles.begin(); it != s.handles.end();) {
        if (!connected.contains(it->first) || !SDL_GamepadConnected(it->second)) {
            SDL_CloseGamepad(it->second);
            it = s.handles.erase(it);
        } else
            ++it;
    }
    for (u32 id : connected)
        if (!s.handles.contains(id)) {
            if (auto* pad = SDL_OpenGamepad(id))
                s.handles.emplace(id, pad);
        }
    SDL_UpdateGamepads();
    for (const auto& [id, pad] : s.handles) {
        char guid[33];
        SDL_GUIDToString(SDL_GetJoystickGUID(SDL_GetGamepadJoystick(pad)), guid, sizeof(guid));
        const char* serial = SDL_GetGamepadSerial(pad);
        const char* name = SDL_GetGamepadName(pad);
        s.devices.push_back({id, name ? name : "", std::string(guid) + ":" + (serial ? serial : ""), read(pad, id)});
    }
    s.players.update(s.devices);
    if (s.players.joined(0)) {
        s.primary = s.players.state(0);
        const auto current = std::ranges::find(s.devices, s.primary.device, &GamepadDevice::id);
        if (current != s.devices.end())
            s.name = current->name;
    } else if (!s.devices.empty()) {
        s.primary = s.devices.front().state;
        s.name = s.devices.front().name;
    }
    return s.primary;
}
bool Gamepads::rumble(u32 id, f32 low, f32 high, u32 duration_ms) {
    const auto found = _impl->handles.find(id);
    if (found == _impl->handles.end() || !std::isfinite(low) || !std::isfinite(high))
        return false;
    return SDL_RumbleGamepad(found->second, static_cast<Uint16>(std::clamp(low, 0.0f, 1.0f) * 65535),
                             static_cast<Uint16>(std::clamp(high, 0.0f, 1.0f) * 65535), duration_ms);
}
bool Gamepads::rumble_player(std::size_t player, f32 low, f32 high, u32 duration_ms) {
    return rumble(_impl->players.device(player), low, high, duration_ms);
}
} // namespace kin
