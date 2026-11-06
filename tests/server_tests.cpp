#include <kin/render3d/camera3d.hpp>
#include <kin/anim/player.hpp>
#include <kin/anim/anim_format.hpp>
#include <kin/core/json.hpp>
#include <kin/core/json_value.hpp>
#include <kin/ecs/world.hpp>
#include <kin/ecs/system.hpp>
#include <kin/core/rng.hpp>
#include <kin/platform/app.hpp>
#include <kin/platform/input.hpp>
#include <kin/platform/log.hpp>
#include <kin/renderer/renderer2d.hpp>
#if KIN_ENABLE_DEVELOPMENT
#include <kin/runtime/development.hpp>
#endif
#include <kin/runtime/scene_server.hpp>
#include <kin/scene/scene_manager.hpp>
#include <kin/ui2/context.hpp>
#include <kin/ui2/menu_scene.hpp>
#include <kin/ui2/widgets.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <optional>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace {

struct ServerTransform {
    kin::Vec2f position{};
    kin::i32 hp = 0;
};

bool has_log_event(const std::vector<kin::LogEvent>& events,
                   std::string_view category,
                   std::string_view message) {
    return std::ranges::any_of(events, [&](const kin::LogEvent& event) {
        return event.category == category && event.message == message;
    });
}

// Counts how often its bound action fires and records the seeded RNG value, then
// exposes both through the run report so the server's scene.current and seed
// threading can be asserted.
class CounterScene final : public kin::Scene {
public:
    std::string_view name() const override { return "Counter"; }

    void update(kin::SceneContext& ctx) override {
        _observed = kin::rng_u32(ctx.rng);
        if (ctx.input.frame_pressed("tick")) {
            ++_count;
        }
    }

    void write_report(kin::JsonWriter& json) const override {
        json.field("count", _count);
        json.field("observed", _observed);
    }

private:
    kin::i32 _count = 0;
    kin::u32 _observed = 0;
};

class SemanticProbeScene final : public kin::Scene {
public:
    explicit SemanticProbeScene(bool in_render = false) : _in_render(in_render) {}
    kin::ui2::Context* ui_context() override { return &_ui; }
    void update(kin::SceneContext& ctx) override {
        if (!_in_render) draw(ctx);
    }
    void render(kin::SceneContext& ctx) override {
        if (_in_render) draw(ctx);
    }
    void draw(kin::SceneContext& ctx) {
        _ui.begin(ctx.input, ctx.renderer, ctx.dt);
        kin::ui2::Button button{.id = kin::ui2::make_id("play"), .bounds = {10, 10, 80, 25}, .label = "Play"};
        kin::ui2::run(_ui, button);
        if (button.clicked) ++_clicks;
        _ui.semantic_key(_text.id, "player_name");
        kin::ui2::run(_ui, _text);
        _ui.end();
    }
    void write_report(kin::JsonWriter& json) const override {
        json.field("clicks", _clicks);
        json.field("text", _text.state.text);
    }
private:
    kin::ui2::Context _ui;
    kin::ui2::TextInput _text{.id = kin::ui2::make_id("name"), .bounds = {10, 45, 120, 25}};
    int _clicks = 0;
    bool _in_render = false;
};

// Reacts to injected pointer and text input so the input.mouse / input.text
// endpoints can be tested without depending on the legacy UI module.
class InputProbeScene final : public kin::Scene {
public:
    std::string_view name() const override { return "InputProbe"; }

    void update(kin::SceneContext& ctx) override {
        const kin::Vec2f mouse = ctx.renderer.window_to_logical(ctx.input.mouse_pos());
        if (ctx.input.mouse_pressed(kin::MouseButton::Left) &&
            mouse.x >= 10.0f && mouse.x < 50.0f &&
            mouse.y >= 10.0f && mouse.y < 30.0f) {
            ++_clicks;
        }
        const std::string_view typed = ctx.input.text_input();
        if (!typed.empty()) {
            _typed.assign(typed);
        }
        _motion.x += ctx.input.mouse_motion().x;
        _motion.y += ctx.input.mouse_motion().y;
    }

    void write_report(kin::JsonWriter& json) const override {
        json.field("clicks", _clicks);
        json.field("typed", std::string_view{_typed});
        json.field("motion_x", static_cast<double>(_motion.x)).field("motion_y", static_cast<double>(_motion.y));
    }

private:
    kin::i32 _clicks = 0;
    std::string _typed;
    kin::Vec2f _motion{};
};

// A ui2 region at (10, 10, 40, 20) that counts clicks, and a count of the
// "jump" action's release edges.
class ClickProbeScene final : public kin::Scene {
public:
    std::string_view name() const override { return "ClickProbe"; }

    // Injected actions act through their bindings, so "jump" needs one.
    void on_enter(kin::SceneContext& ctx) override { ctx.input.bind("jump", kin::Key::Space); }

    void update(kin::SceneContext& ctx) override {
        _ui.begin(ctx.input, ctx.renderer);
        if (_ui.region(kin::ui2::make_id("button"), {10.0f, 10.0f, 40.0f, 20.0f}).clicked) {
            ++_clicks;
        }
        _ui.end();
        if (ctx.input.released("jump")) {
            ++_jump_releases;
        }
    }

    void write_report(kin::JsonWriter& json) const override {
        json.field("clicks", _clicks);
        json.field("jump_releases", _jump_releases);
    }

private:
    kin::ui2::Context _ui;
    kin::i32 _clicks = 0;
    kin::i32 _jump_releases = 0;
};

// A TextEdit that submits on Enter, driven through input.key and input.text. It
// runs its UI in render(), as games do, so an extra render (a screenshot) must
// not replay the step's input.
class TextEditProbeScene final : public kin::Scene {
public:
    std::string_view name() const override { return "TextEditProbe"; }

    void render(kin::SceneContext& ctx) override {
        _ui.begin(ctx.input, ctx.renderer);
        kin::ui2::run(_ui, _edit);
        if (_edit.result.submitted) {
            ++_submits;
        }
        _ui.end();
    }

    void write_report(kin::JsonWriter& json) const override {
        json.field("text", std::string_view{_edit.state.text});
        json.field("submits", _submits);
    }

private:
    kin::ui2::Context _ui;
    kin::ui2::TextEdit _edit{
        .id = kin::ui2::make_id("edit"),
        .bounds = {0.0f, 0.0f, 200.0f, 60.0f},
        .submit = kin::ui2::UiSubmitKey::Enter,
    };
    kin::i32 _submits = 0;
};

// The files dropped on its window, as it takes them in update().
class DropProbeScene final : public kin::Scene {
public:
    std::string_view name() const override { return "DropProbe"; }

    void update(kin::SceneContext& ctx) override {
        for (const kin::DroppedFile& file : ctx.input.take_dropped_files()) {
            _dropped += (_dropped.empty() ? "" : ";") + file.path.generic_string() + "@" +
                        std::to_string(static_cast<int>(ctx.renderer.window_to_logical(file.pos).x));
        }
    }

    void write_report(kin::JsonWriter& json) const override { json.field("dropped", std::string_view{_dropped}); }

private:
    std::string _dropped;
};

kin::Animation make_server_animation(std::string name) {
    return kin::Animation{
        .name = std::move(name),
        .root = kin::clip_node(kin::Clip{.duration = 10.0f}),
    };
}

class AnimationSnapshotScene final : public kin::Scene {
public:
    AnimationSnapshotScene() {
        _world.component<kin::AnimationPlayer>("AnimationPlayer");
        _animations.add(make_server_animation("idle"));
        _animations.add(make_server_animation("blink"));

        kin::AnimationPlayer player = kin::make_animation_player({
            .registry = &_animations,
            .bindings = {{"X", "hero"}},
            .base_animation = "idle",
        });
        player.state = "idle";
        player.params.set_trigger("hit");
        kin::push(player, "blink", {.speed = 2.0f, .blend_in = 1.0f});
        _world.entity("actor").set(std::move(player));
    }

    std::string_view name() const override { return "AnimationSnapshot"; }
    kin::EcsWorld* world() override { return &_world; }
    const kin::AnimationAssetLibrary* animation_library() const override { return &_library; }

    void update(kin::SceneContext& ctx) override {
        kin::advance_animation_players(_world, ctx.dt);
    }

    void add_diagnostic_fixture() {
        kin::AnimationRegistryFragment fragment;
        fragment.animations.push_back(kin::Animation{
            .name = "broken",
            .root = kin::ref_node("missing"),
        });
        _library.merge(fragment);
    }

private:
    kin::EcsWorld _world;
    kin::AnimationRegistry _animations;
    kin::AnimationAssetLibrary _library;
};

class KinSnapshotScene final : public kin::Scene {
public:
    KinSnapshotScene() {
        _world.components().native<ServerTransform>("ServerTransform")
            .field("position", &ServerTransform::position)
            .field("hp", &ServerTransform::hp);
        _world.components().data("SpawnClock");
        _world.components().data("InspectionClock");
        _world.entity("server.actor").set(ServerTransform{.position = {2.0f, 3.0f}, .hp = 12});
        _world.systems().register_task({
            .id = "server.spawn",
            .reads = {"SpawnClock"},
            .writes = {"ServerTransform"},
            .execution_policy = kin::SystemExecutionPolicy::ParallelEligible,
        }, [](kin::SystemContext& ctx) {
            kin::EcsDeferredEntity entity = ctx.commands->create_entity("server.spawned");
            ctx.commands->set<ServerTransform>(entity, ServerTransform{.position = {4.0f, 5.0f}, .hp = 6});
        });
        _world.systems().register_task({
            .id = "server.inspect",
            .reads = {"InspectionClock"},
            .execution_policy = kin::SystemExecutionPolicy::ParallelEligible,
        }, [](kin::SystemContext&) {});
        _world.run_frame(0.0f, kin::SystemExecutionMode::ParallelBatches);
    }

    std::string_view name() const override { return "KinSnapshot"; }
    kin::EcsWorld* world() override { return &_world; }

private:
    kin::EcsWorld _world;
};

const kin::JsonValue* response_result(const std::string& text, kin::i64 id) {
    static kin::JsonValue result;
    std::istringstream lines{text};
    std::string line;
    while (std::getline(lines, line)) {
        if (line.empty()) {
            continue;
        }
        kin::JsonParseResult parsed = kin::parse_json(line);
        assert(parsed.ok());
        const kin::JsonValue& envelope = *parsed.value;
        if (envelope.int_at("id", -1) != id) {
            continue;
        }
        const kin::JsonValue* found = envelope.find("result");
        if (!found) {
            return nullptr;
        }
        result = *found;
        return &result;
    }
    return nullptr;
}

