#include <kin/core/json.hpp>
#include <kin/ecs/system.hpp>
#include <kin/platform/input.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/runtime/game_info.hpp>
#include <kin/runtime/run_report.hpp>
#include <kin/runtime/scene_app.hpp>
#include <kin/scene/scene.hpp>
#include <kin/scene/scene_manager.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace graph_demo {
namespace {

constexpr kin::Vec2f logical_size{960.0f, 540.0f};

struct Transform {
    kin::Vec2f pos;
};

struct Velocity {
    kin::Vec2f value;
};

struct Box {
    kin::Vec2f size{42.0f, 42.0f};
    kin::Color color = kin::Color::rgb(230, 170, 80);
};

struct Pulse {
    kin::f32 speed = 1.0f;
    kin::f32 offset = 0.0f;
};

struct Bounds {
    kin::Vec2f min{24.0f, 96.0f};
    kin::Vec2f max{936.0f, 500.0f};
};

struct Lifetime {
    kin::f32 seconds = 0.0f;
};

// Carries a field because the component registry only takes non-empty types.
struct CommandSpawned {
    bool spawned = true;
};

struct DemoState {
    bool paused = false;
    kin::i32 spawned = 0;
    kin::i32 command_created = 0;
    kin::i32 command_destroyed = 0;
    kin::i32 graph_task_runs = 0;
    kin::i32 parallel_task_runs = 0;
    kin::i32 parallel_batches = 0;
    kin::f32 time = 0.0f;
    kin::f32 spawn_accumulator = 0.0f;
    bool saw_running_parallel = false;
    bool self_test_checked = false;
};

kin::GameInfo make_game_info() {
    kin::InputMap input;
    input.bind("quit", kin::Key::Escape);
    input.bind("pause", kin::Key::Space);
    input.bind("reset", kin::Key::R);

    return {
        .id = "ecs_graph_demo",
        .title = "ECS Graph Demo",
        .version = "0.1",
        .description = "Small Kin ECS dependency graph scheduling sample.",
        .window = {
            .width = static_cast<kin::i32>(logical_size.x),
            .height = static_cast<kin::i32>(logical_size.y),
            .logical_width = static_cast<kin::i32>(logical_size.x),
            .logical_height = static_cast<kin::i32>(logical_size.y),
            .resizable = true,
        },
        .tags = {"sample", "ecs", "graph", "systems"},
        .input_map = input,
    };
}

kin::u8 wave(kin::f32 phase, kin::f32 min_value, kin::f32 max_value) {
    const kin::f32 t = 0.5f + 0.5f * static_cast<kin::f32>(std::sin(phase));
    return static_cast<kin::u8>(std::clamp(min_value + (max_value - min_value) * t, 0.0f, 255.0f));
}

std::string_view execution_mode_name(kin::SystemExecutionMode mode) {
    switch (mode) {
    case kin::SystemExecutionMode::SerialGraph: return "SerialGraph";
    case kin::SystemExecutionMode::ParallelBatches: return "ParallelBatches";
    }
    return "Unknown";
}

class EcsGraphDemoScene final : public kin::Scene {
public:
    EcsGraphDemoScene() {
        register_components();
        register_systems();
        reset_world();
    }

    std::string_view name() const override {
        return "ECS Graph Demo";
    }

    void update(kin::SceneContext& ctx) override {
        if (ctx.window.close_requested() || ctx.input.pressed("quit")) {
            ctx.app.quit();
            return;
        }
        if (ctx.input.pressed("pause")) {
            _state.paused = !_state.paused;
        }
        if (ctx.input.pressed("reset")) {
            reset_world();
        }

        if (!_state.paused) {
            _state.time += ctx.dt;
            _world.run_frame(ctx.dt, kin::SystemExecutionMode::ParallelBatches);
            update_parallel_batch_count();
            validate_headless_self_test(ctx);
        } else {
            _world.systems().rebuild_schedule();
            update_parallel_batch_count();
        }
    }

