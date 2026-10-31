#pragma once

#include <kin/ecs/component.hpp>
#include <kin/ecs/render.hpp>
#include <kin/ecs/world.hpp>
#include <kin/physics/physics.hpp>

#include <memory>
#include <string>

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

// Shapes to collide with. On an entity with a body, they are its body's.
// On one without, they are its nearest ancestor's body's (a body of several
// shapes, each an entity of its own): placed, turned and scaled as the entity
// is from the body's entity, and made again when that changes (a hitbox an
// animation moves). With no body above them, they are nothing.
struct PhysicsColliderComponent {
    std::vector<PhysicsCollider> colliders;
    // The ancestor's body its colliders are on, and where this entity was
    // from that body's entity when they were made (none: its own body's).
    PhysicsBody body{};
    WorldTransform placed{};
};

// The entity's body joined to another's (`other`; none: to where it is in the
// world). `anchor` is a point of this entity (as its children are placed);
// `other_anchor`, a distance joint's point on the other one; `axis`, a
// slider's or a wheel's, this entity's. Angles in degrees, as Transform2D's:
// a hinge's limits, a hinge's or a wheel's motor speed (per second); the
// rest in units. Made once both bodies are, where
// they are then; made again when either body is. A joint with a
// break_force breaks when it pulls harder (broken: then it stays so).
struct PhysicsJointComponent {
    PhysicsJointType type = PhysicsJointType::Hinge;
    ComponentEntityRef other{};
    Vec2f anchor{};
    Vec2f other_anchor{};
    Vec2f axis{1.0f, 0.0f};
    bool collide_connected = false;
    bool limit = false;
    f32 lower = 0.0f;
    f32 upper = 0.0f;
    bool motor = false;
    f32 motor_speed = 0.0f;
    f32 max_motor_force = 0.0f;
    f32 length = 0.0f;
    f32 frequency = 0.0f;
    f32 damping_ratio = 0.7f;
    f32 break_force = 0.0f; // 0: it never breaks
    bool broken = false;
    PhysicsJoint joint{};
    // What the joint was made from (it is made again when that changes), and
    // its limits, motor and spring as last given it.
    struct Made {
        PhysicsJointType type = PhysicsJointType::Hinge;
        EcsId other = 0;
        Vec2f anchor{};
        Vec2f other_anchor{};
        Vec2f axis{};
        bool collide_connected = false;
        PhysicsJointDef def{};
    } made{};
};

// A tilemap entity's solid cells, as a static body where it is: each run of
// touching solid cells one loop around it (TileMap::collision_shapes), a cell
// with a collision polygon its own shape. Made again when the map does
// (TilemapComponent::tilemap, read again). `layer`: only that layer's cells
// (empty: every layer that takes part in collision).
struct TilemapCollider {
    std::string layer;
    f32 friction = 0.6f;
    f32 restitution = 0.0f;
    bool sensor = false;
    u16 category_bits = 0x0001;
    u16 mask_bits = 0xFFFF;
    PhysicsBody body{};
    std::shared_ptr<const void> built_from; // the map its body was made from
};

// Put on an entity with a body: at the next step its body goes to where the
// entity is (it keeps its velocity), once; then the tag goes. A body the
// simulation leads can be moved so (SceneFlow::arrive does, for travellers).
struct PhysicsTeleport {};

struct PhysicsVelocity {
    Vec2f linear{};
};

struct PhysicsContactQueue {
    std::vector<PhysicsContactEvent> events;
};

struct PhysicsOverlapSet {
    std::vector<PhysicsContact> contacts;
};

// Destroys an entity's body and fixtures when it, or its component, goes.
// Made after both worlds, gone before either: its observers go with it (so
// none is left to reach a physics world that has gone).
class EcsPhysicsBridge {
public:
    EcsPhysicsBridge(EcsWorld& world, PhysicsWorld& physics);
    ~EcsPhysicsBridge();

    EcsPhysicsBridge(const EcsPhysicsBridge&) = delete;
    EcsPhysicsBridge& operator=(const EcsPhysicsBridge&) = delete;
    EcsPhysicsBridge(EcsPhysicsBridge&& other) noexcept;
    EcsPhysicsBridge& operator=(EcsPhysicsBridge&& other) noexcept;

private:
    void release();

    flecs::observer _remove_body_observer;
    flecs::observer _remove_collider_observer;
    flecs::observer _remove_joint_observer;
    flecs::observer _remove_tilemap_observer;
};

void register_physics_components(EcsWorld& world);

void sync_physics_bodies(EcsWorld& world, PhysicsWorld& physics);

void sync_transforms_to_physics(EcsWorld& world, PhysicsWorld& physics);

void step_physics(EcsWorld& world, PhysicsWorld& physics, f32 dt, i32 velocity_iterations = 8, i32 position_iterations = 3);

void sync_transforms_from_physics(EcsWorld& world, PhysicsWorld& physics);

void collect_physics_contacts(EcsWorld& world, PhysicsWorld& physics);

void update_physics_world(EcsWorld& world, PhysicsWorld& physics, f32 dt, i32 velocity_iterations = 8, i32 position_iterations = 3);

} // namespace kin