void test_json_parser() {
    const kin::JsonParseResult result =
        kin::parse_json(R"({"a":1,"b":[true,null,"x"],"c":{"d":2.5}})");
    assert(result.ok());
    const kin::JsonValue& value = *result.value;
    assert(value.int_at("a") == 1);
    const kin::JsonValue* array = value.find("b");
    assert(array != nullptr && array->items().size() == 3);
    assert(array->items()[0].as_bool() == true);
    assert(array->items()[1].is_null());
    assert(array->items()[2].as_string() == "x");
    assert(value.find("c")->number_at("d") == 2.5);

    assert(!kin::parse_json("{bad}").ok());
    assert(!kin::parse_json("").ok());
    const std::string escaped = "\"escaped \\\" and \\n\"";
    assert(kin::parse_json(escaped).ok());
}

void test_driven_server() {
    kin::InputMap input;
    input.bind("tick", kin::Key::Space);

    kin::SceneManager scenes;
    scenes.push(std::make_unique<CounterScene>());

    std::ostringstream out;
    std::istringstream in(
        R"({"id":1,"method":"ping"})" "\n"
        R"({"id":2,"method":"scene.stack"})" "\n"
        R"({"id":3,"method":"scene.current"})" "\n"
        R"({"id":4,"method":"input.action","params":{"name":"tick","mode":"press"}})" "\n"
        R"({"id":5,"method":"sim.tick","params":{"count":1}})" "\n"
        R"({"id":6,"method":"scene.current"})" "\n"
        R"({"id":7,"method":"world.snapshot"})" "\n"
        R"({"id":8,"method":"game.info"})" "\n"
        R"({"id":9,"method":"bogus.method"})" "\n"
        R"({"id":10,"method":"server.shutdown"})" "\n"
        R"({"id":11,"method":"ping"})" "\n"); // after shutdown: must be ignored

    kin::GameInfo game;
    game.id = "server-test";
    game.title = "Server Test";
    game.window.borderless = true;

    const int code = kin::run_scene_server({
        .window = {.title = "server", .width = 64, .height = 64, .input_map = input},
        .mode = kin::ServerMode::Driven,
        .seed = 5,
        .game = &game,
        .in = &in,
        .out = &out,
    }, scenes);
    assert(code == 0);

    const std::string text = out.str();

    // ping answered.
    assert(text.find(R"("pong":true)") != std::string::npos);
    // scene stack realized before any tick.
    assert(text.find(R"("name":"Counter")") != std::string::npos);
    assert(text.find(R"("depth":1)") != std::string::npos);
    // Counter started at 0 and reached 1 after the injected press + tick.
    assert(text.find(R"("count":0)") != std::string::npos);
    assert(text.find(R"("count":1)") != std::string::npos);
    // Seed reached the scene deterministically.
    const kin::u32 expected = kin::rng_u32(kin::make_key(5));
    assert(text.find("\"observed\":" + std::to_string(expected)) != std::string::npos);
    // world.snapshot on a non-ECS scene reports an error rather than crashing.
    assert(text.find(R"("error":{"message":"scene has no ECS world"})") != std::string::npos);
    // game.info echoed metadata.
    assert(text.find(R"("title":"Server Test")") != std::string::npos);
    assert(text.find(R"("borderless":true)") != std::string::npos);
    // unknown method surfaced as an error.
    assert(text.find("unknown method: bogus.method") != std::string::npos);
    // shutdown acknowledged.
    assert(text.find(R"("bye":true)") != std::string::npos);
    // The line after shutdown (id 11) must not have been processed: exactly one
    // pong appears.
    assert(text.find(R"("pong":true)") == text.rfind(R"("pong":true)"));
}

void test_world_snapshot_reports_animation_players() {
    kin::SceneManager scenes;
    scenes.push(std::make_unique<AnimationSnapshotScene>());

    std::ostringstream out;
    std::istringstream in(
        R"({"id":1,"method":"world.snapshot"})" "\n"
        R"({"id":2,"method":"world.snapshot","params":{"animations":false}})" "\n"
        R"({"id":3,"method":"sim.tick","params":{"count":1}})" "\n"
        R"({"id":4,"method":"world.snapshot"})" "\n"
        R"({"id":5,"method":"server.shutdown"})" "\n");

    const int code = kin::run_scene_server({
        .window = {.title = "server", .width = 64, .height = 64, .fixed_dt = 1.0f / 60.0f},
        .mode = kin::ServerMode::Driven,
        .in = &in,
        .out = &out,
    }, scenes);
    assert(code == 0);

    const std::string text = out.str();
    const kin::JsonValue* first = response_result(text, 1);
    assert(first != nullptr);
    const kin::JsonValue* animations = first->find("animations");
    assert(animations != nullptr);
    assert(animations->items().size() == 1);
    const kin::JsonValue& actor = animations->items()[0];
    assert(actor.string_at("name") == "actor");
    assert(actor.string_at("state") == "idle");
    assert(actor.find("bindings")->string_at("X") == "hero");
    assert(actor.find("triggers")->items()[0].as_string() == "hit");
    const kin::JsonValue* layers = actor.find("layers");
    assert(layers != nullptr);
    assert(layers->items().size() == 2);
    assert(layers->items()[0].string_at("kind") == "base");
    assert(layers->items()[0].string_at("animation") == "idle");
    assert(layers->items()[1].string_at("kind") == "override");
    assert(layers->items()[1].string_at("animation") == "blink");
    assert(layers->items()[1].number_at("weight") < 1.0);
    const kin::f64 first_time = layers->items()[0].number_at("time");
    const kin::f64 first_override_time = layers->items()[1].number_at("time");
    const kin::f64 first_override_weight = layers->items()[1].number_at("weight");

    const kin::JsonValue* omitted = response_result(text, 2);
    assert(omitted != nullptr);
    assert(omitted->find("animations") == nullptr);

    const kin::JsonValue* second = response_result(text, 4);
    assert(second != nullptr);
    const kin::JsonValue* second_layers = second->find("animations")->items()[0].find("layers");
    assert(second_layers->items()[0].number_at("time") > first_time);
    assert(second_layers->items()[1].number_at("time") > first_override_time);
    assert(second_layers->items()[1].number_at("weight") > first_override_weight);
}

void test_world_snapshot_reports_kin_metadata_and_payload_switches() {
    kin::SceneManager scenes;
    scenes.push(std::make_unique<KinSnapshotScene>());

    std::ostringstream out;
    std::istringstream in(
        R"({"id":1,"method":"world.snapshot"})" "\n"
        R"({"id":2,"method":"world.snapshot","params":{"raw":false,"animations":false}})" "\n"
        R"({"id":3,"method":"world.snapshot","params":{"kin":false,"animations":false}})" "\n"
        R"({"id":4,"method":"world.snapshot","params":{"raw":false,"animations":false,"events":false}})" "\n"
        R"({"id":5,"method":"server.shutdown"})" "\n");

    const int code = kin::run_scene_server({
        .window = {.title = "server", .width = 64, .height = 64},
        .mode = kin::ServerMode::Driven,
        .in = &in,
        .out = &out,
    }, scenes);
    assert(code == 0);

    const std::string text = out.str();
    const kin::JsonValue* full = response_result(text, 1);
    assert(full != nullptr);
    assert(full->find("world") != nullptr);
    const kin::JsonValue* kin_snapshot = full->find("kin");
    assert(kin_snapshot != nullptr);
    const kin::JsonValue* kin_world = kin_snapshot->find("world");
    assert(kin_world != nullptr);
    const kin::JsonValue* components = kin_world->find("components");
    assert(components != nullptr);
    const bool has_server_transform = std::ranges::any_of(components->items(), [](const kin::JsonValue& component) {
        return component.string_at("name") == "ServerTransform";
    });
    assert(has_server_transform);
    assert(kin_world->find("entities")->items().size() == 2);
    const kin::JsonValue& entity = kin_world->find("entities")->items()[0];
    assert(entity.string_at("name") == "server.actor");
    assert(entity.find("components")->items()[0].string_at("name") == "ServerTransform");
    const kin::JsonValue* systems = kin_snapshot->find("systems");
    assert(systems != nullptr);
    assert(systems->items().size() == 2);
    const kin::JsonValue& spawn_system = systems->items()[0];
    assert(spawn_system.string_at("id") == "server.spawn");
    assert(spawn_system.string_at("last_execution_mode") == "parallel_batches");
    assert(spawn_system.find("command_stats")->int_at("queued") == 2);
    assert(spawn_system.find("command_stats")->int_at("flushed") == 2);
    const kin::JsonValue* schedule = kin_snapshot->find("schedule");
    assert(schedule != nullptr);
    assert(schedule->string_at("requested_execution_mode") == "parallel_batches");
    assert(schedule->string_at("effective_execution_mode") == "parallel_batches");
    const kin::JsonValue& batch = schedule->find("batches")->items()[0];
    assert(batch.string_at("execution_mode") == "parallel_batches");
    assert(batch.find("command_stats")->int_at("flushed") == 2);
    const kin::JsonValue* events = kin_snapshot->find("events");
    assert(events != nullptr);
    assert(events->find("dirty")->bool_at("world"));
    assert(events->find("dirty")->bool_at("systems"));
    assert(!events->find("items")->items().empty());

    const kin::JsonValue* kin_only = response_result(text, 2);
    assert(kin_only != nullptr);
    assert(kin_only->find("world") == nullptr);
    assert(kin_only->find("kin") != nullptr);

    const kin::JsonValue* raw_only = response_result(text, 3);
    assert(raw_only != nullptr);
    assert(raw_only->find("world") != nullptr);
    assert(raw_only->find("kin") == nullptr);

    const kin::JsonValue* no_events = response_result(text, 4);
    assert(no_events != nullptr);
    assert(no_events->find("kin") != nullptr);
    assert(no_events->find("kin")->find("events") == nullptr);
}