    void render(kin::SceneContext& ctx) override {
        ctx.renderer.clear(kin::Color::rgb(16, 18, 24));
        ctx.renderer.fill_rect({0.0f, 0.0f}, {logical_size.x, 72.0f}, kin::Color::rgb(26, 30, 40));
        ctx.renderer.draw_line({0.0f, 72.0f}, {logical_size.x, 72.0f}, kin::Color::rgb(78, 86, 108));

        draw_schedule_bands(ctx.renderer);
        _world.query<Transform, Box>().each_entity([&](kin::EcsEntity, const Transform& transform, const Box& box) {
            ctx.renderer.fill_rect(transform.pos, box.size, box.color);
            ctx.renderer.draw_rect({transform.pos.x, transform.pos.y, box.size.x, box.size.y}, kin::Color::rgb(245, 248, 255));
        });
    }

    void collect_actions(kin::InputActionContext& actions) const override {
        actions.add("pause", "Pause graph");
        actions.add("reset", "Reset boxes");
        actions.add("quit", "Quit");
    }

    void write_report(kin::JsonWriter& json) const override {
        json.field("paused", _state.paused);
        json.field("spawned", _state.spawned);
        json.field("command_created", _state.command_created);
        json.field("command_destroyed", _state.command_destroyed);
        json.field("parallel_task_runs", _state.parallel_task_runs);
        json.field("parallel_batches", _state.parallel_batches);
        json.field("saw_running_parallel", _state.saw_running_parallel);
        json.field("graph_task_runs", static_cast<kin::i64>(_state.graph_task_runs));
        write_systems(json);
        write_schedule(json);
    }

    kin::EcsWorld* world() override {
        return &_world;
    }

private:
    void register_components() {
        // Parallel-eligible systems may only declare access to components the
        // registry knows, so register through it rather than raw flecs.
        auto& components = _world.components();
        components.native<Transform>("Transform");
        components.native<Velocity>("Velocity");
        components.native<Box>("Box");
        components.native<Pulse>("Pulse");
        components.native<Bounds>("Bounds");
        components.native<Lifetime>("Lifetime");
        components.native<CommandSpawned>("CommandSpawned");
        // Data-only access tokens for the task systems below.
        components.data("SpawnClock");
        components.data("SpawnCommands");
        components.data("CleanupCommands");
        components.data("ReportClock");
    }

