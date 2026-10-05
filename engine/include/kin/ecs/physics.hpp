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

// A Box2D body for the entity. Physics works in the world, Transform2D from
// the entity's parent (flecs ChildOf): the sync converts between them.
// - sync_from_physics (the default): the simulation leads. The body stays
//   where it is in the world when its parent moves, and Transform2D is
//   rewritten to say where that is from the parent.
// - sync_to_physics: the entity leads. The body is moved to the entity's
//   place in the world before each step, so it follows its parent (hitboxes,
//   moving platforms). With both, the entity can move the body and the
//   simulation carries on from there.
// Bodies are made where the entity is in the world. They ignore scale (a
// warning, once): size the colliders instead.
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