void test_animation_diagnostics_endpoint() {
    auto scene = std::make_unique<AnimationSnapshotScene>();
    scene->add_diagnostic_fixture();
    kin::SceneManager scenes;
    scenes.push(std::move(scene));

    std::ostringstream out;
    std::istringstream in(
        R"({"id":1,"method":"animation.diagnostics"})" "\n"
        R"({"id":2,"method":"server.shutdown"})" "\n");

    const int code = kin::run_scene_server({
        .window = {.title = "server", .width = 64, .height = 64},
        .mode = kin::ServerMode::Driven,
        .in = &in,
        .out = &out,
    }, scenes);
    assert(code == 0);

    const kin::JsonValue* result = response_result(out.str(), 1);
    assert(result != nullptr);
    const kin::JsonValue* diagnostics = result->find("diagnostics");
    assert(diagnostics != nullptr);
    assert(diagnostics->items().size() == 1);
    assert(diagnostics->items()[0].string_at("severity") == "error");
    assert(diagnostics->items()[0].string_at("message").find("unresolved ref") != std::string::npos);
}

void test_animation_diagnostics_endpoint_missing_provider_errors() {
    kin::SceneManager scenes;
    scenes.push(std::make_unique<CounterScene>());

    std::ostringstream out;
    std::istringstream in(
        R"({"id":1,"method":"animation.diagnostics"})" "\n"
        R"({"id":2,"method":"server.shutdown"})" "\n");

    const int code = kin::run_scene_server({
        .window = {.title = "server", .width = 64, .height = 64},
        .mode = kin::ServerMode::Driven,
        .in = &in,
        .out = &out,
    }, scenes);
    assert(code == 0);
    assert(out.str().find("scene has no animation library") != std::string::npos);
}

void test_ui_snapshot_and_pointer_input() {
    kin::SceneManager scenes;
    scenes.push(std::make_unique<InputProbeScene>());

    std::ostringstream out;
    std::istringstream in(
        R"({"id":1,"method":"sim.tick","params":{"count":1}})" "\n"
        R"({"id":2,"method":"ui.snapshot"})" "\n"
        R"({"id":3,"method":"input.mouse","params":{"x":30,"y":20,"button":"left","mode":"press"}})" "\n"
        R"({"id":4,"method":"sim.tick","params":{"count":1}})" "\n"
        R"({"id":5,"method":"scene.current"})" "\n"
        R"({"id":6,"method":"input.text","params":{"text":"hello"}})" "\n"
        R"({"id":7,"method":"sim.tick","params":{"count":1}})" "\n"
        R"({"id":8,"method":"scene.current"})" "\n"
        R"({"id":9,"method":"input.mouse","params":{"dx":12,"dy":-3}})" "\n"
        R"({"id":10,"method":"sim.tick","params":{"count":2}})" "\n"
        R"({"id":11,"method":"scene.current"})" "\n"
        R"({"id":12,"method":"input.mouse","params":{"button":"left"}})" "\n"
        R"({"id":13,"method":"server.shutdown"})" "\n");

    const int code = kin::run_scene_server({
        .window = {.title = "ui", .width = 64, .height = 64},
        .mode = kin::ServerMode::Driven,
        .in = &in,
        .out = &out,
    }, scenes);
    assert(code == 0);

    const std::string text = out.str();

    const kin::JsonValue* snapshot = response_result(text, 2);
    assert(snapshot == nullptr);
    assert(text.find("scene has no semantic UI") != std::string::npos);

    // The injected click activated the button.
    const kin::JsonValue* after_click = response_result(text, 5);
    assert(after_click != nullptr);
    assert(after_click->find("state")->int_at("clicks") >= 1);

    // The injected text reached the scene.
    const kin::JsonValue* after_text = response_result(text, 8);
    assert(after_text != nullptr);
    assert(after_text->find("state")->string_at("typed") == "hello");

    // Mouse motion (mouse look) reaches one step, without moving the pointer.
    const kin::JsonValue* after_motion = response_result(text, 11);
    assert(after_motion != nullptr);
    assert(after_motion->find("state")->number_at("motion_x") == 12.0);
    assert(after_motion->find("state")->number_at("motion_y") == -3.0);
    assert(response_result(text, 12) == nullptr); // neither a place nor a motion
}

void test_controller_menu_scene() {
    kin::App app{{.mode = kin::AppMode::Headless}};
    auto& window = app.create_window({.title = "controller menu", .width = 320, .height = 240, .hidden = true});
    kin::Renderer2D renderer{window};
    kin::SceneManager scenes;
    kin::SceneContext ctx{app, window, renderer, app.input(), scenes, 0.016f};
    int play = 0, options = 0, cancelled = 0;
    kin::ui2::MenuScene menu{{
        .items = {{.label = "Play", .action = [&](auto&) { ++play; }},
                  {.label = "Options", .action = [&](auto&) { ++options; }}},
        .size = {320, 240},
        .cancel = [&](auto&) { ++cancelled; },
    }};
    const auto frame = [&] {
        app.input().begin_frame();
        menu.update(ctx);
        menu.render(ctx);
    };
    // A frame with the controller read as App reads it: after the frame begins
    // (ui2 takes accept/back as this frame's presses, as it does keys).
    const auto pad_frame = [&](const kin::GamepadState& state) {
        app.input().begin_frame();
        app.input().set_gamepad(state);
        menu.update(ctx);
        menu.render(ctx);
    };
    frame();
    kin::GamepadState pad{.connected = true, .buttons = 1u << static_cast<kin::u8>(kin::GamepadButton::DPadDown)};
    pad_frame(pad);
    pad.buttons = 1u << static_cast<kin::u8>(kin::GamepadButton::A);
    pad_frame(pad);
    frame();
    assert(play == 0 && options == 1); // update + render never double-activate
    pad.buttons = 1u << static_cast<kin::u8>(kin::GamepadButton::B);
    app.input().set_gamepad(pad);
    menu.render(ctx); // a zero-step frame parks cancellation for the next update
    assert(cancelled == 0);
    frame(); frame();
    assert(cancelled == 1);
}

void test_semantic_ui_server() {
    for (bool in_render : {false, true}) {
        kin::SceneManager scenes;
        scenes.push(std::make_unique<SemanticProbeScene>(in_render));
        std::ostringstream out;
        std::istringstream in(
            R"({"id":0,"method":"ui.snapshot"})" "\n"
            R"({"id":1,"method":"view.screenshot","params":{"path":"kin-semantic-test.png"}})" "\n"
            R"({"id":2,"method":"sim.tick"})" "\n"
            R"({"id":3,"method":"ui.snapshot"})" "\n"
            R"({"id":4,"method":"ui.action","params":{"id":"Play","action":"click"}})" "\n"
            R"({"id":5,"method":"ui.action","params":{"id":"Play"}})" "\n"
            R"({"id":6,"method":"view.screenshot","params":{"path":"kin-semantic-test.png"}})" "\n"
            R"({"id":7,"method":"scene.current"})" "\n"
            R"({"id":8,"method":"sim.tick","params":{"count":2}})" "\n"
            R"({"id":9,"method":"scene.current"})" "\n"
            R"({"id":10,"method":"ui.action","params":{"id":"player_name","action":"set_text","text":"Player α"}})" "\n"
            R"({"id":11,"method":"sim.tick"})" "\n"
            R"({"id":12,"method":"scene.current"})" "\n"
            R"({"id":13,"method":"ui.action","params":{"id":"missing"}})" "\n"
            R"({"id":14,"method":"ui.action","params":{"id":"Play"}})" "\n"
            R"({"id":15,"method":"sim.reset"})" "\n"
            R"({"id":16,"method":"sim.tick"})" "\n"
            R"({"id":17,"method":"scene.current"})" "\n"
            R"({"id":18,"method":"server.shutdown"})" "\n");
        assert(kin::run_scene_server({
            .window = {.title = "semantic UI", .width = 160, .height = 100},
            .mode = kin::ServerMode::Driven,
            .in = &in,
            .out = &out,
            .reset_scenes = [in_render](kin::SceneManager& reset) {
                reset.push(std::make_unique<SemanticProbeScene>(in_render));
            },
        }, scenes) == 0);
        const std::string text = out.str();
        if (in_render) assert(response_result(text, 0)->find("elements")->items().size() == 2);
        const auto* snapshot = response_result(text, 3);
        assert(snapshot && snapshot->find("elements")->items().size() == 2);
        const auto& button = snapshot->find("elements")->items().front();
        assert(button.string_at("role") == "button" && button.string_at("label") == "Play");
        assert(button.find("id")->is_string()); // stable 64-bit ids are lossless in JS
        assert(response_result(text, 4)->bool_at("queued"));
        assert(!response_result(text, 5)); // one action per tick
        assert(response_result(text, 7)->find("state")->int_at("clicks") == 0);
        assert(response_result(text, 9)->find("state")->int_at("clicks") == 1);
        assert(response_result(text, 12)->find("state")->string_at("text") == "Player α");
        assert(!response_result(text, 13));
        assert(response_result(text, 17)->find("state")->int_at("clicks") == 0);
        std::filesystem::remove("kin-semantic-test.png");
    }
}

void test_press_completes_a_ui2_click_and_frame_timing() {
    kin::SceneManager scenes;
    scenes.push(std::make_unique<ClickProbeScene>());

    // Hover for a step (ui2 resolves hover from the previous frame), then tap:
    // the button is down for one step and released at the start of the next,
    // where the click completes.
    std::ostringstream out;
    std::istringstream in(
        R"({"id":1,"method":"input.mouse","params":{"x":20,"y":20}})" "\n"
        R"({"id":2,"method":"sim.tick","params":{"count":1}})" "\n"
        R"({"id":3,"method":"input.mouse","params":{"x":20,"y":20,"button":"left","mode":"press"}})" "\n"
        R"({"id":4,"method":"input.action","params":{"name":"jump"}})" "\n"
        R"({"id":5,"method":"sim.tick","params":{"count":1}})" "\n"
        R"({"id":6,"method":"scene.current"})" "\n"
        R"({"id":7,"method":"sim.tick","params":{"count":1}})" "\n"
        R"({"id":8,"method":"scene.current"})" "\n"
        R"({"id":12,"method":"frame.timing","params":{"since":0}})" "\n"
        R"({"id":9,"method":"frame.timing","params":{"reset":true}})" "\n"
        R"({"id":10,"method":"frame.timing"})" "\n"
        R"({"id":11,"method":"server.shutdown"})" "\n");
    const int code = kin::run_scene_server({
        .window = {.title = "click", .width = 64, .height = 64},
        .mode = kin::ServerMode::Driven,
        .in = &in,
        .out = &out,
    }, scenes);
    assert(code == 0);
    const std::string text = out.str();

    const kin::JsonValue* pressed = response_result(text, 6);
    assert(pressed != nullptr);
    assert(pressed->find("state")->int_at("clicks") == 0);        // still down
    assert(pressed->find("state")->int_at("jump_releases") == 0);
    const kin::JsonValue* released = response_result(text, 8);
    assert(released != nullptr);
    assert(released->find("state")->int_at("clicks") == 1);
    assert(released->find("state")->int_at("jump_releases") == 1);

    const kin::JsonValue* timing = response_result(text, 9);
    assert(timing != nullptr);
    assert(timing->int_at("frames") == 3);
    assert(timing->int_at("window") == 600);
    assert(timing->find("mean")->find("total_ms")->as_number() >= 0.0);
    assert(timing->find("max")->find("update_ms")->as_number() >= timing->find("mean")->find("update_ms")->as_number());
    assert(!timing->contains("history")); // not asked for
    const kin::JsonValue* after_reset = response_result(text, 10);
    assert(after_reset != nullptr && after_reset->int_at("frames") == 0);
    // The frames after the first, each.
    const kin::JsonValue* since = response_result(text, 12);
    assert(since != nullptr); // (response_result's value is shared: `timing` is this now)
    const auto& history = since->find("history")->items();
    assert(history.size() == 2 && history[0].int_at("frame") == 1 && history[1].int_at("frame") == 2);
    assert(history[1].find("render_ms")->as_number() >= 0.0 && history[1].find("interval_ms")->as_number() == 0.0);
}