    void register_systems() {
        _world.systems().register_native<Transform, const Velocity>({
            .id = "graph.move",
            .name = "Move",
            .phase = kin::SystemPhase::Update,
            .order = 0,
            .reads = {"Velocity"},
            .writes = {"Transform"},
            .run = [](kin::EcsEntity, Transform& transform, const Velocity& velocity, kin::SystemContext& ctx) {
                transform.pos.x += velocity.value.x * ctx.dt;
                transform.pos.y += velocity.value.y * ctx.dt;
            },
        });

        _world.systems().register_native<Box, const Pulse>({
            .id = "graph.color",
            .name = "Color",
            .phase = kin::SystemPhase::Update,
            .order = 100,
            .reads = {"Pulse"},
            .writes = {"Box"},
            .run = [this](kin::EcsEntity, Box& box, const Pulse& pulse, kin::SystemContext&) {
                const kin::f32 phase = _state.time * pulse.speed + pulse.offset;
                box.color = kin::Color::rgb(wave(phase, 120.0f, 250.0f),
                                            wave(phase + 2.0f, 110.0f, 230.0f),
                                            wave(phase + 4.0f, 150.0f, 255.0f));
            },
        });

        _world.systems().register_native<Transform, Velocity, const Box, const Bounds>({
            .id = "graph.bounce",
            .name = "Bounce",
            .phase = kin::SystemPhase::Update,
            .order = 200,
            .reads = {"Box", "Bounds"},
            .writes = {"Transform", "Velocity"},
            .after = {"graph.move", "graph.color"},
            .run = [](kin::EcsEntity, Transform& transform, Velocity& velocity, const Box& box, const Bounds& bounds, kin::SystemContext&) {
                const kin::f32 max_x = bounds.max.x - box.size.x;
                const kin::f32 max_y = bounds.max.y - box.size.y;
                if (transform.pos.x < bounds.min.x || transform.pos.x > max_x) {
                    transform.pos.x = std::clamp(transform.pos.x, bounds.min.x, max_x);
                    velocity.value.x *= -1.0f;
                }
                if (transform.pos.y < bounds.min.y || transform.pos.y > max_y) {
                    transform.pos.y = std::clamp(transform.pos.y, bounds.min.y, max_y);
                    velocity.value.y *= -1.0f;
                }
            },
        });

        _world.systems().register_native<Lifetime>({
            .id = "graph.lifetime",
            .name = "Lifetime",
            .phase = kin::SystemPhase::Update,
            .order = 300,
            .writes = {"Lifetime"},
            .execution_policy = kin::SystemExecutionPolicy::ParallelEligible,
            .run = [](kin::EcsEntity, Lifetime& lifetime, kin::SystemContext& ctx) {
                lifetime.seconds -= ctx.dt;
            },
        });

        _world.systems().register_task({
            .id = "graph.command_spawn",
            .name = "Command Spawn",
            .phase = kin::SystemPhase::PostUpdate,
            .order = 0,
            .reads = {"SpawnClock"},
            .writes = {"SpawnCommands"},
            .execution_policy = kin::SystemExecutionPolicy::ParallelEligible,
        }, [this](kin::SystemContext& ctx) {
            if (!ctx.commands) {
                return;
            }
            _state.spawn_accumulator += ctx.dt;
            if (_state.spawn_accumulator < 0.05f) {
                return;
            }
            _state.spawn_accumulator -= 0.05f;

            const kin::i32 i = _state.spawned++;
            const kin::f32 size = 26.0f + static_cast<kin::f32>((i * 11) % 22);
            const kin::f32 x = 64.0f + static_cast<kin::f32>((i * 71) % 820);
            const kin::f32 y = 126.0f + static_cast<kin::f32>((i * 41) % 320);
            const kin::f32 vx = (i % 2 == 0 ? 1.0f : -1.0f) * (95.0f + static_cast<kin::f32>((i * 13) % 90));
            const kin::f32 vy = (i % 3 == 0 ? -1.0f : 1.0f) * (75.0f + static_cast<kin::f32>((i * 17) % 70));

            const kin::EcsDeferredEntity entity = ctx.commands->create_entity("command_box_" + std::to_string(i));
            ctx.commands->set<Transform>(entity, Transform{.pos = {x, y}});
            ctx.commands->set<Velocity>(entity, Velocity{.value = {vx, vy}});
            ctx.commands->set<Box>(entity, Box{.size = {size, size}, .color = kin::Color::rgb(90, 190, 145)});
            ctx.commands->set<Pulse>(entity, Pulse{.speed = 1.4f, .offset = static_cast<kin::f32>(i) * 0.45f});
            ctx.commands->set<Bounds>(entity, Bounds{});
            ctx.commands->set<Lifetime>(entity, Lifetime{.seconds = 0.45f});
            ctx.commands->add<CommandSpawned>(entity);
            ++_state.command_created;
        });

        _world.systems().register_task({
            .id = "graph.command_cleanup",
            .name = "Command Cleanup",
            .phase = kin::SystemPhase::PostUpdate,
            .order = 0,
            .reads = {"Lifetime", "CommandSpawned"},
            .writes = {"CleanupCommands"},
            .execution_policy = kin::SystemExecutionPolicy::ParallelEligible,
        }, [this](kin::SystemContext& ctx) {
            if (!ctx.commands || !ctx.world) {
                return;
            }
            std::vector<kin::EcsId> expired;
            ctx.world->query<Lifetime, const CommandSpawned>().each_entity([&](kin::EcsEntity entity, const Lifetime& lifetime, const CommandSpawned&) {
                if (lifetime.seconds <= 0.0f) {
                    expired.push_back(entity.id());
                }
            });
            for (kin::EcsId id : expired) {
                ctx.commands->destroy(id);
                ++_state.command_destroyed;
            }
        });

        _world.systems().register_task({
            .id = "graph.report_task",
            .name = "Report Task",
            .phase = kin::SystemPhase::PostUpdate,
            .order = 0,
            .reads = {"ReportClock"},
            .execution_policy = kin::SystemExecutionPolicy::ParallelEligible,
        }, [this](kin::SystemContext& ctx) {
            ++_state.graph_task_runs;
            if (ctx.running_parallel) {
                ++_state.parallel_task_runs;
                _state.saw_running_parallel = true;
            }
        });
    }

