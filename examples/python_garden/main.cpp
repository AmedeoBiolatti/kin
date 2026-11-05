// Python garden: a small platformer whose gameplay is all Python
// (content/scripts/, docs/python_scripting.md). The gardener picks the seeds
// on the beds while beetles walk below; three lives.
//
// The host only makes the world, its physics and the scripts, steps them and
// draws the world; the scripts build the garden (on "start") and keep the
// HUD's texts. Headless (--headless --frames=N), the gardener plays itself
// and the run fails if a script raised or nothing was picked.

#include <kin/assets/content.hpp>
#include <kin/core/json.hpp>
#include <kin/ecs/assets2d.hpp>
#include <kin/ecs/component.hpp>
#include <kin/ecs/physics.hpp>
#include <kin/ecs/render.hpp>
#include <kin/ecs/world.hpp>
#include <kin/runtime/run_report.hpp>
#include <kin/runtime/scene_app.hpp>
#include <kin/scene/scene_asset.hpp>
#include <kin/scripting/python.hpp>

#include <iostream>
#include <memory>

namespace {

using namespace kin;

class Garden final : public Scene {
public:
    explicit Garden(std::filesystem::path root) : _root(std::move(root)) {}

    std::string_view name() const override { return "Python garden"; }

    void on_enter(SceneContext& ctx) override {
        InputMap map;
        map.bind("jump", {Key::Space, Key::W, Key::Up});
        map.bind("down", {Key::S, Key::Down});
        map.bind("restart", Key::R);
        map.set_axis({.name = "move",
                      .negative = {*parse_binding_text("key:A"), *parse_binding_text("key:Left")},
                      .positive = {*parse_binding_text("key:D"), *parse_binding_text("key:Right")}});
        ctx.input.set_map(map);

        _world = std::make_unique<kin::EcsWorld>();
        register_builtin_editor_components(*_world);
        _physics = std::make_unique<PhysicsWorld>(PhysicsWorldDef{.gravity = {0.0f, 0.0f}, .units_per_meter = 32.0f});
        _bridge = std::make_unique<EcsPhysicsBridge>(*_world, *_physics);
        _assets = std::make_unique<Assets2D>(ctx.renderer, _root); // fonts for the texts
        _scripts = std::make_unique<PythonScripts>(*_world, PythonScriptsConfig{.content = _root});
        _headless = ctx.app.headless();
        if (!_headless) {
            // For editors: VS Code completes and checks the scripts with it.
            std::string error;
            if (!_scripts->write_stubs(_root / "scripts/kin.pyi", &error)) {
                std::cerr << "kin.pyi: " << error << '\n';
            }
        }
        _scripts->emit("start");
        if (_headless) {
            _scripts->emit("autoplay");
        }
        tell_problems(ctx);
    }

    void update(SceneContext& ctx) override {
        if (ctx.input.pressed("restart")) {
            _scripts->emit("restart");
        }
        update_physics_world(*_world, *_physics, ctx.dt);
        _scripts->update(ctx.input, ctx.dt);
        ++_frames;
        tell_problems(ctx);
        // Ten seconds of autoplay pick something, or the game is broken; then
        // R starts it again, from nothing picked and every seed planted.
        if (_headless && ctx.report) {
            if (_frames == 600 && game_field(0) == 0) {
                ctx.report->fail("the autoplaying gardener picked no seed in 600 frames");
            }
            if (_frames == 610) {
                _scripts->emit("restart");
            }
            if (_frames == 612 && (game_field(0) != 0 || game_field(1) == 0 || game_field(2) != 3)) {
                ctx.report->fail("restart did not start the garden again");
            }
        }
    }

    void render(SceneContext& ctx) override {
        _assets->bind(*_world);
        ctx.renderer.clear(Color::rgb(170, 210, 235));
        render_world(*_world, ctx.renderer);
    }

    void write_report(JsonWriter& json) const override {
        json.field("seeds", static_cast<i64>(game_field(0)));
        json.field("lives", static_cast<i64>(game_field(2)));
        json.field("tasks", static_cast<u64>(_scripts ? _scripts->tasks() : 0));
    }

    kin::EcsWorld* world() override { return _world.get(); }

    void on_exit(SceneContext&) override {
        _scripts.reset(); // before its world
        _bridge.reset();
        _physics.reset();
        _world.reset();
        _assets.reset();
    }

private:
    // A field of the scripts' Game component (0 seeds, 1 total, 2 lives).
    i32 game_field(std::size_t index) const {
        if (!_world) {
            return 0;
        }
        const kin::EcsEntity game = _world->entities().find_by_name("game");
        const std::optional<ComponentSnapshot> snapshot = game.valid() ? _world->components().snapshot(game, "Game")
                                                                       : std::nullopt;
        return snapshot && index < snapshot->fields.size() ? std::get<i32>(snapshot->fields[index].value) : 0;
    }

    void tell_problems(SceneContext& ctx) {
        for (const auto& [module, problem] : _scripts->problems()) {
            if (_told[module] == problem) {
                continue;
            }
            _told[module] = problem;
            std::cerr << module << ": " << problem << '\n';
            if (_headless && ctx.report) {
                ctx.report->fail(module + ": " + problem);
            }
        }
    }

    std::filesystem::path _root;
    std::unique_ptr<kin::EcsWorld> _world;
    std::unique_ptr<PhysicsWorld> _physics;
    std::unique_ptr<EcsPhysicsBridge> _bridge;
    std::unique_ptr<Assets2D> _assets;
    std::unique_ptr<PythonScripts> _scripts;
    std::map<std::string, std::string> _told;
    bool _headless = false;
    int _frames = 0;
};

} // namespace

int main(int argc, char** argv) {
    try {
        SceneManager scenes;
        scenes.push(std::make_unique<Garden>(find_content_root(KIN_GAME_CONTENT, argc, argv)));
        return run_scene_app({.window = {.title = "Python garden",
                                         .width = 960,
                                         .height = 540,
                                         .logical_width = 960,
                                         .logical_height = 540,
                                         .max_fps = 60},
                              .headless = parse_headless_options(argc, argv),
                              .render_headless = true},
                             scenes);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
