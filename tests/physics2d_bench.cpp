// What a step of 2D physics costs kin around Box2D (update_physics_world),
// in a world shaped like a small top-down game's (Lantern Vale's): static
// walls in runs, trees made from a prefab (their body and collider
// inherited), a few bodies moving (the player, slimes) and sensors with a
// contact queue (gems, doors). The Box2D step itself is timed apart.
// Run: ninja kin_physics2d_bench && ./bin/kin_physics2d_bench [walls] [steps]

#include <kin/ecs/physics.hpp>
#include <kin/scene/scene_asset.hpp>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
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

    kin::EcsWorld world;
    kin::register_physics_components(world);
    world.component<kin::Transform2D>("Transform2D");
    kin::PhysicsWorld physics;
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

    const kin::f32 dt = 1.0f / 60.0f;
    kin::update_physics_world(world, physics, dt); // the bodies made
    std::vector<double> step_ms;
    std::vector<double> box2d_ms;
    for (int s = 0; s < steps; ++s) {
        for (std::size_t i = 0; i < movers.size(); ++i) {
            const kin::f32 phase = static_cast<kin::f32>(s + static_cast<int>(i) * 13) * 0.02f;
            movers[i].set(kin::PhysicsVelocity{.linear = {60.0f * std::cos(phase), 40.0f * std::sin(phase)}});
        }
        const auto start = Clock::now();
        kin::update_physics_world(world, physics, dt);
        step_ms.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        const auto box2d = Clock::now();
        physics.step(0.0f, 8, 3); // Box2D alone, nothing advanced: its fixed cost
        box2d_ms.push_back(std::chrono::duration<double, std::milli>(Clock::now() - box2d).count());
    }
    std::ranges::sort(step_ms);
    std::ranges::sort(box2d_ms);
    std::printf("bodies %d (walls %d, trees %d, movers 12, sensors 8), steps %d\n", walls + walls / 5 + 20, walls, walls / 5,
                steps);
    std::printf("update_physics_world: median %.4f ms, p90 %.4f ms; an empty Box2D step %.4f ms\n", step_ms[step_ms.size() / 2],
                step_ms[step_ms.size() * 9 / 10], box2d_ms[box2d_ms.size() / 2]);
    return 0;
}