    void reset_world() {
        std::vector<kin::EcsEntity> entities;
        _world.query<Box>().each_entity([&](kin::EcsEntity entity, Box&) {
            entities.push_back(entity);
        });
        for (kin::EcsEntity entity : entities) {
            entity.destroy();
        }

        _state.spawned = 0;
        _state.command_created = 0;
        _state.command_destroyed = 0;
        _state.graph_task_runs = 0;
        _state.parallel_task_runs = 0;
        _state.parallel_batches = 0;
        _state.time = 0.0f;
        _state.spawn_accumulator = 0.0f;
        _state.saw_running_parallel = false;
        _state.self_test_checked = false;
        for (kin::i32 i = 0; i < 10; ++i) {
            spawn_box(i);
        }
        _world.systems().rebuild_schedule();
    }

    void spawn_box(kin::i32 i) {
        const kin::f32 size = 30.0f + static_cast<kin::f32>((i * 9) % 24);
        const kin::f32 x = 48.0f + static_cast<kin::f32>((i * 83) % 820);
        const kin::f32 y = 116.0f + static_cast<kin::f32>((i * 53) % 320);
        const kin::f32 vx = (i % 2 == 0 ? 1.0f : -1.0f) * (80.0f + static_cast<kin::f32>((i * 23) % 120));
        const kin::f32 vy = (i % 3 == 0 ? -1.0f : 1.0f) * (60.0f + static_cast<kin::f32>((i * 19) % 90));

        ++_state.spawned;
        _world.entity("graph_box_" + std::to_string(i))
            .set(Transform{.pos = {x, y}})
            .set(Velocity{.value = {vx, vy}})
            .set(Box{.size = {size, size}, .color = kin::Color::rgb(220, 170, 90)})
            .set(Pulse{.speed = 1.0f + static_cast<kin::f32>(i % 4) * 0.35f, .offset = static_cast<kin::f32>(i) * 0.7f})
            .set(Bounds{});
    }

    void draw_schedule_bands(kin::Renderer2D& renderer) const {
        const kin::SystemScheduleSnapshot& schedule = _world.systems().schedule_snapshot();
        const std::array<kin::Color, 4> colors{{
            kin::Color::rgb(72, 132, 220),
            kin::Color::rgb(90, 190, 140),
            kin::Color::rgb(220, 170, 80),
            kin::Color::rgb(180, 110, 220),
        }};
        kin::f32 x = 28.0f;
        for (const kin::SystemBatch& batch : schedule.batches) {
            const kin::f32 width = 42.0f + static_cast<kin::f32>(batch.systems.size()) * 48.0f;
            const kin::f32 y = batch.phase == kin::SystemPhase::Update ? 18.0f : 44.0f;
            renderer.fill_rect({x, y}, {width, 14.0f}, colors[static_cast<std::size_t>(batch.index) % colors.size()]);
            const kin::Color outline = batch.parallel_eligible
                ? kin::Color::rgb(120, 245, 185)
                : kin::Color::rgb(235, 240, 250);
            renderer.draw_rect({x, y, width, 14.0f}, outline);
            x += width + 12.0f;
        }
    }

    void write_systems(kin::JsonWriter& json) const {
        json.key("systems").begin_array();
        for (const kin::SystemSnapshot& system : _world.systems().snapshots()) {
            json.begin_object();
            json.field("id", system.id);
            json.field("phase", kin::system_phase_name(system.phase));
            json.field("batch_index", static_cast<kin::i64>(system.batch_index));
            json.field("parallel_eligible", system.parallel_eligible);
            json.field("runs", system.stats.runs);
            json.field("matched_entities", static_cast<kin::i64>(system.stats.matched_entities));
            json.field("last_error", system.last_error);
            json.key("diagnostics").begin_array();
            for (const std::string& diagnostic : system.schedule_diagnostics) {
                json.value(diagnostic);
            }
            json.end_array();
            json.end_object();
        }
        json.end_array();
    }