void test_input_key_drives_a_text_edit() {
    kin::SceneManager scenes;
    scenes.push(std::make_unique<TextEditProbeScene>());
    std::ostringstream out;
    std::istringstream in(
        R"({"id":1,"method":"input.mouse","params":{"x":20,"y":20}})" "\n"
        R"({"id":2,"method":"sim.tick","params":{"count":1}})" "\n"
        R"({"id":3,"method":"input.mouse","params":{"x":20,"y":20,"button":"left","mode":"press"}})" "\n"
        R"({"id":4,"method":"sim.tick","params":{"count":2}})" "\n"
        R"({"id":5,"method":"input.text","params":{"text":"hi"}})" "\n"
        R"({"id":6,"method":"sim.tick","params":{"count":1}})" "\n"
        R"({"id":22,"method":"view.screenshot","params":{"path":"kin-server-text-edit.png"}})" "\n"
        R"({"id":7,"method":"input.key","params":{"name":"Enter","modifiers":["shift"]}})" "\n"
        R"({"id":8,"method":"sim.tick","params":{"count":1}})" "\n"
        R"({"id":9,"method":"input.text","params":{"text":"yo"}})" "\n"
        R"({"id":10,"method":"sim.tick","params":{"count":1}})" "\n"
        R"({"id":11,"method":"input.key","params":{"name":"enter"}})" "\n"
        R"({"id":12,"method":"sim.tick","params":{"count":1}})" "\n"
        R"({"id":13,"method":"scene.current"})" "\n"
        R"({"id":14,"method":"input.key","params":{"name":"A","modifiers":["ctrl"]}})" "\n"
        R"({"id":15,"method":"sim.tick","params":{"count":1}})" "\n"
        R"({"id":16,"method":"input.key","params":{"name":"Backspace"}})" "\n"
        R"({"id":17,"method":"sim.tick","params":{"count":1}})" "\n"
        R"({"id":18,"method":"scene.current"})" "\n"
        R"({"id":19,"method":"input.key","params":{"name":"NoSuchKey"}})" "\n"
        R"({"id":20,"method":"input.key","params":{"name":"A","modifiers":["hyper"]}})" "\n"
        R"({"id":21,"method":"server.shutdown"})" "\n");
    const int code = kin::run_scene_server({
        .window = {.title = "keys", .width = 240, .height = 80},
        .mode = kin::ServerMode::Driven,
        .render = true,
        .in = &in,
        .out = &out,
    }, scenes);
    assert(code == 0);
    const std::string text = out.str();
    std::filesystem::remove(std::filesystem::current_path() / "kin-server-text-edit.png");

    // Shift+Enter started a new line; Enter alone submitted. The screenshot after
    // "hi" did not type it again.
    const kin::JsonValue* typed = response_result(text, 13);
    assert(typed != nullptr);
    assert(typed->find("state")->string_at("text") == "hi\nyo");
    assert(typed->find("state")->int_at("submits") == 1);
    // Ctrl+A then Backspace cleared it: the tapped Ctrl was released in between.
    const kin::JsonValue* cleared = response_result(text, 18);
    assert(cleared != nullptr);
    assert(cleared->find("state")->string_at("text").empty());
    // Unknown key names and modifiers are errors.
    const auto is_error = [&](kin::i64 id) {
        std::istringstream lines{text};
        std::string line;
        while (std::getline(lines, line)) {
            const kin::JsonParseResult parsed = kin::parse_json(line);
            if (parsed.ok() && parsed.value->int_at("id", -1) == id) {
                return parsed.value->find("error") != nullptr;
            }
        }
        return false;
    };
    assert(is_error(19));
    assert(is_error(20));
    assert(!is_error(14));
}

// input.drop: a file dropped on the window at a logical point, as from the desktop.
// Two players on two controllers: each one's jumps and releases, the first's
// stick, and controllers unplugged, for input.pad.
class PadProbeScene final : public kin::Scene {
public:
    std::string_view name() const override { return "PadProbe"; }

    void on_enter(kin::SceneContext& ctx) override {
        kin::InputMap map;
        map.bind("jump", kin::PadControl::A);
        map.set_axis({.name = "move_x", .sticks = {{.axis = kin::PadAxis::LeftX}}, .dead_zone = 0.0f});
        ctx.input.set_map(map);
    }

    void update(kin::SceneContext& ctx) override {
        for (kin::i32 p = 0; p < 2; ++p) {
            _jumps[p] += ctx.input.pressed(p, "jump") ? 1 : 0;
            _releases[p] += ctx.input.released(p, "jump") ? 1 : 0;
        }
        _axis = ctx.input.axis(0, "move_x");
        _unplugged += was_connected && !ctx.input.player_gamepad(0).connected ? 1 : 0;
        was_connected = ctx.input.player_gamepad(0).connected;
        _first_is_controller = ctx.input.gamepad() == ctx.input.player_gamepad(0);
    }

    void write_report(kin::JsonWriter& json) const override {
        json.field("jumps_1", _jumps[0]).field("jumps_2", _jumps[1]);
        json.field("releases_2", _releases[1]);
        json.field("axis_1", static_cast<double>(_axis));
        json.field("unplugged", _unplugged);
        json.field("first_is_controller", _first_is_controller);
    }

private:
    kin::i32 _jumps[2] = {};
    kin::i32 _releases[2] = {};
    kin::f32 _axis = 0.0f;
    kin::i32 _unplugged = 0;
    bool was_connected = false;
    bool _first_is_controller = false;
};

void test_input_pad() {
    kin::SceneManager scenes;
    scenes.push(std::make_unique<PadProbeScene>());
    std::ostringstream out;
    std::istringstream in(
        R"({"id":1,"method":"input.pad","params":{"player":1,"button":"a"}})" "\n"
        R"({"id":2,"method":"sim.tick","params":{"count":1}})" "\n"
        R"({"id":3,"method":"input.pad","params":{"player":0,"left_x":-1}})" "\n"
        R"({"id":4,"method":"sim.tick","params":{"count":2}})" "\n"
        R"({"id":5,"method":"scene.current"})" "\n"
        R"({"id":6,"method":"input.pad","params":{"player":0,"connected":false}})" "\n"
        R"({"id":7,"method":"sim.tick","params":{"count":1}})" "\n"
        R"({"id":8,"method":"scene.current"})" "\n"
        R"({"id":9,"method":"input.pad","params":{"player":9}})" "\n"
        R"({"id":10,"method":"input.pad","params":{"button":"left_trigger"}})" "\n"
        R"({"id":11,"method":"server.shutdown"})" "\n");
    const int code = kin::run_scene_server({
        .window = {.title = "pads", .width = 64, .height = 64},
        .mode = kin::ServerMode::Driven,
        .in = &in,
        .out = &out,
    }, scenes);
    assert(code == 0);
    const std::string text = out.str();
    const kin::JsonValue* first = response_result(text, 5);
    assert(first);
    const kin::JsonValue* state = first->find("state");
    assert(state && state->find("jumps_1")->as_number() == 0 && state->find("jumps_2")->as_number() == 1);
    assert(state->find("releases_2")->as_number() == 1 && state->find("axis_1")->as_number() == -1);
    assert(state->find("first_is_controller")->as_bool());
    const kin::JsonValue* second = response_result(text, 8);
    assert(second && second->find("state")->find("unplugged")->as_number() == 1);
    assert(second->find("state")->find("axis_1")->as_number() == 0);
    assert(!response_result(text, 9) && !response_result(text, 10));
}

// Taps, a long press and a finger's position, for input.touch.
class TouchProbeScene final : public kin::Scene {
public:
    std::string_view name() const override { return "TouchProbe"; }

    void on_enter(kin::SceneContext& ctx) override {
        kin::InputMap map;
        map.bind("jump", kin::TouchControl::Tap);
        map.bind("menu", kin::TouchControl::LongPress);
        ctx.input.set_map(map);
    }

    void update(kin::SceneContext& ctx) override {
        _taps += ctx.input.pressed("jump") ? 1 : 0;
        _menus += ctx.input.pressed("menu") ? 1 : 0;
        _fingers = static_cast<kin::i32>(ctx.input.touches().size());
        if (!ctx.input.touches().empty()) {
            _x = ctx.input.touches()[0].pos.x;
        }
    }

    void write_report(kin::JsonWriter& json) const override {
        json.field("taps", _taps).field("menus", _menus).field("fingers", _fingers);
        json.field("x", static_cast<double>(_x));
    }

private:
    kin::i32 _taps = 0;
    kin::i32 _menus = 0;
    kin::i32 _fingers = 0;
    kin::f32 _x = 0.0f;
};

