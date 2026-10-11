#pragma once

// 3D physics for entities (experimental, docs/physics_3d.md): a rigid body and a collider on an entity, and a system
// that makes the World3D's bodies for them, moves them from and to Transform3D, steps the world at the fixed step and
// hands the contact events to game code.
//
//     kin::World3D physics;
//     kin::WorldPhysicsState3D bodies{world, physics};
//     world.entity().set(kin::Transform3D{.position = {0, 5, 0}})
//                   .set(kin::Collider3D{.kind = kin::Collider3D::Kind::Box, .half_extents = {0.5f, 0.5f, 0.5f}})
//                   .set(kin::RigidBody3D{.density = 600.0f});
//     ...each frame:  bodies.advance(frame_seconds);        // or step(fixed_dt) from the engine's fixed step
//                     for (const kin::ContactEvent3D& e : bodies.events()) { ... bodies.entity_of(e.user_a) ... }
//
// The rules (the same as the 2D bridge's, in 3D):
//  * An entity with Transform3D and Collider3D has a body; RigidBody3D says how it moves (without it: a static body).
//    Bodies are made, in entity id order, at the next sync (step/advance/sync_bodies), where the entity is in the WORLD
//    (its Transform3D composed with its parents'), with the entity's world scale applied to the collider's sizes. A body
//    is destroyed when its entity, its Collider3D or its Transform3D goes. Changing the components of a live body
//    (a collider's size, a body's density) does nothing until `rebuild(entity)`; changing RigidBody3D::motion
//    rebuilds by itself.
//  * Bodies are world level. A dynamic body of an entity with a parent is NOT carried by the parent: it is where the
//    simulation puts it, and the entity's Transform3D is rewritten each step as its place in the world seen from the
//    parent as it is now (the parent moving leaves the body behind). A kinematic body follows its entity (with the
//    parent's motion) and a static one is moved when its entity is.
//  * Dynamic bodies write Transform3D (position and rotation; the scale is left) after the step. If game code changes
//    a dynamic entity's Transform3D between steps, the body is moved there (velocities kept).
//  * Determinism: bodies are made and destroyed in ascending entity id order, so body numbers, the step and the events
//    are the same in two runs that do the same things.

#if !defined(KIN_EXPERIMENTAL_3D)
#error "kin's 3D physics is experimental: configure kin with -DKIN_EXPERIMENTAL_3D=ON"
#endif

#include <kin/ecs/render3d.hpp>
#include <kin/physics3d/debug_draw3d.hpp>
#include <kin/physics3d/world3d.hpp>
#include <kin/render3d/ik.hpp>
#include <kin/render3d/mesh.hpp>

#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace kin {

class Renderer3D;
class Scene3D;

// What an entity collides as.
struct Collider3D {
    enum class Kind : u8 { Box, Sphere, Capsule, Cylinder, ConvexHull, TriangleMesh, Compound };
    Kind kind = Kind::Box;
    Vec3f half_extents{0.5f, 0.5f, 0.5f}; // Box
    f32 radius = 0.5f;                    // Sphere, Capsule, Cylinder
    f32 half_height = 0.5f;               // Capsule (its straight part's half), Cylinder: along the shape's Y
    // ConvexHull (the points are its vertices) and TriangleMesh: triangles on the CPU. The GPU's Mesh keeps none
    // (collider_from_mesh_data builds one from a MeshData). TriangleMesh is for static and kinematic bodies.
    std::shared_ptr<const MeshData> mesh;
    // Compound: each child places itself with its own offset and rotation.
    std::vector<Collider3D> children;
    // Where the shape sits in the entity's space (the entity's scale applies to it).
    Vec3f offset{};
    Quatf rotation{};
    // Times the entity's world scale (a collider that is not the size of its mesh).
    Vec3f scale{1.0f, 1.0f, 1.0f};
    // A shape made already (a level's mesh, shared by many entities): used as it is, in place of everything above bar
    // offset and rotation. The scale is not applied to it.
    Shape3D shape;
};

// How an entity moves.
struct RigidBody3D {
    Body3DMotion motion = Body3DMotion::Dynamic;
    f32 mass = 0.0f; // kg when above 0, else density times the collider's volume
    f32 density = 1000.0f;
    f32 friction = 0.2f;
    f32 restitution = 0.0f;
    f32 linear_damping = 0.0f;
    f32 angular_damping = 0.0f;
    f32 gravity_factor = 1.0f;
    u8 layer = layers3d::Auto;
    bool sensor = false;
    bool allow_sleeping = true;
    bool continuous = false;
    // At creation.
    Vec3f linear_velocity{};
    Vec3f angular_velocity{};
};

// The body made for an entity: set by the system, read by game code (apply forces through World3D with it).
struct PhysicsBody3D {
    Body3D body{};
};

// The events of the last step (or advance), on the ECS world as a singleton too.
struct PhysicsContacts3D {
    std::vector<ContactEvent3D> events;
};

struct WorldPhysicsState3DConfig {
    // advance() steps in whole multiples of this, the way the engine's fixed step does.
    f32 fixed_dt = 1.0f / 60.0f;
    // advance() takes at most this many steps for one frame (a long frame must not spiral).
    i32 max_steps = 8;
    // Debug lines for the 3D view (collect_debug); off draws nothing and costs nothing.
    bool debug_draw = false;
    DebugDraw3DOptions debug{};
    // Draw the contacts of the step as well (persist events must be on in the World3DConfig to see resting contacts).
    bool debug_contacts = true;
};