    void write_schedule(kin::JsonWriter& json) const {
        const kin::SystemScheduleSnapshot& schedule = _world.systems().schedule_snapshot();
        json.key("schedule").begin_object();
        json.field("valid", schedule.valid);
        json.field("requested_execution_mode", execution_mode_name(schedule.requested_execution_mode));
        json.field("effective_execution_mode", execution_mode_name(schedule.effective_execution_mode));
        json.field("parallel_batches", static_cast<kin::i64>(_state.parallel_batches));
        json.key("diagnostics").begin_array();
        for (const std::string& diagnostic : schedule.diagnostics) {
            json.value(diagnostic);
        }
        json.end_array();
        json.key("batches").begin_array();
        for (const kin::SystemBatch& batch : schedule.batches) {
            json.begin_object();
            json.field("phase", kin::system_phase_name(batch.phase));
            json.field("index", static_cast<kin::i64>(batch.index));
            json.field("parallel_eligible", batch.parallel_eligible);
            json.key("systems").begin_array();
            for (const kin::SystemId& id : batch.systems) {
                json.value(id);
            }
            json.end_array();
            json.end_object();
        }
        json.end_array();
        json.key("edges").begin_array();
        for (const kin::SystemDependencyEdge& edge : schedule.edges) {
            json.begin_object();
            json.field("before", edge.before);
            json.field("after", edge.after);
            json.field("reason", edge.reason);
            json.end_object();
        }
        json.end_array();
        json.end_object();
    }

    void update_parallel_batch_count() {
        _state.parallel_batches = 0;
        const kin::SystemScheduleSnapshot& schedule = _world.systems().schedule_snapshot();
        for (const kin::SystemBatch& batch : schedule.batches) {
            if (batch.parallel_eligible && batch.systems.size() > 1) {
                ++_state.parallel_batches;
            }
        }
    }

    kin::i32 command_spawned_count() {
        return _world.count<CommandSpawned>();
    }

    bool native_serial_diagnostic_present() const {
        // graph.lifetime is a parallel-eligible native system that matches too
        // few entities for Flecs workers, so the registry runs it serially.
        for (const kin::SystemSnapshot& system : _world.systems().snapshots()) {
            if (system.execution_decision_reason == "native matched entity count below threshold") {
                return true;
            }
        }
        return false;
    }

    void validate_headless_self_test(kin::SceneContext& ctx) {
        if (!ctx.report || _state.self_test_checked || _state.time < 1.0f) {
            return;
        }
        _state.self_test_checked = true;
        const kin::SystemScheduleSnapshot& schedule = _world.systems().schedule_snapshot();
        if (schedule.effective_execution_mode != kin::SystemExecutionMode::ParallelBatches) {
            ctx.report->fail("ecs_graph_demo did not run a parallel task batch");
            return;
        }
        if (!_state.saw_running_parallel || _state.parallel_task_runs <= 0) {
            ctx.report->fail("ecs_graph_demo task systems did not observe parallel execution");
            return;
        }
        if (_state.command_created <= 0 || command_spawned_count() <= 0) {
            ctx.report->fail("ecs_graph_demo command-created entities are missing");
            return;
        }
        if (_state.command_destroyed <= 0) {
            ctx.report->fail("ecs_graph_demo command cleanup did not destroy expired entities");
            return;
        }
        if (!native_serial_diagnostic_present()) {
            ctx.report->fail("ecs_graph_demo native serial diagnostic is missing");
        }
    }

    kin::EcsWorld _world;
    DemoState _state;
};

std::unique_ptr<kin::Scene> make_scene() {
    return std::make_unique<EcsGraphDemoScene>();
}

} // namespace
} // namespace graph_demo

int main(int argc, char** argv) {
    kin::GameInfo game = graph_demo::make_game_info();
    kin::SceneManager scenes;

    const auto build_scenes = [](kin::SceneManager& target) {
        target.push(graph_demo::make_scene());
    };
    build_scenes(scenes);

    return kin::run_scene_app({
        .window = kin::window_config(game),
        .headless = kin::parse_headless_options(argc, argv),
        .game = &game,
        .reset_scenes = build_scenes,
    }, scenes);
}
