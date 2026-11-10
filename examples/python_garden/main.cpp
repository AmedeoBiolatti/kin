// Python garden: a small platformer whose gameplay is all Python
// (content/scripts/, docs/python_scripting.md). The gardener picks the seeds
// of the garden and of its shed while beetles walk about; three lives; F5
// saves, F9 reads the save back, R starts again.
//
// The scenes and prefabs (content/scenes/, content/prefabs/) are data, the
// scripts' components in them. The host reads a scene into a world of its
// own, with its physics and scripts, and gives the scripts kin's scene flow
// (the doors, scene.go) and saves (save.*), then steps and draws it. Going
// through a door, it keeps what the scene's Saved entities are and reads the
// other; a save read, it reads the scene it was written in. Headless
// (--headless --frames=N), the gardener plays itself, and the run fails if a
// script raised, nothing was picked, or starting again, saving and reading
// back, or going to the shed and back did not do what they should.

#include <filesystem>
#include <iostream>
#include <kin/assets/content.hpp>
#include <kin/core/json.hpp>
#include <kin/ecs/assets2d.hpp>
#include <kin/ecs/component.hpp>
#include <kin/ecs/physics.hpp>
#include <kin/ecs/render.hpp>
#include <kin/ecs/saving.hpp>
#include <kin/ecs/scene_flow.hpp>
#include <kin/ecs/world.hpp>
#include <kin/runtime/run_report.hpp>
#include <kin/runtime/scene_app.hpp>
#include <kin/scene/scene_asset.hpp>
#include <kin/scripting/python.hpp>
#include <memory>

namespace {

using namespace kin;

const std::string start_scene = "scenes/garden.kinscene";

// One scene's world, its physics and its scripts, made and gone together.
struct Level {
    std::unique_ptr<kin::EcsWorld> world;
    std::unique_ptr<PhysicsWorld> physics;
    std::unique_ptr<EcsPhysicsBridge> bridge;
    std::unique_ptr<PythonScripts> scripts;
    std::string scene;
    Color clear = Color::rgb(0, 0, 0);

    ~Level() {
        scripts.reset(); // before its world
        bridge.reset();
        physics.reset();
        world.reset();
    }
};

class Garden final : public Scene {
public:
    explicit Garden(std::filesystem::path root) : _root(std::move(root)) {}

    std::string_view name() const override { return "Python garden"; }

    void on_enter(SceneContext& ctx) override {
        InputMap map;
        map.bind("jump", {Key::Space, Key::W, Key::Up});
        map.bind("down", {Key::S, Key::Down});
        map.bind("interact", Key::E);
        map.bind("restart", Key::R);
        map.bind("save", Key::F5);
        map.bind("load", Key::F9);
        map.set_axis({.name = "move",
                      .negative = {*parse_binding_text("key:A"), *parse_binding_text("key:Left")},
                      .positive = {*parse_binding_text("key:D"), *parse_binding_text("key:Right")}});
        ctx.input.set_map(map);

        _headless = ctx.app.headless();
        SaveStoreConfig store{.game_id = "kin_python_garden", .game_version = "1"};
        if (_headless) { // a run of its own, not the player's saves
            store.root_override = std::filesystem::temp_directory_path() / "kin_python_garden_saves";
            std::filesystem::remove_all(store.root_override);
        }
        _saves = std::make_unique<SceneSaves>(store, _root);
        _assets = std::make_unique<Assets2D>(ctx.renderer, _root); // fonts for the texts
        new_game(ctx);
        if (!_headless) {
            // For editors: VS Code completes and checks the scripts with it.
            std::string error;
            if (!_level->scripts->write_stubs(_root / "scripts/kin.pyi", &error)) {
                std::cerr << "kin.pyi: " << error << '\n';
            }
        }
    }

    void update(SceneContext& ctx) override {
        if (ctx.input.pressed("restart")) {
            new_game(ctx);
        }
        update_physics_world(*_level->world, *_level->physics, ctx.dt);
        _level->scripts->update(ctx.input, ctx.dt);
        _flow.update(*_level->world, ctx.input);
        _saves->played(ctx.dt);
        tell_problems(ctx);
        if (std::optional<SceneTravel> go = _flow.take()) {
            _saves->keep(*_level->world); // what its Saved entities are, for coming back
            enter(ctx, go->scene.empty() ? _level->scene : go->scene, go->arrive);
        }
        if (std::optional<std::string> slot = _saves->take_read()) {
            std::string error;
            if (std::optional<std::string> scene = _saves->read(*slot, error)) {
                enter(ctx, *scene, "");
                _level->scripts->emit("loaded");
            } else {
                std::cerr << "save " << *slot << ": " << error << '\n';
            }
        }
        ++_frames;
        if (_headless && ctx.report) {
            check(ctx);
        }
    }

    void render(SceneContext& ctx) override {
        _assets->bind(*_level->world);
        ctx.renderer.clear(_level->clear);
        render_world(*_level->world, ctx.renderer);
    }

    void write_report(JsonWriter& json) const override {
        json.field("scene", std::string_view{_level ? _level->scene : std::string()});
        json.field("seeds", value("seeds", 0));
        json.field("lives", value("lives", 3));
        json.field("tasks", static_cast<u64>(_level ? _level->scripts->tasks() : 0));
    }

