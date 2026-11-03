#include <SDL3/SDL.h>
#include <cassert>
#include <kin/platform/input_router.hpp>
#include <limits>

using namespace kin;
void routing() {
    Input input;
    InputMap game, menu;
    game.bind("fire", Key::Space);
    menu.bind("accept", Key::Space);
    InputRouter router;
    router.push({.name = "game", .map = game});
    router.push({.name = "menu", .map = menu, .enabled = false, .modal = true});
    input.set_key_pressed(Key::Space);
    router.update(input);
    assert(router.action("game", "fire").pressed);
    router.layer("menu")->enabled = true;
    router.update(input);
    assert(!router.action("game", "fire").held && router.action("game", "fire").released);
    assert(router.action("menu", "accept").held);
    input.advance_step_edges();
    router.update(input);
    assert(!router.action("menu", "accept").pressed);
    router.layer("menu")->modal = false;
    input.set_key_released(Key::Space);
    router.update(input);
    assert(router.action("menu", "accept").released);
    input.begin_frame();
    input.set_key_pressed(Key::Space);
    router.update(input);
    assert(router.action("menu", "accept").pressed && !router.action("game", "fire").pressed);
    router.remove("menu");
    input.advance_step_edges();
    router.update(input);
    assert(router.action("game", "fire").held && !router.action("game", "fire").pressed);
    InputMap analogue, digital;
    analogue.set_axis({.name = "move", .sticks = {{.axis = PadAxis::LeftX}}});
    digital.bind("right", PadControl::LeftStickRight);
    InputRouter sticks;
    sticks.push({.name = "game", .map = analogue});
    sticks.push({.name = "ui", .map = digital});
    input.set_gamepad({.connected = true, .left_x = .9f});
    sticks.update(input);
    assert(sticks.action("ui", "right").held && sticks.axis("game", "move") == 0);
    InputRouter reverse;
    reverse.push({.name = "game", .map = digital});
    reverse.push({.name = "ui", .map = analogue});
    reverse.update(input);
    assert(reverse.axis("ui", "move") > .8f && !reverse.action("game", "right").held);

    InputRebinder rebind;
    rebind.begin(input, "fire");
    assert(rebind.update(input, 0.1f) == RebindState::Listening); // initial held Space ignored
    input.begin_frame();
    input.set_key_released(Key::Space);
    rebind.update(input, 0);
    input.begin_frame();
    input.set_key_pressed(Key::Enter);
    game.bind("confirm", Key::Enter);
    assert(rebind.update(input, 0) == RebindState::Captured);
    auto before = game;
    std::string error;
    assert(!rebind.apply(game, RebindConflict::Reject, error) && game == before);
    assert(rebind.apply(game, RebindConflict::Replace, error));
    assert(!game.bindings("confirm") && game.bindings("fire")->front().code == static_cast<i32>(Key::Enter));
    const auto overrides = serialize_input_overrides(before, game);
    InputMap restored;
    assert(apply_input_overrides(overrides, before, restored, error) && restored == game);
    const auto save = std::filesystem::temp_directory_path() / "kin-test-input-overrides.json";
    assert(save_input_overrides(before, game, save));
    assert(save_input_overrides(before, game, save)); // atomic replacement of an existing file
    assert(load_input_overrides(before, restored, save, error) && restored == game);
    std::filesystem::remove(save);
    const auto good = restored;
    assert(!apply_input_overrides("{}", before, restored, error) && restored == good);
    input.begin_frame();
    rebind.begin(input, "fire", 0.1f);
    assert(rebind.update(input, 0.2f) == RebindState::TimedOut);
    rebind.begin(input, "fire");
    input.set_key_pressed(Key::Escape);
    assert(rebind.update(input, 0) == RebindState::Cancelled);
}
void ownership() {
    GamepadPlayers players{2};
    GamepadDevice a{1, "first", "same model", {.connected = true}};
    GamepadDevice b{2, "second", "same model", {.connected = true}};
    players.update(std::vector{a, b}, false);
    assert(players.join(1, 0) && players.join(2, 1) && !players.join(2, 0));
    players.update(std::vector{b}, false);
    assert(players.joined(0) && !players.state(0).connected && players.device(1) == 2);
    a.id = 3;
    players.update(std::vector{a, b}, false);
    assert(players.device(0) == 3 && players.state(0).connected); // reconnect, no theft
    players.leave(0);
    a.state.set_button(GamepadButton::Start, true);
    players.update(std::vector{a, b});
    assert(players.joined(0) && players.device(0) == 3);
    assert(gamepad_button_label(GamepadFamily::PlayStation, GamepadButton::A) == "Cross");
    assert(gamepad_button_label(GamepadFamily::Nintendo, GamepadButton::A) == "B");
}
struct Rumble {
    Uint16 low = 0, high = 0;
};
bool SDLCALL rumble(void* data, Uint16 low, Uint16 high) {
    *static_cast<Rumble*>(data) = {low, high};
    return true;
}
void virtual_devices() {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    Gamepads pads;
    Rumble output;
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = 6;
    desc.nbuttons = 14;
    desc.button_mask = (1u << 14) - 1;
    desc.axis_mask = (1u << 6) - 1;
    desc.name = "Kin test pad";
    desc.userdata = &output;
    desc.Rumble = rumble;
    const auto first = SDL_AttachVirtualJoystick(&desc);
    desc.product_id = 2;
    const auto second = SDL_AttachVirtualJoystick(&desc);
    assert(first && second);
    pads.poll();
    assert(pads.devices().size() == 2);
    assert(pads.players().join(second, 0));
    auto* joystick = SDL_OpenJoystick(second);
    assert(joystick && SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_SOUTH, true));
    pads.poll();
    assert(pads.players().state(0).button(GamepadButton::A));
    assert(pads.rumble_player(0, 0.5f, 1.0f, 100) && output.low > 32000 && output.high == 65535);
    assert(!pads.rumble(second, std::numeric_limits<float>::quiet_NaN(), 0, 1));
    SDL_CloseJoystick(joystick);
    assert(SDL_DetachVirtualJoystick(second));
    pads.poll();
    assert(!pads.players().state(0).connected);
    assert(!pads.rumble_player(0, 1, 1, 1));
    assert(SDL_DetachVirtualJoystick(first));
}
int main() {
    routing();
    ownership();
    virtual_devices();
}