void test_input_touch() {
    kin::SceneManager scenes;
    scenes.push(std::make_unique<TouchProbeScene>());
    std::ostringstream out;
    std::istringstream in(
        R"({"id":1,"method":"input.touch","params":{"x":10,"y":10,"space":"window"}})" "\n"
        R"({"id":2,"method":"sim.tick","params":{"count":2}})" "\n"
        R"({"id":3,"method":"input.touch","params":{"id":2,"x":30,"y":10,"mode":"down","space":"window"}})" "\n"
        R"({"id":4,"method":"sim.tick","params":{"count":1}})" "\n"
        R"({"id":5,"method":"scene.current"})" "\n"
        R"({"id":6,"method":"sim.tick","params":{"count":40}})" "\n"
        R"({"id":7,"method":"input.touch","params":{"id":2,"x":31,"y":10,"mode":"up","space":"window"}})" "\n"
        R"({"id":8,"method":"sim.tick","params":{"count":1}})" "\n"
        R"({"id":9,"method":"scene.current"})" "\n"
        R"({"id":10,"method":"input.touch","params":{"x":1}})" "\n"
        R"({"id":11,"method":"server.shutdown"})" "\n");
    const int code = kin::run_scene_server({
        .window = {.title = "touch", .width = 64, .height = 64},
        .mode = kin::ServerMode::Driven,
        .in = &in,
        .out = &out,
    }, scenes);
    assert(code == 0);
    const std::string text = out.str();
    const kin::JsonValue* held = response_result(text, 5);
    assert(held && held->find("state")->find("taps")->as_number() == 1 && held->find("state")->find("fingers")->as_number() == 1);
    assert(held->find("state")->find("x")->as_number() == 30);
    const kin::JsonValue* after = response_result(text, 9);
    // Held still over many steps: a long press, and no tap when it lifts.
    assert(after && after->find("state")->find("menus")->as_number() == 1 && after->find("state")->find("taps")->as_number() == 1);
    assert(after->find("state")->find("fingers")->as_number() == 0);
    assert(!response_result(text, 10));
}

void test_input_drop() {
    kin::SceneManager scenes;
    scenes.push(std::make_unique<DropProbeScene>());
    std::ostringstream out;
    std::istringstream in(
        R"({"id":1,"method":"input.drop","params":{"path":"art/hero.png","x":40,"y":20}})" "\n"
        R"({"id":2,"method":"sim.tick","params":{"count":1}})" "\n"
        R"({"id":3,"method":"input.drop","params":{"path":""}})" "\n"
        R"({"id":4,"method":"scene.current"})" "\n"
        R"({"id":5,"method":"server.shutdown"})" "\n");
    const int code = kin::run_scene_server({
        .window = {.title = "drops", .width = 120, .height = 80},
        .mode = kin::ServerMode::Driven,
        .render = true,
        .in = &in,
        .out = &out,
    }, scenes);
    assert(code == 0);
    const std::string text = out.str();
    const kin::JsonValue* state = response_result(text, 4);
    assert(state && state->find("state")->string_at("dropped") == "art/hero.png@40");
    assert(!response_result(text, 3)); // a path is needed
}

void test_sim_reset() {
    std::vector<kin::LogEvent> log_events;
    kin::set_logger_config({
        .min_level = kin::LogLevel::Debug,
        .format = kin::LogFormat::Text,
        .sdl_sink = false,
        .memory_events = &log_events,
    });

    kin::InputMap input;
    input.bind("tick", kin::Key::Space);

    kin::SceneManager scenes;
    scenes.push(std::make_unique<CounterScene>());
    const auto factory = [](kin::SceneManager& manager) {
        manager.push(std::make_unique<CounterScene>());
    };

    std::ostringstream out;
    std::istringstream in(
        R"({"id":1,"method":"input.action","params":{"name":"tick","mode":"press"}})" "\n"
        R"({"id":2,"method":"sim.tick","params":{"count":1}})" "\n"
        R"({"id":3,"method":"scene.current"})" "\n"
        R"({"id":4,"method":"sim.reset","params":{"seed":9}})" "\n"
        R"({"id":5,"method":"server.status"})" "\n"
        R"({"id":6,"method":"sim.tick","params":{"count":1}})" "\n"
        R"({"id":7,"method":"scene.current"})" "\n"
        R"({"id":8,"method":"server.shutdown"})" "\n");

    const int code = kin::run_scene_server({
        .window = {.title = "reset", .width = 64, .height = 64, .input_map = input},
        .mode = kin::ServerMode::Driven,
        .seed = 5,
        .in = &in,
        .out = &out,
        .reset_scenes = factory,
    }, scenes);
    assert(code == 0);
    assert(has_log_event(log_events, "server", "scene server starting"));
    assert(has_log_event(log_events, "server", "sim.reset completed"));
    assert(has_log_event(log_events, "server", "server shutdown requested"));
    kin::set_logger_config({
        .min_level = kin::LogLevel::Debug,
        .format = kin::LogFormat::Text,
        .sdl_sink = false,
    });

    const std::string text = out.str();

    // Before reset: the injected tick advanced the counter.
    const kin::JsonValue* before = response_result(text, 3);
    assert(before != nullptr);
    assert(before->find("state")->int_at("count") == 1);

    // sim.reset zeroes the frame counter.
    const kin::JsonValue* status = response_result(text, 5);
    assert(status != nullptr);
    assert(status->int_at("frame") == 0);

    // After reset: a fresh scene (count back to 0) and the new seed reached it.
    const kin::JsonValue* after = response_result(text, 7);
    assert(after != nullptr);
    assert(after->find("state")->int_at("count") == 0);
    const kin::u32 reseeded = kin::rng_u32(kin::make_key(9));
    assert(static_cast<kin::u32>(after->find("state")->int_at("observed")) == reseeded);
}

void test_sim_reset_unsupported_without_factory() {
    kin::SceneManager scenes;
    scenes.push(std::make_unique<CounterScene>());
    std::ostringstream out;
    std::istringstream in(
        R"({"id":1,"method":"sim.reset"})" "\n"
        R"({"id":2,"method":"server.shutdown"})" "\n");
    const int code = kin::run_scene_server({
        .window = {.title = "reset", .width = 64, .height = 64},
        .mode = kin::ServerMode::Driven,
        .in = &in,
        .out = &out,
    }, scenes);
    assert(code == 0);
    assert(out.str().find("sim.reset is not supported") != std::string::npos);
}

void test_view_screenshot() {
    kin::SceneManager scenes;
    scenes.push(std::make_unique<InputProbeScene>());

    // The server confines screenshot output to its working directory, so use a
    // relative filename (resolved against the current path) rather than an
    // absolute temp path.
    const std::string filename = "kin-server-screenshot.png";
    const std::filesystem::path path = std::filesystem::current_path() / filename;
    const std::filesystem::path escape_path =
        (std::filesystem::current_path() / ".." / "kin-server-escape.png").lexically_normal();
    std::filesystem::remove(path);
    std::filesystem::remove(escape_path);

    std::ostringstream out;
    std::istringstream in(
        R"({"id":1,"method":"sim.tick","params":{"count":1}})" "\n" +
        std::string(R"({"id":2,"method":"view.screenshot","params":{"path":")") + filename + R"("}})" "\n" +
        std::string(R"({"id":4,"method":"view.screenshot","params":{"path":"../kin-server-escape.png"}})") + "\n" +
        std::string(R"({"id":3,"method":"server.shutdown"})") + "\n");

    const int code = kin::run_scene_server({
        .window = {.title = "shot", .width = 64, .height = 48},
        .mode = kin::ServerMode::Driven,
        .in = &in,
        .out = &out,
    }, scenes);
    assert(code == 0);

    const kin::JsonValue* result = response_result(out.str(), 2);
    assert(result != nullptr);
    assert(result->int_at("width") == 64);
    assert(result->int_at("height") == 48);

    // A valid PNG file was written inside the working directory.
    assert(std::filesystem::exists(path));
    std::ifstream file(path, std::ios::binary);
    unsigned char signature[8] = {};
    file.read(reinterpret_cast<char*>(signature), sizeof(signature));
    assert(signature[0] == 0x89 && signature[1] == 'P' && signature[2] == 'N' && signature[3] == 'G');
    file.close();
    std::filesystem::remove(path);

    // A path that escapes the working directory is rejected (error envelope, no
    // "result") and no file is written outside the root.
    assert(response_result(out.str(), 4) == nullptr);
    assert(!std::filesystem::exists(escape_path));
}

// A scene a tool views (view.scene, view.frame): it shows the files it is
// given, and fills the frame with a colour telling the camera it is drawn
// through (red: ten times its zoom, green: its centre's x).
class ViewedScene final : public kin::Scene {
public:
    explicit ViewedScene(std::filesystem::path* shown) : _shown(shown) {}
    bool view_scene(const std::filesystem::path& file, const std::filesystem::path&, std::string& error) override {
        if (file.filename() == "broken.kinscene") {
            error = "broken.kinscene does not read";
            return false;
        }
        *_shown = file;
        return true;
    }
    // (Takes edits to an entity "box"; others refused, with why.)
    bool view_edit(const kin::JsonValue& entities, std::string& error) override {
        for (const kin::JsonValue& item : entities.items()) {
            if (item.string_at("id") != "box") {
                error = "no entity '" + item.string_at("id") + "'";
                return false;
            }
        }
        ++_edits;
        return true;
    }
    int edits() const { return _edits; }
    bool set_view_camera(const kin::Camera2D* camera) override {
        _camera = camera ? std::optional<kin::Camera2D>(*camera) : std::nullopt;
        _camera3d.reset();
        return true;
    }
    // (A 3D camera: red its field of view, green ten times its height, blue a hundred times its turn's y.)
    bool set_view_camera3d(const kin::Camera3D* camera) override {
        _camera3d = camera ? std::optional<kin::Camera3D>(*camera) : std::nullopt;
        return true;
    }
    void render(kin::SceneContext& ctx) override {
        if (_camera3d) {
            ctx.renderer.clear(kin::Color::rgb(static_cast<kin::u8>(_camera3d->fov), static_cast<kin::u8>(_camera3d->position.y * 10.0f),
                                               static_cast<kin::u8>(_camera3d->rotation.y * 100.0f + 0.5f)));
            return;
        }
        const auto red = static_cast<kin::u8>(_camera ? _camera->zoom * 10.0f : 0.0f);
        const auto green = static_cast<kin::u8>(_camera ? _camera->center().x : 0.0f);
        ctx.renderer.clear(kin::Color::rgb(red, green, 7));
    }

private:
    std::filesystem::path* _shown;
    int _edits = 0;
    std::optional<kin::Camera2D> _camera;
    std::optional<kin::Camera3D> _camera3d;
};

void test_view_scene_and_frame() {
    const std::string frame_name = "kin-server-view.rgba";
    const std::filesystem::path frame_path = std::filesystem::current_path() / frame_name;
    std::filesystem::remove(frame_path);
    std::filesystem::path shown;
    kin::SceneManager scenes;
    auto viewed = std::make_unique<ViewedScene>(&shown);
    const ViewedScene* viewed_scene = viewed.get();
    scenes.push(std::move(viewed));

    std::ostringstream out;
    std::istringstream in(
        R"({"id":1,"method":"view.scene","params":{"path":"edited.kinscene"}})" "\n"
        R"({"id":2,"method":"view.scene","params":{"path":"broken.kinscene"}})" "\n"
        R"({"id":3,"method":"view.scene","params":{"path":"../outside.kinscene"}})" "\n" +
        std::string(R"({"id":4,"method":"view.frame","params":{"path":")") + frame_name +
        R"(","width":32,"height":16,"x":40,"y":5,"zoom":3}})" "\n"
        R"({"id":5,"method":"view.frame","params":{"width":0,"height":16}})" "\n" +
        std::string(R"({"id":6,"method":"view.frame","params":{"path":")") + frame_name + ".3d" +
        R"(","width":8,"height":4,"camera3d":{"position":[0,4,2],"rotation":[0,0.6,0,0.8],"fov":50}}})" "\n"
        R"({"id":7,"method":"view.frame","params":{"width":8,"height":4,"camera3d":{"position":[0,1],"fov":50}}})" "\n"
        R"({"id":8,"method":"view.frame","params":{"width":8,"height":4,"camera3d":{"rotation":[0,0,0,0]}}})" "\n"
        R"({"id":9,"method":"view.edit","params":{"entities":[{"id":"box","components":{"Transform3D":{"position":[1,2,3]}}}]}})" "\n"
        R"({"id":10,"method":"view.edit","params":{"entities":[{"id":"ghost","components":{}}]}})" "\n"
        R"({"id":11,"method":"view.edit","params":{"entities":{}}})" "\n"
        R"({"id":12,"method":"server.shutdown"})" "\n");
    const int code = kin::run_scene_server({
        .window = {.title = "view", .width = 64, .height = 48},
        .mode = kin::ServerMode::Driven,
        .in = &in,
        .out = &out,
    }, scenes);
    assert(code == 0);
    const std::string text = out.str();

    // The scene shows the file (in the working folder); one it can't read, or
    // one outside the folder, is refused with why.
    assert(response_result(text, 1) != nullptr && shown == std::filesystem::current_path() / "edited.kinscene");
    assert(response_result(text, 2) == nullptr && text.find("broken.kinscene does not read") != std::string::npos);
    assert(response_result(text, 3) == nullptr && text.find("invalid scene path") != std::string::npos);

    // Drawn at the size asked for, through the camera asked for, its pixels raw.
    const kin::JsonValue* frame = response_result(text, 4);
    assert(frame != nullptr && frame->int_at("width") == 32 && frame->int_at("height") == 16);
    assert(std::filesystem::file_size(frame_path) == 32u * 16u * 4u);
    std::ifstream file(frame_path, std::ios::binary);
    unsigned char pixel[4] = {};
    file.read(reinterpret_cast<char*>(pixel), sizeof(pixel));
    assert(pixel[0] == 30 && pixel[1] == 40 && pixel[2] == 7 && pixel[3] == 255);
    file.close();
    std::filesystem::remove(frame_path);
    assert(response_result(text, 5) == nullptr && text.find("width and height of 1 to 8192") != std::string::npos);

    // Through a 3D camera: where it is, its turn, its field of view; one that is not a camera refused with why.
    const kin::JsonValue* frame3d = response_result(text, 6);
    const std::filesystem::path frame3d_path = std::filesystem::path(frame_path) += ".3d";
    assert(frame3d != nullptr && frame3d->int_at("width") == 8 && std::filesystem::file_size(frame3d_path) == 8u * 4u * 4u);
    std::ifstream file3d(frame3d_path, std::ios::binary);
    file3d.read(reinterpret_cast<char*>(pixel), sizeof(pixel));
    assert(pixel[0] == 50 && pixel[1] == 40 && pixel[2] == 60 && pixel[3] == 255);
    file3d.close();
    std::filesystem::remove(frame3d_path);
    assert(response_result(text, 7) == nullptr && text.find("camera3d needs position [x, y, z]") != std::string::npos);
    assert(response_result(text, 8) == nullptr && text.find("is not a turn") != std::string::npos);

    // Edits in place: handed to the scene; one it refuses, or one that is not a list, fails with why.
    const kin::JsonValue* edited = response_result(text, 9);
    assert(edited != nullptr && edited->int_at("edited") == 1 && viewed_scene->edits() == 1);
    assert(response_result(text, 10) == nullptr && text.find("no entity 'ghost'") != std::string::npos);
    assert(response_result(text, 11) == nullptr && text.find("view.edit needs entities") != std::string::npos);
}

