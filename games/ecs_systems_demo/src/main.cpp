#include <kin/core/json.hpp>
#include <kin/ecs/system.hpp>
#include <kin/platform/input.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/runtime/game_info.hpp>
#include <kin/runtime/scene_app.hpp>
#include <kin/scene/scene.hpp>
#include <kin/scene/scene_manager.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <string_view>

namespace demo {
namespace {

constexpr kin::Vec2f logical_size{960.0f, 540.0f};

struct Transform {
    kin::Vec2f pos;
};

struct Velocity {
    kin::Vec2f value;
};

struct Box {
    kin::Vec2f size{48.0f, 48.0f};
    kin::Color color = kin::Color::rgb(240, 180, 80);
};

struct Pulse {
    kin::f32 speed = 1.0f;
    kin::f32 offset = 0.0f;
};

struct DemoState {
    bool paused = false;
    kin::i32 spawned = 0;
    kin::f32 time = 0.0f;
};

kin::GameInfo make_game_info() {
    kin::InputMap input;
    input.bind("quit", kin::Key::Escape);
    input.bind("pause", kin::Key::Space);
    input.bind("reset", kin::Key::R);
    input.bind("spawn", kin::Key::Enter);

    return {
        .id = "ecs_systems_demo",
        .title = "ECS Systems Demo",
        .version = "0.1",
        .description = "Small Kin ECS system registry example.",
        .window = {
            .width = static_cast<kin::i32>(logical_size.x),
            .height = static_cast<kin::i32>(logical_size.y),
            .logical_width = static_cast<kin::i32>(logical_size.x),
            .logical_height = static_cast<kin::i32>(logical_size.y),
            .resizable = true,
        },
        .tags = {"sample", "ecs", "systems"},
        .input_map = input,
    };
}

kin::u8 pulse_channel(kin::f32 phase, kin::f32 min_value, kin::f32 max_value) {
    const kin::f32 t = 0.5f + 0.5f * static_cast<kin::f32>(std::sin(phase));
    return static_cast<kin::u8>(std::clamp(min_value + (max_value - min_value) * t, 0.0f, 255.0f));
}

class EcsSystemsDemoScene final : public kin::Scene {
public:
    EcsSystemsDemoScene() {
        register_components();
        register_systems();
        reset_world();
    }

    std::string_view name() const override {
        return "ECS Systems Demo";
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
        if (ctx.input.pressed("spawn")) {
            spawn_box();
        }

        if (!_state.paused) {
            _state.time += ctx.dt;
            _world.run_frame(ctx.dt);
        }
    }

    void render(kin::SceneContext& ctx) override {
        ctx.renderer.clear(kin::Color::rgb(18, 20, 26));

        ctx.renderer.draw_line({0.0f, 72.0f}, {logical_size.x, 72.0f}, kin::Color::rgb(54, 60, 76));
        ctx.renderer.draw_line({0.0f, logical_size.y - 40.0f}, {logical_size.x, logical_size.y - 40.0f}, kin::Color::rgb(54, 60, 76));

        _world.query<Transform, Box>().each_entity([&](kin::EcsEntity, const Transform& transform, const Box& box) {
            ctx.renderer.fill_rect(transform.pos, box.size, box.color);
            ctx.renderer.draw_rect({transform.pos.x, transform.pos.y, box.size.x, box.size.y}, kin::Color::rgb(245, 248, 255));
        });
    }

    void collect_actions(kin::InputActionContext& actions) const override {
        actions.add("pause", "Pause systems");
        actions.add("reset", "Reset boxes");
        actions.add("spawn", "Spawn box");
        actions.add("quit", "Quit");
    }

    void write_report(kin::JsonWriter& json) const override {
        json.field("paused", _state.paused);
        json.field("spawned", _state.spawned);
        json.field("box_count", static_cast<kin::u64>(_state.spawned));
        json.key("systems").begin_array();
        for (const kin::SystemSnapshot& system : _world.systems().snapshots()) {
            json.begin_object();
            json.field("id", system.id);
            json.field("phase", kin::system_phase_name(system.phase));
            json.field("rate", static_cast<kin::i64>(system.rate));
            json.field("interval_seconds", system.interval_seconds);
            json.field("runs", system.stats.runs);
            json.field("matched_entities", static_cast<kin::i64>(system.stats.matched_entities));
            json.field("last_duration_ms", system.stats.last_duration_ms);
            json.end_object();
        }
        json.end_array();
    }

    kin::EcsWorld* world() override {
        return &_world;
    }

private:
    void register_components() {
        _world.component<Transform>("Transform");
        _world.component<Velocity>("Velocity");
        _world.component<Box>("Box");
        _world.component<Pulse>("Pulse");
    }