class WorldPhysicsState3D {
public:
    WorldPhysicsState3D(flecs::world& world, World3D& physics, WorldPhysicsState3DConfig config = {});
    WorldPhysicsState3D(EcsWorld& world, World3D& physics, WorldPhysicsState3DConfig config = {})
        : WorldPhysicsState3D(world.raw(), physics, config) {}

    WorldPhysicsState3DConfig& config() { return _config; }
    const WorldPhysicsState3DConfig& config() const { return _config; }

    // Makes and destroys bodies for the entities that gained or lost their components (ascending entity id).
    // step() does it itself first; call it to have the bodies before the first step (to query them).
    void sync_bodies();
    // One fixed step of `dt`: sync_bodies, entities to bodies, World3D::step, bodies to entities, events.
    void step(f32 dt);
    // Steps for `frame_seconds` of time at the configured fixed step (carrying the remainder); returns the steps
    // taken. alpha() is then how far the next step is, 0 to 1, for interpolate().
    i32 advance(f32 frame_seconds);
    f32 alpha() const { return _config.fixed_dt > 0.0f ? _accumulator / _config.fixed_dt : 0.0f; }
    // Writes the Transform3D of every awake dynamic body as between the last two steps' places, `alpha` of the way
    // (0: the one before, 1: the last: what step() leaves). For drawing between fixed steps; opt-in, call it
    // after advance() with alpha().
    void interpolate(f32 alpha);

    // Destroys the entity's body and makes it again at the next sync from the components as they are now.
    void rebuild(flecs::entity entity);

    // The contact events of the last step() (all the steps of one advance()), in the world's order, with the
    // entity ids in user_a and user_b.
    std::span<const ContactEvent3D> events() const { return _events; }
    // The entity a user id of an event is (invalid when it is gone).
    flecs::entity entity_of(u64 user_id) const;
    // The body of an entity (invalid when it has none yet).
    Body3D body_of(flecs::entity entity) const;

    // Draws the world's debug lines (the shapes, constraints, contacts) into `scene` as a wireframe, if
    // config().debug_draw. Add your own with debug() first (rays, sweeps) and they are drawn too.
    DebugDraw3D& debug() { return _debug; }
    void collect_debug(Renderer3D& renderer, Scene3D& scene);

    World3D& physics() { return _physics; }
    const World3D& physics() const { return _physics; }
    // Entities that have a body now, in ascending id order.
    std::size_t body_count() const { return _bound.size(); }

private:
    struct Bound {
        flecs::entity_t entity = 0;
        Body3D body{};
        Body3DMotion motion = Body3DMotion::Static;
        // The entity's Transform3D as this system last wrote it (dynamic) or the world pose last sent (kinematic, static).
        Vec3f last_position{};
        Quatf last_rotation{};
        // The body's pose after the last step and before it (interpolation), in the world.
        Vec3f position{}, previous_position{};
        Quatf rotation{}, previous_rotation{};
        Vec3f scale{1.0f, 1.0f, 1.0f}; // the world scale at creation
        bool rebuild = false;
    };

    void create_body(flecs::entity entity, Bound& out);
    void write_back(Bound& b, Vec3f position, Quatf rotation);

    flecs::world _ecs;
    World3D& _physics;
    WorldPhysicsState3DConfig _config;
    flecs::query<const Transform3D, const Collider3D, const RigidBody3D*> _query;
    std::vector<Bound> _bound; // ascending entity id
    std::vector<ContactEvent3D> _events;
    f32 _accumulator = 0.0f;
    DebugDraw3D _debug;
    Mesh _debug_mesh; // the lines of the last collect_debug, kept so the GPU is done with them before they go
};

// A ground query for foot placement (kin/render3d/ik.hpp) over a World3D: the first surface along the segment, in the
// world's space, by raycast. `ignore` is the character's own body. The world must outlive the query.
GroundQuery ground_query_from_world(const World3D& world, Layer3DMask mask = all_layers3d, Body3D ignore = {});

// Colliders from what a mesh entity has. The GPU mesh keeps no triangles, so from the entity alone only its bounds are
// known: a box about them (offset to their centre). With the mesh's MeshData, a hull or the triangles.
std::optional<Collider3D> collider_from_mesh(flecs::entity entity);
Collider3D collider_from_bounds(const Bounds3& bounds);
Collider3D collider_hull_from_mesh_data(std::shared_ptr<const MeshData> mesh);
Collider3D collider_triangles_from_mesh_data(std::shared_ptr<const MeshData> mesh);

// Debug lines drawn as a wireframe (an unlit, depth tested line mesh, one draw): makes a mesh of them with
// `renderer`, which the caller keeps until the frame is drawn (WorldPhysicsState3D keeps its own). An empty list draws
// nothing and returns an invalid mesh. Each line is a sliver triangle of the wireframe material, its ends moved a hair toward
// the eye (same pixel, nearer) so it wins against the face it edges.
Mesh draw_debug_lines(Renderer3D& renderer, Scene3D& scene, std::span<const DebugLine3D> lines);

} // namespace kin
