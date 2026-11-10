// What a step of 2D physics costs kin around Box2D (update_physics_world),
// in a world shaped like a small top-down game's (Lantern Vale's): static
// walls in runs, trees made from a prefab (their body and collider
// inherited), a few bodies moving (the player, slimes) and sensors with a
// contact queue (gems, doors). The Box2D step itself is timed apart.
// A crowd (a third number) adds that many more bodies pressing on a point
// that wanders, as a horde presses on its prey: Box2D's own work then.
// Run: ninja kin_physics2d_bench && ./bin/kin_physics2d_bench [walls] [steps] [crowd] [units_per_meter]

#include <kin/ecs/physics.hpp>
#include <kin/scene/scene_asset.hpp>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

kin::PhysicsCollider box(kin::Vec2f half, bool sensor = false) {
    return {.def = {.shape = kin::PhysicsShape::box(half), .sensor = sensor}};
}

} // namespace

int main(int argc, char** argv) {
    const int walls = argc > 1 ? std::atoi(argv[1]) : 300;
    const int steps = argc > 2 ? std::atoi(argv[2]) : 2000;
    const int crowd = argc > 3 ? std::atoi(argv[3]) : 0;
    const kin::f32 units_per_meter = argc > 4 ? static_cast<kin::f32>(std::atof(argv[4])) : 1.0f;

    kin::EcsWorld world;
    kin::register_physics_components(world);
    world.component<kin::Transform2D>("Transform2D");
    kin::PhysicsWorld physics{kin::PhysicsWorldDef{.units_per_meter = units_per_meter}};
    kin::EcsPhysicsBridge bridge{world, physics};

    for (int i = 0; i < walls; ++i) {
        world.entity()
            .set(kin::Transform2D{.pos = {static_cast<kin::f32>(i % 40) * 32.0f, static_cast<kin::f32>(i / 40) * 32.0f}})
            .set(kin::PhysicsBodyComponent{.def = {.type = kin::PhysicsBodyType::Static}})
            .set(kin::PhysicsColliderComponent{.colliders = {box({16.0f, 16.0f})}});
    }
    flecs::entity tree = world.raw().prefab("Tree")
                             .set(kin::PhysicsBodyComponent{.def = {.type = kin::PhysicsBodyType::Static}})
                             .set(kin::PhysicsColliderComponent{.colliders = {box({6.0f, 4.0f})}});
    for (int i = 0; i < walls / 5; ++i) {
        world.raw().entity().is_a(tree).set(kin::Transform2D{.pos = {static_cast<kin::f32>(i) * 21.0f, 700.0f}});
    }
    std::vector<kin::EcsEntity> movers;
    for (int i = 0; i < 12; ++i) {
        movers.push_back(world.entity()
                             .set(kin::Transform2D{.pos = {100.0f + static_cast<kin::f32>(i) * 40.0f, 400.0f}})
                             .set(kin::PhysicsBodyComponent{.def = {.type = kin::PhysicsBodyType::Dynamic, .gravity_scale = 0.0f, .fixed_rotation = true}})
                             .set(kin::PhysicsColliderComponent{.colliders = {box({6.0f, 6.0f})}})
                             .set(kin::PhysicsContactQueue{}));
    }
    for (int i = 0; i < 8; ++i) {
        world.entity()
            .set(kin::Transform2D{.pos = {150.0f + static_cast<kin::f32>(i) * 60.0f, 420.0f}})
            .set(kin::PhysicsBodyComponent{.def = {.type = kin::PhysicsBodyType::Static}})
            .set(kin::PhysicsColliderComponent{.colliders = {box({8.0f, 8.0f}, true)}})
            .set(kin::PhysicsContactQueue{});
    }

    std::vector<kin::EcsEntity> horde;
    for (int i = 0; i < crowd; ++i) {
        const kin::f32 a = static_cast<kin::f32>(i) * 2.399963f; // spread on a spiral
        const kin::f32 r = 20.0f + 5.1f * std::sqrt(static_cast<kin::f32>(i)); // packed: about 9 units apart

        horde.push_back(world.entity()
                            .set(kin::Transform2D{.pos = {640.0f + r * std::cos(a), 640.0f + r * std::sin(a)}})
                            .set(kin::PhysicsBodyComponent{.def = {.type = kin::PhysicsBodyType::Dynamic, .gravity_scale = 0.0f,
                                                                   .fixed_rotation = true}})
                            .set(kin::PhysicsColliderComponent{.colliders = {{.def = {.shape = kin::PhysicsShape::circle(4.0f)}}}}));
    }

    const kin::f32 dt = 1.0f / 60.0f;
    kin::update_physics_world(world, physics, dt); // the bodies made
    std::vector<double> step_ms;
    std::vector<double> box2d_ms;
    for (int s = 0; s < steps; ++s) {
        for (std::size_t i = 0; i < movers.size(); ++i) {
            const kin::f32 phase = static_cast<kin::f32>(s + static_cast<int>(i) * 13) * 0.02f;
            movers[i].set(kin::PhysicsVelocity{.linear = {60.0f * std::cos(phase), 40.0f * std::sin(phase)}});
        }
        const kin::Vec2f prey{640.0f + 200.0f * std::cos(static_cast<kin::f32>(s) * 0.01f), 640.0f + 150.0f * std::sin(static_cast<kin::f32>(s) * 0.013f)};
        for (kin::EcsEntity& one : horde) {
            const kin::Vec2f at = one.get<kin::Transform2D>()->pos;
            const kin::f32 dx = prey.x - at.x, dy = prey.y - at.y, d = std::sqrt(dx * dx + dy * dy) + 0.001f;
            one.set(kin::PhysicsVelocity{.linear = {dx / d * 40.0f, dy / d * 40.0f}});
        }
        const auto start = Clock::now();
        kin::update_physics_world(world, physics, dt);
        step_ms.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        const auto box2d = Clock::now();
        physics.step(dt, 8, 3); // Box2D alone, a step more (the crowd moves twice as fast)
        box2d_ms.push_back(std::chrono::duration<double, std::milli>(Clock::now() - box2d).count());
    }
    std::ranges::sort(step_ms);
    std::ranges::sort(box2d_ms);
    std::printf("bodies %d (walls %d, trees %d, movers 12, sensors 8, crowd %d), steps %d\n", walls + walls / 5 + 20 + crowd, walls,
                walls / 5, crowd, steps);
    std::printf("update_physics_world: median %.4f ms, p90 %.4f ms; Box2D's step alone: median %.4f ms\n", step_ms[step_ms.size() / 2],
                step_ms[step_ms.size() * 9 / 10], box2d_ms[box2d_ms.size() / 2]);
    // Where everything ended, bit for bit: the same however many threads step it.
    kin::u64 hash = 1469598103934665603ull;
    const auto mix = [&](kin::f32 value) {
        kin::u32 bits = 0;
        std::memcpy(&bits, &value, sizeof bits);
        hash = (hash ^ bits) * 1099511628211ull;
    };
    for (const kin::EcsEntity& one : horde) {
        mix(one.get<kin::Transform2D>()->pos.x);
        mix(one.get<kin::Transform2D>()->pos.y);
    }
    for (const kin::EcsEntity& one : movers) {
        mix(one.get<kin::Transform2D>()->pos.x);
        mix(one.get<kin::Transform2D>()->pos.y);
    }
    std::printf("positions hash %016llx\n", static_cast<unsigned long long>(hash));
    return 0;
}