    void register_systems() {
        _world.systems().register_native<Transform, const Velocity>({
            .id = "demo.move",
            .name = "Move Boxes",
            .phase = kin::SystemPhase::Update,
            .order = 10,
            .reads = {"Velocity"},
            .writes = {"Transform"},
            .run = [](kin::EcsEntity, Transform& transform, const Velocity& velocity, kin::SystemContext& ctx) {
                transform.pos.x += velocity.value.x * ctx.dt;
                transform.pos.y += velocity.value.y * ctx.dt;
            },
        });

        _world.systems().register_native<Transform, Velocity, const Box>({
            .id = "demo.bounce",
            .name = "Bounce At Bounds",
            .phase = kin::SystemPhase::PostUpdate,
            .order = 0,
            .reads = {"Box"},
            .writes = {"Transform", "Velocity"},
            .run = [](kin::EcsEntity, Transform& transform, Velocity& velocity, const Box& box, kin::SystemContext&) {
                const kin::f32 min_y = 80.0f;
                const kin::f32 max_x = logical_size.x - box.size.x;
                const kin::f32 max_y = logical_size.y - 48.0f - box.size.y;

                if (transform.pos.x < 0.0f || transform.pos.x > max_x) {
                    transform.pos.x = std::clamp(transform.pos.x, 0.0f, max_x);
                    velocity.value.x *= -1.0f;
                }
                if (transform.pos.y < min_y || transform.pos.y > max_y) {
                    transform.pos.y = std::clamp(transform.pos.y, min_y, max_y);
                    velocity.value.y *= -1.0f;
                }
            },
        });

        _world.systems().register_native<Box, const Pulse>({
            .id = "demo.color_pulse",
            .name = "Color Pulse",
            .phase = kin::SystemPhase::Animation,
            .order = 0,
            .reads = {"Pulse"},
            .writes = {"Box"},
            .run = [this](kin::EcsEntity, Box& box, const Pulse& pulse, kin::SystemContext&) {
                const kin::f32 phase = _state.time * pulse.speed + pulse.offset;
                box.color = kin::Color::rgb(pulse_channel(phase, 90.0f, 245.0f),
                                            pulse_channel(phase + 2.1f, 110.0f, 230.0f),
                                            pulse_channel(phase + 4.2f, 130.0f, 255.0f));
            },
        });
    }

    void reset_world() {
        std::vector<kin::EcsEntity> boxes;
        _world.query<Box>().each_entity([&](kin::EcsEntity entity, Box&) {
            boxes.push_back(entity);
        });
        for (kin::EcsEntity entity : boxes) {
            entity.destroy();
        }

        _state.spawned = 0;
        _state.time = 0.0f;
        for (kin::i32 i = 0; i < 8; ++i) {
            spawn_box();
        }
    }

    void spawn_box() {
        const kin::i32 i = _state.spawned++;
        const kin::f32 x = 64.0f + static_cast<kin::f32>((i * 91) % 760);
        const kin::f32 y = 104.0f + static_cast<kin::f32>((i * 47) % 330);
        const kin::f32 sx = (i % 2 == 0 ? 1.0f : -1.0f) * (90.0f + static_cast<kin::f32>((i * 17) % 95));
        const kin::f32 sy = (i % 3 == 0 ? -1.0f : 1.0f) * (70.0f + static_cast<kin::f32>((i * 29) % 80));
        const kin::f32 size = 32.0f + static_cast<kin::f32>((i * 7) % 28);

        _world.entity("box_" + std::to_string(i))
            .set(Transform{.pos = {x, y}})
            .set(Velocity{.value = {sx, sy}})
            .set(Box{.size = {size, size}, .color = kin::Color::rgb(230, 180, 90)})
            .set(Pulse{.speed = 1.0f + static_cast<kin::f32>(i % 5) * 0.25f, .offset = static_cast<kin::f32>(i) * 0.65f});
    }

    kin::EcsWorld _world;
    DemoState _state;
};

std::unique_ptr<kin::Scene> make_scene() {
    return std::make_unique<EcsSystemsDemoScene>();
}

} // namespace
} // namespace demo

int main(int argc, char** argv) {
    kin::GameInfo game = demo::make_game_info();
    kin::SceneManager scenes;

    const auto build_scenes = [] (kin::SceneManager& target) {
        target.push(demo::make_scene());
    };
    build_scenes(scenes);

    return kin::run_scene_app({
        .window = kin::window_config(game),
        .headless = kin::parse_headless_options(argc, argv),
        .game = &game,
        .reset_scenes = build_scenes,
    }, scenes);
}