void test_view_scene_refused_by_default() {
    kin::SceneManager scenes;
    scenes.push(std::make_unique<InputProbeScene>());
    std::ostringstream out;
    std::istringstream in(
        R"({"id":1,"method":"view.scene","params":{"path":"edited.kinscene"}})" "\n"
        R"({"id":2,"method":"view.frame","params":{"width":8,"height":8}})" "\n"
        R"({"id":3,"method":"view.frame","params":{"width":8,"height":8,"camera3d":{}}})" "\n"
        R"({"id":4,"method":"view.edit","params":{"entities":[]}})" "\n"
        R"({"id":5,"method":"server.shutdown"})" "\n");
    const int code = kin::run_scene_server({
        .window = {.title = "view", .width = 64, .height = 48},
        .mode = kin::ServerMode::Driven,
        .in = &in,
        .out = &out,
    }, scenes);
    assert(code == 0);
    assert(response_result(out.str(), 4) == nullptr && out.str().find("takes no edits in place") != std::string::npos);
    // A scene that does not say how shows no other files, and has no world to
    // draw through another camera.
    assert(response_result(out.str(), 1) == nullptr && out.str().find("does not show other scene files") != std::string::npos);
    assert(response_result(out.str(), 2) == nullptr && out.str().find("no world to draw") != std::string::npos);
    assert(response_result(out.str(), 3) == nullptr && out.str().find("no 3D world to draw") != std::string::npos);
}

} // namespace

// A scene that records its teardown: the server must pop (and destroy) the
// stack before it returns, while its renderer is still alive, so scene-owned
// GPU resources are never freed into a dead device.
class TeardownScene final : public kin::Scene {
public:
    TeardownScene(bool* exited, bool* destroyed) : _exited(exited), _destroyed(destroyed) {}
    ~TeardownScene() override { *_destroyed = true; }
    std::string_view name() const override { return "Teardown"; }
    void on_exit(kin::SceneContext&) override { *_exited = true; }

private:
    bool* _exited;
    bool* _destroyed;
};

void test_server_tears_down_scenes_before_returning() {
    bool exited = false;
    bool destroyed = false;
    kin::SceneManager scenes;
    scenes.push(std::make_unique<TeardownScene>(&exited, &destroyed));
    std::ostringstream out;
    std::istringstream in(R"({"id":1,"method":"server.shutdown"})" "\n");
    const int code = kin::run_scene_server({
        .window = {.title = "server", .width = 64, .height = 64},
        .mode = kin::ServerMode::Driven,
        .in = &in,
        .out = &out,
    }, scenes);
    assert(code == 0);
    assert(exited);
    assert(destroyed);
    assert(scenes.depth() == 0);
}

