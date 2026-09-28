#pragma once

#include <kin/ecs/render.hpp>
#include <kin/ecs/world.hpp>
#include <kin/physics/physics.hpp>

#include <vector>

namespace kin {

struct PhysicsCollider {
    PhysicsFixtureDef def{};
    PhysicsFixture fixture{};
};

struct PhysicsBodyComponent {
    PhysicsBody body{};
    PhysicsBodyDef def{};
    bool sync_to_physics = false;
    bool sync_from_physics = true;
};

struct PhysicsColliderComponent {
    std::vector<PhysicsCollider> colliders;
};

struct PhysicsVelocity {
    Vec2f linear{};
};

struct PhysicsContactQueue {
    std::vector<PhysicsContactEvent> events;
};

struct PhysicsOverlapSet {
    std::vector<PhysicsContact> contacts;
};

class EcsPhysicsBridge {
public:
    EcsPhysicsBridge(EcsWorld& world, PhysicsWorld& physics);

    EcsPhysicsBridge(const EcsPhysicsBridge&) = delete;
    EcsPhysicsBridge& operator=(const EcsPhysicsBridge&) = delete;
    EcsPhysicsBridge(EcsPhysicsBridge&&) noexcept = default;
    EcsPhysicsBridge& operator=(EcsPhysicsBridge&&) noexcept = default;

private:
    flecs::observer _remove_body_observer;
    flecs::observer _remove_collider_observer;
};

void register_physics_components(EcsWorld& world);

void sync_physics_bodies(EcsWorld& world, PhysicsWorld& physics);

void sync_transforms_to_physics(EcsWorld& world, PhysicsWorld& physics);

void step_physics(EcsWorld& world, PhysicsWorld& physics, f32 dt, i32 velocity_iterations = 8, i32 position_iterations = 3);

void sync_transforms_from_physics(EcsWorld& world, PhysicsWorld& physics);

void collect_physics_contacts(EcsWorld& world, PhysicsWorld& physics);

void update_physics_world(EcsWorld& world, PhysicsWorld& physics, f32 dt, i32 velocity_iterations = 8, i32 position_iterations = 3);

} // namespace kin
