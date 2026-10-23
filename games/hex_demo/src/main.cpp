// Hex grid demo: a small map of terrain with a unit, its movement range and an A*
// path to a goal, drawn in any of the four hex layouts.
//
//   Left click   set the goal          Tab  pointy-top / flat-top
//   Right click  move the unit         O    odd / even offset
//   R            reset                 Esc  quit

#include "hex_game.hpp"
#include <kin/platform/input.hpp>
#include <kin/runtime/game_info.hpp>
#include <kin/runtime/scene_app.hpp>
#include <kin/scene/scene.hpp>
#include <kin/scene/scene_manager.hpp>

namespace demo {
namespace {
kin::GameInfo make_game_info() {
    kin::InputMap input;
    input.bind("quit", kin::Key::Escape);
    input.bind("orientation", kin::Key::Tab);
    input.bind("offset", kin::Key::O);
    input.bind("reset", kin::Key::R);

    return {
        .id = "hex_demo",
        .title = "Kin Hex Demo",
        .version = "0.1",
        .description = "Hex coordinates, picking, movement range and A* paths in all four hex layouts.",
        .author = "Kin contributors",
        .window = {
            .width = static_cast<kin::i32>(logical_size.x),
            .height = static_cast<kin::i32>(logical_size.y),
            .logical_width = static_cast<kin::i32>(logical_size.x),
            .logical_height = static_cast<kin::i32>(logical_size.y),
            .resizable = true,
        },
        .tags = {"sample", "hex", "pathfinding"},
        .fields = {},
        .input_map = input,
    };
}

class HexDemoScene final : public kin::Scene {
public:
    std::string_view name() const override { return "Hex Demo"; }
    void update(kin::SceneContext& ctx) override {
        if (!_game.update({
            .quit = ctx.window.close_requested() || ctx.input.pressed("quit"),
            .reset = ctx.input.pressed("reset"),
            .orientation = ctx.input.pressed("orientation"),
            .offset = ctx.input.pressed("offset"),
            .left = ctx.input.mouse_pressed(kin::MouseButton::Left),
            .right = ctx.input.mouse_pressed(kin::MouseButton::Right),
            .mouse = ctx.renderer.window_to_logical(ctx.input.mouse_pos()),
        })) ctx.app.quit();
    }
    void render(kin::SceneContext& ctx) override { _game.render(ctx.renderer); }
    void collect_actions(kin::InputActionContext& actions) const override {
        actions.add("orientation", "Toggle pointy-top / flat-top");
        actions.add("offset", "Toggle odd / even offset");
        actions.add("reset", "Reset unit and goal");
        actions.add("quit", "Quit");
    }
    void write_report(kin::JsonWriter& json) const override { _game.write_report(json); }
private:
    HexGame _game;
};
} // namespace
} // namespace demo

#ifndef KIN_HEX_DEMO_TEST
int main(int argc, char** argv) {
    kin::GameInfo game = demo::make_game_info();
    kin::SceneManager scenes;

    const auto build_scenes = [](kin::SceneManager& target) {
        target.push(std::make_unique<demo::HexDemoScene>());
    };
    build_scenes(scenes);

    return kin::run_scene_app({
        .window = kin::window_config(game),
        .headless = kin::parse_headless_options(argc, argv),
        .game = &game,
        .reset_scenes = build_scenes,
    }, scenes);
}

#endif