// The live inspector reads the last frame's UI without laying it out again,
// which would consume the player's input.
void test_live_semantic_snapshot_preserves_player_input() {
    kin::App app{{.mode = kin::AppMode::Headless}};
    auto& window = app.create_window({.title = "semantic inspector", .width = 160, .height = 100, .hidden = true});
    kin::Renderer2D renderer{window};
    kin::SceneManager scenes;
    auto scene = std::make_unique<SemanticProbeScene>(true);
    auto* ui = scene->ui_context();
    scenes.push(std::move(scene));
    kin::ServerContext ctx{app, window, renderer, scenes, {}, kin::make_key(1), 1.0f / 60.0f, false};
    ctx.realize_scenes();
    app.input().set_key_pressed(kin::Key::A);
    std::istringstream in(R"({"id":1,"method":"ui.snapshot"})" "\n"
                          R"({"id":2,"method":"ui.action","params":{"id":"Play"}})" "\n");
    std::ostringstream out;
    kin::LiveInspector inspector{in, out};
    for (int i = 0; i < 2000 && !inspector.finished(); ++i) {
        inspector.poll(ctx, nullptr);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(inspector.finished());
    inspector.flush();
    assert(response_result(out.str(), 1)->find("elements")->items().empty());
    assert(!response_result(out.str(), 2));
    assert(ui->elements().empty() && app.input().frame_pressed(kin::Key::A));
    ctx.shutdown_scenes();
}

// --inspect: a windowed game answers between frames, and is held and stepped.
void test_live_inspector() {
    kin::App app{{.mode = kin::AppMode::Headless}};
    kin::Window& window = app.create_window({.title = "inspected", .width = 32, .height = 32, .hidden = true});
    kin::Renderer2D renderer{window};
    kin::SceneManager scenes;
    scenes.push(std::make_unique<KinSnapshotScene>());
    kin::ServerContext ctx{app, window, renderer, scenes, {.title = "inspected"}, kin::make_key(1), 1.0f / 60.0f, false};
    ctx.realize_scenes();
    kin::GameInfo game;
    game.title = "Inspected";
    // Two frames drawn: their times, as scene_app records them.
    ctx.record_frame_timing({.update_ms = 1.0, .render_ms = 2.0, .present_ms = 0.5, .interval_ms = 16.5});
    ctx.record_frame_timing({.update_ms = 1.5, .render_ms = 3.0, .present_ms = 0.25, .interval_ms = 17.0});

    std::istringstream in(
        R"({"id":10,"method":"frame.timing","params":{"since":-1}})" "\n"
        R"({"id":1,"method":"game.info"})" "\n"
        R"({"id":2,"method":"world.snapshot","params":{"raw":false,"events":false,"animations":false}})" "\n"
        R"({"id":3,"method":"sim.pause"})" "\n"
        R"({"id":4,"method":"sim.tick"})" "\n"
        R"({"id":5,"method":"input.key","params":{"name":"A"}})" "\n"
        R"({"id":6,"method":"sim.step","params":{"count":2}})" "\n"
        R"({"id":11,"method":"ui.action","params":{"id":"Play"}})" "\n"
        "\n"
        "not json\n");
    std::ostringstream out;
    kin::LiveInspector inspector{in, out};
    assert(inspector.admit_update() && inspector.frame() == 1); // running: every update

    // Requests are read on a thread: poll until all of them are answered.
    for (int i = 0; i < 2000 && !inspector.finished(); ++i) {
        inspector.poll(ctx, &game);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(inspector.finished());
    inspector.flush(); // answers are written on a thread of their own
    const std::string text = out.str();
    assert(response_result(text, 1)->string_at("title") == "Inspected");
    const kin::JsonValue* frames = response_result(text, 10);
    assert(frames && frames->int_at("frames") == 2 && frames->find("max")->find("total_ms")->as_number() == 4.75);
    const auto& history = frames->find("history")->items();
    assert(history.size() == 2 && history[0].int_at("frame") == 0 && history[1].find("interval_ms")->as_number() == 17.0);
    const kin::JsonValue* snapshot = response_result(text, 2);
    assert(snapshot && !snapshot->contains("world") && snapshot->contains("kin"));
    assert(text.find("server.actor") != std::string::npos);
    const kin::JsonValue* paused = response_result(text, 3);
    assert(paused && paused->bool_at("paused") && paused->int_at("frame") == 1);
    assert(!response_result(text, 4) && text.find("sim.tick drives a headless server") != std::string::npos);
    assert(!response_result(text, 5)); // input belongs to the player
    assert(!response_result(text, 11)); // semantic actions also belong to the player
    assert(response_result(text, 6)->bool_at("paused"));
    assert(text.find("invalid JSON") != std::string::npos);

    // The input ended, so nothing could resume it: it runs again (the steps with it).
    assert(!inspector.paused());
    assert(inspector.admit_update() && inspector.admit_update() && inspector.frame() == 3);

    // Held: no updates but the steps asked for.
    std::istringstream more(R"({"id":7,"method":"sim.step","params":{"count":2}})" "\n"
                            R"({"id":8,"method":"server.status"})" "\n"
                            R"({"id":9,"method":"server.shutdown"})" "\n");
    std::ostringstream answers;
    kin::LiveInspector held{more, answers};
    for (int i = 0; i < 2000 && answers.str().find("bye") == std::string::npos; ++i) {
        held.poll(ctx, &game);
        held.flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(!app.running()); // server.shutdown quits the game
    assert(response_result(answers.str(), 8)->bool_at("paused"));
    // finished() came with the same poll as the shutdown, so the hold may be
    // lifted already; held only if the input is still open.
    if (held.paused()) {
        assert(held.admit_update() && held.admit_update() && !held.admit_update());
    }
}

// A tool slow to read (a large answer filling the pipe) holds the inspector's
// writer, not the game: poll() returns at once, and the answers follow, in
// order, once the tool reads again.
void test_live_inspector_slow_tool() {
    kin::App app{{.mode = kin::AppMode::Headless}};
    kin::Window& window = app.create_window({.title = "slow", .width = 32, .height = 32, .hidden = true});
    kin::Renderer2D renderer{window};
    kin::SceneManager scenes;
    scenes.push(std::make_unique<KinSnapshotScene>());
    kin::ServerContext ctx{app, window, renderer, scenes, {.title = "slow"}, kin::make_key(1), 1.0f / 60.0f, false};
    ctx.realize_scenes();

    // A stream whose flush waits until the tool reads.
    struct Gate : std::stringbuf {
        std::mutex mutex;
        std::condition_variable opened;
        bool open = false;
        int sync() override {
            std::unique_lock<std::mutex> lock(mutex);
            opened.wait(lock, [this] { return open; });
            return std::stringbuf::sync();
        }
    } gate;
    std::ostream out(&gate);
    // The tool reads at last after 1.5 s (so a held game fails below, not hangs).
    std::thread reads_late([&gate] {
        std::unique_lock<std::mutex> lock(gate.mutex);
        gate.opened.wait_for(lock, std::chrono::milliseconds(1500), [&gate] { return gate.open; });
        gate.open = true;
        gate.opened.notify_all();
    });
    std::istringstream in(R"({"id":1,"method":"ping"})" "\n"
                          R"({"id":2,"method":"server.status"})" "\n");
    kin::LiveInspector inspector{in, out};
    for (int i = 0; i < 2000 && !inspector.finished(); ++i) {
        const auto start = std::chrono::steady_clock::now();
        inspector.poll(ctx, nullptr);
        assert(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(500)); // never held
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(inspector.finished());
    {
        std::lock_guard<std::mutex> lock(gate.mutex);
        gate.open = true;
    }
    gate.opened.notify_all();
    reads_late.join();
    inspector.flush();
    const std::string text = gate.str();
    assert(response_result(text, 1) && response_result(text, 2));
    assert(text.find("\"id\":1") < text.find("\"id\":2"));
}

// --inspect debugs the game's scripts: a script stopped is held inside the
// inspector, which answers debug.* and the status then, and the rest after.
void test_live_inspector_debugs_scripts() {
    kin::App app{{.mode = kin::AppMode::Headless}};
    kin::Window& window = app.create_window({.title = "debugged", .width = 32, .height = 32, .hidden = true});
    kin::Renderer2D renderer{window};
    kin::SceneManager scenes;
    scenes.push(std::make_unique<KinSnapshotScene>());
    kin::ServerContext ctx{app, window, renderer, scenes, {.title = "debugged"}, kin::make_key(1), 1.0f / 60.0f, false};
    ctx.realize_scenes();
    kin::GameInfo game;
    game.title = "Debugged";
    kin::ScriptDebugger& debugger = kin::script_debugger();
    assert(!debugger.waiting());
    {
        std::istringstream in(
            R"({"id":1,"method":"debug.state"})" "\n"
            R"({"id":2,"method":"server.status"})" "\n"
            R"({"id":3,"method":"game.info"})" "\n"
            R"({"id":4,"method":"debug.step","params":{"how":"sideways"}})" "\n"
            R"({"id":5,"method":"debug.step","params":{"how":"into"}})" "\n"
            R"({"id":6,"method":"debug.continue"})" "\n");
        std::ostringstream out;
        kin::LiveInspector inspector{in, out};
        assert(debugger.waiting());
        debugger.set_breakpoints({{"scripts/a.lua", {3}}});
        assert(debugger.armed());

        // A script (here the test, as Lua would) stops: the tool's requests
        // are answered from inside, until one says how to go on.
        kin::ScriptStop stop{.reason = "breakpoint"};
        stop.frames.push_back({.function = "on_update", .file = "scripts/a.lua", .line = 3,
                               .locals = {{.name = "dt", .value = "0.5", .kind = "number"}}});
        assert(debugger.stopped(stop, 1, 1) == kin::ScriptResume::Into);
        assert(!debugger.stop() && debugger.seeking());

        for (int i = 0; i < 2000 && !inspector.finished(); ++i) {
            inspector.poll(ctx, &game);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        inspector.flush();
        const std::string text = out.str();
        const kin::JsonValue* state = response_result(text, 1);
        assert(state && state->bool_at("stopped") && state->string_at("reason") == "breakpoint" && state->int_at("serial") > 0);
        const kin::JsonValue& frame = state->find("frames")->items().at(0);
        assert(frame.string_at("function") == "on_update" && frame.string_at("file") == "scripts/a.lua" && frame.int_at("line") == 3);
        assert(frame.find("locals")->items().at(0).string_at("value") == "0.5" && frame.find("upvalues")->items().empty());
        assert(response_result(text, 2)->bool_at("stopped"));
        assert(!response_result(text, 4) && text.find("how must be into, over or out") != std::string::npos);
        assert(response_result(text, 5));
        // game.info waited for the script to go on.
        assert(response_result(text, 3)->string_at("title") == "Debugged");
        assert(text.find("\"id\":3") > text.find("\"id\":5"));
        assert(!response_result(text, 6) && text.find("no script is stopped") != std::string::npos);
    }
    assert(!debugger.waiting() && !debugger.armed() && debugger.breakpoints()->empty());
    {
        // Breakpoints and a pause are taken as they are read, before any poll:
        // a game's first update stops already.
        std::istringstream in(R"({"id":7,"method":"debug.breakpoints","params":{"files":{"scripts/a.lua":[3,5,0]}}})" "\n"
                              R"({"id":8,"method":"debug.pause"})" "\n");
        std::ostringstream out;
        kin::LiveInspector inspector{in, out};
        for (int i = 0; i < 2000 && !debugger.seeking(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        assert(debugger.seeking()); // the pause asked for
        assert((debugger.breakpoints()->at("scripts/a.lua") == std::set<kin::i32>{3, 5}));
        for (int i = 0; i < 2000 && !response_result((inspector.flush(), out.str()), 8); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1)); // answered on the reader's thread
        }
        assert(response_result(out.str(), 7)->int_at("breakpoints") == 2);
        assert(response_result(out.str(), 8));
    }
    // The inspector gone, nothing stops any more.
    assert(!debugger.waiting() && !debugger.armed() && debugger.breakpoints()->empty());
}

// The automation methods (kin.automation/1) on a world + semantic UI probe.
struct AutomationHealth {
    kin::i32 value = 12;
    kin::u64 serial = ~kin::u64{0};
};

class AutomationProbeScene final : public kin::Scene {
public:
    explicit AutomationProbeScene(std::filesystem::path development_root = {}) {
        _world.components()
            .native<AutomationHealth>("Health")
            .field("value", &AutomationHealth::value)
            .field("serial", &AutomationHealth::serial);
        _world.entities().create({.name = "Hero", .authored_id = "hero"}).set(AutomationHealth{});
#if KIN_ENABLE_DEVELOPMENT
        if (!development_root.empty()) {
            _development = std::make_unique<kin::DevelopmentSession>(development_root);
        }
#else
        (void)development_root;
#endif
    }
    kin::EcsWorld* world() override { return &_world; }
    kin::ui2::Context* ui_context() override { return &_ui; }
#if KIN_ENABLE_DEVELOPMENT
    kin::DevelopmentSession* development_session() override { return _development.get(); }
#endif
    void update(kin::SceneContext& ctx) override { draw(ctx); }
    void render(kin::SceneContext& ctx) override { draw(ctx); }
    void write_report(kin::JsonWriter& json) const override {
        json.field("clicks", clicks).field("wide", ~kin::u64{0});
    }
    int clicks = 0;

private:
    void draw(kin::SceneContext& ctx) {
        _ui.begin(ctx.input, ctx.renderer, ctx.dt);
        for (int i = 0; i < 2; ++i) {
            const auto group = kin::ui2::make_id(i == 0 ? "left" : "right");
            _ui.describe(group, "group", i == 0 ? "Left" : "Right", {float(i * 100), 0, 100, 100});
            _ui.semantic_key(group, i == 0 ? "left" : "right");
            kin::ui2::Button button{.id = kin::ui2::make_id(i == 0 ? "fire-left" : "fire-right"),
                                    .bounds = {float(i * 100), 0, 80, 20},
                                    .label = "Fire",
                                    .enabled = i == 0};
            _ui.semantic_key(button.id, i == 0 ? "fire-left" : "fire-right");
            kin::ui2::run(_ui, button);
            _ui.semantic_parent(button.id, group);
            clicks += button.clicked;
        }
        _ui.end();
    }

    kin::EcsWorld _world;
    kin::ui2::Context _ui;
#if KIN_ENABLE_DEVELOPMENT
    std::unique_ptr<kin::DevelopmentSession> _development;
#endif
};

kin::JsonValue parse_or_fail(std::string_view text) {
    auto parsed = kin::parse_json(text);
    assert(parsed.ok());
    return std::move(*parsed.value);
}

kin::JsonValue call(kin::ServerContext& ctx, std::string_view method, std::string_view params = "{}") {
    const auto response = kin::dispatch_server_command(ctx, nullptr, method, parse_or_fail(params));
    if (!response.ok) {
        std::fprintf(stderr, "%s: %s\n", std::string(method).c_str(), response.error.c_str());
    }
    assert(response.ok);
    return parse_or_fail(response.result_json);
}

void reject(kin::ServerContext& ctx, std::string_view method, std::string_view params) {
    assert(!kin::dispatch_server_command(ctx, nullptr, method, parse_or_fail(params)).ok);
}

void test_automation_methods() {
    kin::App app{{.mode = kin::AppMode::Headless}};
    auto& window = app.create_window({.width = 240, .height = 120, .hidden = true});
    kin::Renderer2D renderer{window};
    kin::SceneManager scenes;
    scenes.push(std::make_unique<AutomationProbeScene>());
    kin::ServerContext ctx{app, window, renderer, scenes, {}, kin::make_key(7), 1.0f / 60, false};
    ctx.realize_scenes();
    ctx.set_reset_factory([](auto& manager) { manager.push(std::make_unique<AutomationProbeScene>()); });
    auto schema = call(ctx, "server.schema");
    assert(schema.string_at("schema") == "kin.automation/1" && schema.find("methods")->contains("ui.query"));
    // Every advertised method is dispatched (none is unknown to the server).
    for (const auto& [name, descriptor] : schema.find("methods")->members()) {
        if (!descriptor.bool_at("driven", true) || name == "server.shutdown") {
            continue;
        }
        // Parameters each handler refuses or ignores harmlessly (no file is written).
        const auto response = kin::dispatch_server_command(
            ctx, nullptr, name, parse_or_fail(R"({"path":"../outside.png","count":-1,"name":""})"));
        assert(response.ok || response.error.find("unknown method") == std::string::npos);
    }
    ctx.reset(std::nullopt);
    auto caps = call(ctx, "server.capabilities");
    assert(caps.bool_at("ui") && caps.bool_at("world") && caps.bool_at("reset"));
    assert(caps.find("methods")->bool_at("ui.action") && !caps.find("methods")->bool_at("sim.step"));
    auto entities = call(ctx, "world.query", R"({"selector":{"authored_id":"hero","all":["Health"]}})");
    assert(entities.int_at("count") == 1 && !entities.bool_at("truncated"));
    const auto& hero = entities.find("entities")->items().front();
    assert(hero.find("id")->is_string());
    bool precise = false;
    for (const auto& component : hero.find("components")->items()) {
        for (const auto& field : component.find("fields")->items()) {
            if (field.string_at("name") == "serial") {
                precise = field.string_at("value") == "18446744073709551615";
            }
        }
    }
    assert(precise);
    assert(call(ctx, "world.query", R"({"selector":{"none":["Health"]}})").int_at("count") == 0);
    assert(call(ctx, "world.query", R"({"limit":1})").bool_at("truncated") == false);
    reject(ctx, "world.query", R"({"selector":{"all":["Missing"]}})");
    reject(ctx, "world.query", R"({"limit":1.5})");
    reject(ctx, "world.query", R"({"limit":1e99})");
    auto ui = call(ctx, "ui.query", R"({"selector":{"role":"button","label":"Fire"}})");
    assert(ui.int_at("count") == 2);
    auto scoped = call(ctx, "ui.query", R"({"selector":{"role":"button","within":{"key":"left"}}})");
    assert(scoped.int_at("count") == 1);
    reject(ctx, "ui.action", R"({"selector":{"role":"button","label":"Fire"}})");
    reject(ctx, "ui.action", R"({"selector":{"key":"fire-right"}})");
    reject(ctx, "ui.action", R"({"id":"Fire","selector":{"key":"fire-left"}})");
    reject(ctx, "ui.action", R"({"selector":{"key":"fire-left"},"action":"set_number","text":"many"})");
    reject(ctx, "ui.query", R"({"selector":{"key":"absent","within":{"typo":true}}})");
    reject(ctx, "ui.query", R"({"selector":{"enabled":"yes"}})");
    call(ctx, "ui.action", R"({"selector":{"key":"fire-left","within":{"key":"left"}}})");
    auto* probe = static_cast<AutomationProbeScene*>(scenes.top_mut());
    assert(probe->clicks == 0);
    ctx.step();
    assert(probe->clicks == 1);
    call(ctx, "ui.action", R"({"selector":{"key":"fire-left"},"action":"focus"})");
    ctx.step();
    assert(call(ctx, "ui.query", R"({"selector":{"focused":true}})").int_at("count") == 1);
    ctx.set_automation_mode(kin::AutomationMode::Inspect);
    caps = call(ctx, "server.capabilities");
    assert(caps.string_at("mode") == "inspect");
    assert(!caps.find("methods")->bool_at("ui.action") && caps.find("methods")->bool_at("sim.step"));
    ctx.set_automation_mode(kin::AutomationMode::Driven);

    // Recording: one header shape, each command, each step's checkpoint.
    const auto path =
        std::filesystem::temp_directory_path() /
        ("kin-automation-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".jsonl");
    ctx.start_recording(path, nullptr, 7);
    call(ctx, "ui.action", R"({"selector":{"key":"fire-left"}})");
    call(ctx, "sim.tick", R"({"count":1})");
    call(ctx, "scene.current");
    ctx.stop_recording();
    {
        std::ifstream file(path);
        std::string line;
        int requests = 0;
        int commands = 0;
        int frames = 0;
        bool header = false;
        bool wide = false;
        while (std::getline(file, line)) {
            const auto row = parse_or_fail(line);
            const std::string kind = row.string_at("kind");
            if (kind == "header") {
                header =
                    row.string_at("seed") == "7" && row.find("rng_key") && row.find("fixed_dt") && row.find("game_id");
            }
            requests += kind == "request";
            commands += kind == "command";
            frames += kind == "frame";
            wide |= line.find("18446744073709551615") != std::string::npos;
        }
        assert(header && requests == 3 && commands == 3 && frames == 1 && wide);
    }
    std::filesystem::remove(path);

    call(ctx, "input.key", R"({"name":"Space","mode":"hold"})");
    ctx.step();
    assert(app.input().held(kin::Key::Space));
    const kin::InputMap old_map = app.input().map();
    reject(ctx, "sim.reset", R"({"seed":9007199254740992})");
    reject(ctx, "sim.reset", R"({"seed":"18446744073709551616"})");
    reject(ctx, "sim.reset", R"({"seed":-1})");
    call(ctx, "sim.reset", R"({"seed":"18446744073709551615"})");
    assert(!app.input().held(kin::Key::Space) && app.input().map() == old_map && ctx.frame() == 0);
    assert(ctx.rng() == kin::make_key(~kin::u64{0}));
    ctx.step();
    assert(!app.input().held(kin::Key::Space));
    ctx.shutdown_scenes();
}

#if KIN_ENABLE_DEVELOPMENT
// automation.snapshot carries a development session's summary, not its sources:
// recordings and scenario steps would otherwise copy every content file's hash.
void test_development_status_endpoints() {
    const auto root = std::filesystem::temp_directory_path() /
                      ("kin-dev-status-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    std::ofstream(root / "value.txt") << "one";
    {
        kin::App app{{.mode = kin::AppMode::Headless}};
        auto& window = app.create_window({.width = 64, .height = 64, .hidden = true});
        kin::Renderer2D renderer{window};
        kin::SceneManager scenes;
        scenes.push(std::make_unique<AutomationProbeScene>(root));
        kin::ServerContext ctx{app, window, renderer, scenes, {}, kin::make_key(7), 1.0f / 60, false};
        ctx.realize_scenes();
        assert(call(ctx, "server.capabilities").bool_at("development"));
        const auto status = call(ctx, "dev.status");
        assert(status.find("sources")->items().size() == 1 && status.find("targets"));
        const auto snapshot = call(ctx, "automation.snapshot");
        const auto* development = snapshot.find("scenes")->items().front().find("development");
        assert(development && development->string_at("revision") == status.string_at("revision"));
        assert(!development->contains("sources") && !development->contains("targets"));
        const auto queued = call(ctx, "dev.reload", R"({"changes":{"value.txt":"two"}})");
        assert(queued.bool_at("queued") && queued.find("changes")->contains("value.txt"));
        reject(ctx, "dev.reload", R"({"expected_revision":"stale"})");
        ctx.shutdown_scenes();
    }
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}
#endif

int main() {
    test_json_parser();
    test_driven_server();
    test_world_snapshot_reports_animation_players();
    test_world_snapshot_reports_kin_metadata_and_payload_switches();
    test_animation_diagnostics_endpoint();
    test_animation_diagnostics_endpoint_missing_provider_errors();
    test_ui_snapshot_and_pointer_input();
    test_semantic_ui_server();
    test_controller_menu_scene();
    test_press_completes_a_ui2_click_and_frame_timing();
    test_live_inspector_slow_tool();
    test_live_inspector_debugs_scripts();
    test_input_key_drives_a_text_edit();
    test_input_pad();
    test_input_touch();
    test_input_drop();
    test_sim_reset();
    test_sim_reset_unsupported_without_factory();
    test_view_screenshot();
    test_view_scene_and_frame();
    test_view_scene_refused_by_default();
    test_server_tears_down_scenes_before_returning();
    test_live_inspector();
    test_live_semantic_snapshot_preserves_player_input();
    test_automation_methods();
#if KIN_ENABLE_DEVELOPMENT
    test_development_status_endpoints();
#endif
    return 0;
}