    kin::EcsWorld* world() override { return _level ? _level->world.get() : nullptr; }

    void on_exit(SceneContext&) override {
        _level.reset();
        _assets.reset();
        _saves.reset();
    }

private:
    // From nothing picked, in the garden.
    void new_game(SceneContext& ctx) {
        _saves->clear();
        if (_headless) {
            _saves->set_value("autoplay", JsonValue(true)); // the gardener plays itself
        }
        enter(ctx, start_scene, "");
    }

    // The scene read into a new world, what was kept of it put back, and the
    // travellers at `arrive`.
    void enter(SceneContext& ctx, const std::string& scene, const std::string& arrive) {
        _level.reset();
        auto level = std::make_unique<Level>();
        level->world = std::make_unique<kin::EcsWorld>();
        register_builtin_editor_components(*level->world);
        level->physics =
            std::make_unique<PhysicsWorld>(PhysicsWorldDef{.gravity = {0.0f, 0.0f}, .units_per_meter = 32.0f});
        level->bridge = std::make_unique<EcsPhysicsBridge>(*level->world, *level->physics);
        // The scripts first: their components are in the scene file.
        level->scripts = std::make_unique<PythonScripts>(*level->world, PythonScriptsConfig{.content = _root});
        level->scripts->set_physics(level->physics.get());
        level->scripts->set_flow(&_flow);
        level->scripts->set_saves(_saves.get());
        level->scene = scene;
        const SceneLoadResult read = load_scene_asset(_root / scene, level->world->components());
        if (!read.scene) {
            throw std::runtime_error(scene + ": " +
                                     (read.diagnostics.empty() ? "unreadable" : read.diagnostics.front()));
        }
        const SceneInstance made =
            instantiate_scene_asset(*level->world, *read.scene, _root, level->world->components());
        for (const std::string& problem : made.diagnostics) {
            fail(ctx, scene + ": " + problem);
        }
        level->clear = read.scene->clear_color;
        _saves->enter(*level->world, scene);
        for (const std::string& problem : _saves->problems()) {
            fail(ctx, "putting back " + scene + ": " + problem);
        }
        _flow.reset();
        _flow.arrive(*level->world, arrive);
        _level = std::move(level);
        tell_problems(ctx);
    }

    i64 value(const char* name, i64 fallback) const {
        const JsonValue* found = _saves ? _saves->value(name) : nullptr;
        return found ? found->as_int(fallback) : fallback;
    }

    // Entities with a component, by its name.
    i64 count(const char* component) const {
        const ComponentDescriptor* found = _level->world->components().find(component);
        i64 n = 0;
        if (found) {
            _level->world->raw()
                .query_builder()
                .with(static_cast<flecs::id_t>(found->id))
                .build()
                .each([&](flecs::entity) { ++n; });
        }
        return n;
    }

    // Ten seconds of autoplay pick something, or the game is broken; then a
    // save, starting again, the save read back, and the shed and back.
    void check(SceneContext& ctx) {
        switch (_frames) {
        case 600:
            _picked = value("seeds", 0);
            if (_picked == 0) {
                fail(ctx, "the autoplaying gardener picked no seed in 600 frames");
            }
            _level->scripts->emit("quicksave");
            break;
        case 602:
            if (!_saves->store().slot_exists("quick")) {
                fail(ctx, "quicksave wrote no slot");
            }
            new_game(ctx);
            break;
        case 604:
            if (value("seeds", 0) != 0 || value("lives", 3) != 3 || count("Seed") != 9) {
                fail(ctx, "starting again did not plant the garden again (seeds " + std::to_string(value("seeds", 0)) +
                              ", lives " + std::to_string(value("lives", 3)) + ", " + std::to_string(count("Seed")) +
                              " planted)");
            }
            _level->scripts->emit("quickload");
            break;
        case 606:
            // Autoplay picks only in the garden: as many seeds gone as picked.
            if (value("seeds", 0) != _picked || count("Seed") != 9 - _picked) {
                fail(ctx, "the save read back is not the game saved");
            }
            _flow.go({.scene = "scenes/shed.kinscene", .arrive = "garden door"});
            break;
        case 612: // a few updates in the shed
            if (_level->scene != "scenes/shed.kinscene" || !_level->world->entities().find_by_name("shed floor")) {
                fail(ctx, "the door to the shed did not go to the shed");
            }
            _in_shed = count("Seed");
            _flow.go({.scene = start_scene, .arrive = "shed door"});
            break;
        case 614:
            if (_level->scene != start_scene || count("Seed") != 9 - _picked || _in_shed != 3) {
                fail(ctx, "the garden came back not as it was left");
            }
            break;
        default:
            break;
        }
    }

    void fail(SceneContext& ctx, const std::string& why) {
        std::cerr << why << '\n';
        if (_headless && ctx.report) {
            ctx.report->fail(why);
        }
    }

    void tell_problems(SceneContext& ctx) {
        if (!_level) {
            return;
        }
        for (const auto& [module, problem] : _level->scripts->problems()) {
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
    std::unique_ptr<Assets2D> _assets;
    std::unique_ptr<SceneSaves> _saves;
    SceneFlow _flow;
    std::unique_ptr<Level> _level;
    std::map<std::string, std::string> _told;
    bool _headless = false;
    int _frames = 0;
    i64 _picked = 0;
    i64 _in_shed = 0;
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
