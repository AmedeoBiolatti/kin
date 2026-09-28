#include <kin/anim/player.hpp>
#include <kin/anim/anim_format.hpp>
#include <kin/core/json.hpp>
#include <kin/core/json_value.hpp>
#include <kin/ecs/world.hpp>
#include <kin/ecs/system.hpp>
#include <kin/core/rng.hpp>
#include <kin/platform/log.hpp>
#include <kin/runtime/scene_server.hpp>
#include <kin/scene/scene_manager.hpp>

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
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
    }

    void write_report(kin::JsonWriter& json) const override {
        json.field("clicks", _clicks);
        json.field("typed", std::string_view{_typed});
    }

private:
    kin::i32 _clicks = 0;
    std::string _typed;
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
        R"({"id":9,"method":"server.shutdown"})" "\n");

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
    assert(text.find("ui.snapshot is unavailable after the legacy UI module was removed") != std::string::npos);

    // The injected click activated the button.
    const kin::JsonValue* after_click = response_result(text, 5);
    assert(after_click != nullptr);
    assert(after_click->find("state")->int_at("clicks") >= 1);

    // The injected text reached the scene.
    const kin::JsonValue* after_text = response_result(text, 8);
    assert(after_text != nullptr);
    assert(after_text->find("state")->string_at("typed") == "hello");
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

int main() {
    test_json_parser();
    test_driven_server();
    test_world_snapshot_reports_animation_players();
    test_world_snapshot_reports_kin_metadata_and_payload_switches();
    test_animation_diagnostics_endpoint();
    test_animation_diagnostics_endpoint_missing_provider_errors();
    test_ui_snapshot_and_pointer_input();
    test_sim_reset();
    test_sim_reset_unsupported_without_factory();
    test_view_screenshot();
    test_server_tears_down_scenes_before_returning();
    return 0;
}
