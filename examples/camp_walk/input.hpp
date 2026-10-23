#pragma once

// Device state stays at the application boundary. Gameplay and scripted tests
// submit the same small value once per fixed step; nothing here owns a device.
#include <algorithm>
#include <cmath>
#include <kin/platform/gamepad.hpp>
#include <kin/platform/input.hpp>
#include <type_traits>

namespace camp_walk {
using namespace kin;
struct PlayerCommand {
    Vec2f move{}, look{}; // camera-relative movement; look in degrees this step
    bool jump = false, crouch = false, sprint = false, interact = false, push = false;
};
static_assert(std::is_trivially_copyable_v<PlayerCommand>);
struct Commands {
    PlayerCommand player;
    bool restart = false, refresh = false, edit = false, quit = false;
};
inline InputMap input_map() {
    InputMap map;
    map.bind("camp.forward", {Key::W, Key::Up});
    map.bind("camp.back", {Key::S, Key::Down});
    map.bind("camp.left", {Key::A, Key::Left});
    map.bind("camp.right", {Key::D, Key::Right});
    map.bind("camp.jump", Key::Space);
    map.bind("camp.crouch", Key::C);
    map.bind("camp.sprint", Key::LeftShift);
    map.bind("camp.interact", Key::E);
    map.bind("camp.push", Key::F);
    map.bind("camp.look", MouseButton::Right);
    map.bind("camp.restart", Key::R);
    map.bind("camp.refresh", Key::F5);
    map.bind("camp.edit", Key::F6);
    map.bind("camp.quit", Key::Escape);
    return map;
}
inline void collect_actions(InputActionContext& actions) {
    actions.add("camp.forward", "Forward");
    actions.add("camp.back", "Back");
    actions.add("camp.left", "Left");
    actions.add("camp.right", "Right");
    actions.add("camp.jump", "Jump");
    actions.add("camp.crouch", "Crouch");
    actions.add("camp.sprint", "Sprint");
    actions.add("camp.interact", "Collect supply");
    actions.add("camp.push", "Push crate");
    actions.add("camp.look", "Hold to orbit camera");
    actions.add("camp.restart", "Restart");
    actions.add("camp.refresh", "Refresh content");
    actions.add("camp.edit", "Configure camp");
    actions.add("camp.quit", "Quit");
}
inline Vec2f unit_move(Vec2f value) {
    if (!std::isfinite(value.x) || !std::isfinite(value.y))
        return {};
    value.x = std::clamp(value.x, -1.f, 1.f);
    value.y = std::clamp(value.y, -1.f, 1.f);
    const float length = std::sqrt(value.x * value.x + value.y * value.y);
    return length > 1 ? Vec2f{value.x / length, value.y / length} : value;
}
inline Vec2f stick(Vec2f value, float dead_zone) {
    value = unit_move(value);
    dead_zone = std::clamp(dead_zone, 0.f, .95f);
    const float length = std::sqrt(value.x * value.x + value.y * value.y);
    if (length <= dead_zone)
        return {};
    const float scale = (length - dead_zone) / ((1 - dead_zone) * length);
    return {value.x * scale, value.y * scale};
}
struct InputSettings {
    float dead_zone = .18f, mouse_sensitivity = .12f, stick_look_speed = 120;
    GamepadButton jump = GamepadButton::A, crouch = GamepadButton::B, interact = GamepadButton::X,
                  push = GamepadButton::Y, sprint = GamepadButton::LeftStick, restart = GamepadButton::Start;
};
class InputMapper {
    u32 _previous_buttons = 0;

public:
    InputSettings settings;
    // Poll once per fixed step. A held pad button produces one edge, disconnect
    // clears history, and keyboard/mouse use Input's existing sticky step edges.
    Commands sample(const Input& input, const GamepadState& pad, float dt) {
        const u32 buttons = pad.connected ? pad.buttons : 0;
        const u32 pressed = buttons & ~_previous_buttons;
        _previous_buttons = buttons;
        const auto held = [&](GamepadButton b) { return (buttons & (1u << u32(b))) != 0; };
        const auto edge = [&](GamepadButton b) { return (pressed & (1u << u32(b))) != 0; };
        Commands result;
        auto& command = result.player;
        const auto movement = pad.connected ? stick({pad.left_x, pad.left_y}, settings.dead_zone) : Vec2f{};
        command.move = unit_move({float(input.held("camp.right")) - float(input.held("camp.left")) + movement.x,
                                  float(input.held("camp.forward")) - float(input.held("camp.back")) + movement.y});
        const auto look = pad.connected ? stick({pad.right_x, pad.right_y}, settings.dead_zone) : Vec2f{};
        const float seconds = std::isfinite(dt) ? std::max(dt, 0.f) : 0;
        command.look = {look.x * settings.stick_look_speed * seconds, look.y * settings.stick_look_speed * seconds};
        if (input.held("camp.look")) {
            const auto mouse = input.mouse_motion();
            command.look.x += mouse.x * settings.mouse_sensitivity;
            command.look.y -= mouse.y * settings.mouse_sensitivity;
        }
        command.jump = input.pressed("camp.jump") || edge(settings.jump);
        command.crouch = input.held("camp.crouch") || held(settings.crouch);
        command.sprint = input.held("camp.sprint") || held(settings.sprint);
        command.interact = input.pressed("camp.interact") || edge(settings.interact);
        command.push = input.pressed("camp.push") || edge(settings.push);
        result.restart = input.pressed("camp.restart") || edge(settings.restart);
        result.refresh = input.pressed("camp.refresh");
        result.edit = input.pressed("camp.edit");
        result.quit = input.pressed("camp.quit");
        return result;
    }
};
// The demo is just another command producer. Loading/configuration requests are
// handled between steps by the host, like real restart/refresh key presses.
inline Commands demo_commands(int step) {
    Commands commands;
    commands.restart = step == 25;
    commands.edit = step == 35;
    auto& player = commands.player;
    player.interact = step == 40 || step == 100 || step == 140;
    player.push = step == 175;
    if (step >= 60 && step < 90)
        player.move = {-1, 0};
    if (step >= 100 && step < 130)
        player.move = {0, 1};
    player.jump = step == 150;
    player.crouch = step >= 190 && step < 210;
    return commands;
}
} // namespace camp_walk
