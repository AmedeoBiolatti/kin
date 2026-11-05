#include <SDL3/SDL.h>
#include <cassert>
#include <kin/platform/input_router.hpp>
#include <kin/platform/user_data.hpp>
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
    assert(router.layer("menu")->defaults == menu); // what overrides are relative to
    const u64 pushed = router.revision();
    input.set_key_pressed(Key::Space);
    router.update(input);
    assert(router.action("game", "fire").pressed);
    assert(router.set_enabled("menu", true) && router.revision() == pushed); // only maps move it
    assert(!router.set_enabled("missing", true));
    router.update(input);
    assert(!router.action("game", "fire").held && router.action("game", "fire").released);
    assert(router.action("menu", "accept").held);
    input.advance_step_edges();
    router.update(input);
    assert(!router.action("menu", "accept").pressed);
    input.set_key_released(Key::Space);
    router.update(input);
    assert(router.action("menu", "accept").released);
    input.begin_frame();
    input.set_key_pressed(Key::Space);
    router.update(input);
    assert(router.action("menu", "accept").pressed && !router.action("game", "fire").pressed);
    assert(router.remove("menu") && router.revision() != pushed);
    input.advance_step_edges();
    router.update(input);
    assert(router.action("game", "fire").held && !router.action("game", "fire").pressed);

    // A layer that is not modal stops only the controls it uses.
    InputMap jump = game;
    jump.bind("jump", Key::W);
    InputRouter layered;
    layered.push({.name = "game", .map = jump});
    layered.push({.name = "ui", .map = menu});
    Input both;
    both.set_key_pressed(Key::Space);
    both.set_key_pressed(Key::W);
    layered.update(both);
    assert(layered.action("ui", "accept").pressed && !layered.action("game", "fire").held);
    assert(layered.action("game", "jump").pressed);
    // A new map drops the results of what it no longer has.
    const u64 before_map = layered.revision();
    assert(layered.set_map("game", game) && layered.revision() != before_map);
    layered.update(both);
    assert(!layered.action("game", "jump").held && layered.layer("game")->defaults == jump);

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
    input.set_gamepad({});

    InputRebinder rebind;
    rebind.begin(input, "fire");
    assert(rebind.update(input, 0.1f) == RebindState::Listening); // initial held Space ignored
    input.begin_frame();
    input.set_key_released(Key::Space);
    rebind.update(input, 0);
    input.begin_frame();
    input.set_key_pressed(Key::Enter);
    input.consume_frame_edges(); // a frame that ran no step: the step still sees the press
    game.bind("fire", PadControl::A);
    game.bind("confirm", Key::Enter);
    assert(rebind.update(input, 0) == RebindState::Captured);
    auto before = game;
    std::string error;
    assert(!rebind.apply(game, RebindConflict::Reject, error) && game == before);
    assert(rebind.apply(game, RebindConflict::Replace, error));
    assert(!game.bindings("confirm"));
    // The key replaced Fire's keys; its controller button stays.
    const auto& fire = *game.bindings("fire");
    assert(fire.size() == 2 &&
           fire.front() == (InputBinding{InputBindingDevice::Gamepad, static_cast<i32>(PadControl::A)}));
    assert(fire.back() == (InputBinding{InputBindingDevice::Key, static_cast<i32>(Key::Enter)}));
    const auto overrides = serialize_input_overrides(before, game);
    InputMap restored;
    assert(apply_input_overrides(overrides, before, restored, error) && restored == game);

    // Profiles live in the game's own folder, beside its saves.
    const auto data = std::filesystem::temp_directory_path() / "kin-input-workflow-user-data";
    std::filesystem::remove_all(data);
    set_user_data_dir_override(data);
    assert(input_profile_path("keys", error).empty() && !error.empty()); // no game id yet
    set_user_data_game_id("kin.input-test");
    assert(input_profile_path("../escape", error).empty());
    const auto save = input_profile_path("keys", error);
    assert(save == game_data_dir("kin.input-test") / "input" / "keys.json");
    assert(save.string().find("kin.input-test") != std::string::npos);
    assert(save_input_overrides(before, game, save));
    assert(save_input_overrides(before, game, save)); // atomic replacement of an existing file
    assert(!std::filesystem::exists(std::filesystem::path(save.string() + ".tmp")));
    assert(load_input_overrides(before, restored, save, error) && restored == game);
    set_user_data_game_id({});
    clear_user_data_dir_override();
    std::filesystem::remove_all(data);
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
#if KIN_TEST_SDL_GAMEPAD
    virtual_devices();
#endif
}
