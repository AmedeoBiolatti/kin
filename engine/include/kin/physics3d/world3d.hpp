#pragma once

// A rigid body world for kin's experimental 3D work: Jolt Physics behind the
// names of kin (no Jolt header is included here). The Box2D module,
// kin/physics, stays as it is. Experimental: the API may change in any release
// (docs/3d_plan.md, docs/physics_3d.md).
//
// Units are metres, kilograms and seconds, right-handed with +Y up (as math3d);
// gravity is a vector, not -Y, so a planet can come later.
//
// Determinism. A step is a pure function of the world's state, the calls made
// since the last step and `dt`: no clock, no uninitialised memory, no random
// numbers, and the number of worker threads makes no difference to the bits
// (tested, 1 worker against 4 against none). The library is built with Jolt's
// cross-platform determinism on, so another machine gives the same bits too.
// What the caller must keep the same between two runs: the order of the calls
// that change the world (bodies are numbered in creation order, with freed
// numbers reused last in, first out), the descriptors and `dt` (a fixed step).
// Not thread safe: use one world from one thread at a time (the step itself
// runs on the job system; do not call into a world from other threads meanwhile).
//
// Queries (raycast, shape_cast, overlap, ...) read the world as the last step left it and
// return values, never calling back; each result list has a fixed order (documented at
// the call), so the same world gives the same answers in every run and on every worker count.
// Contact events are collected by the step and handed over afterwards in a fixed order
// (events(), drain_events()): nothing of the game runs on Jolt's threads.

#include <kin/core/jobs.hpp>
#include <kin/core/math3d.hpp>
#include <kin/core/types.hpp>
#include <kin/physics3d/character3d.hpp>
#include <kin/physics3d/handles3d.hpp>
#include <kin/physics3d/shape3d.hpp>

#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace kin {

// A constraint between two bodies (or a body and the world): an index and a generation,
// as Body3D is.
struct Constraint3D {
    u32 index = UINT32_MAX;
    u32 generation = 0;

    bool valid() const { return index != UINT32_MAX; }
    explicit operator bool() const { return valid(); }
    friend constexpr bool operator==(Constraint3D, Constraint3D) = default;
};

// What a query tests. A body is considered when its layer is in `mask` and it is not `ignore`.
struct QueryFilter3D {
    Layer3DMask mask = all_layers3d;
    // One body the query never reports (the caster's own, say). An invalid handle: none.
    Body3D ignore{};
    // Sensor bodies (trigger volumes) are left out of queries unless this is set: a ray that
    // is for walls and floors should not stop at a trigger.
    bool sensors = false;
};

// Where a ray met a body (Jolt reports the first hit on each body's surface; a ray that
// starts inside a convex solid hits it at fraction 0, at the start, and a triangle mesh
// is hit from either side).
struct RayHit3D {
    Body3D body{};
    u64 user_id = 0;
    Vec3f point{};
    // The surface's normal there, unit, out of the body (a triangle mesh's is its front
    // side's, also when the ray came from behind it).
    Vec3f normal{};
    f32 fraction = 0.0f; // 0 at `from`, 1 at `to`
    f32 distance = 0.0f; // fraction times the segment's length
    // For a compound: the index of the child that was hit, in the order the children were
    // given; 0 for any other shape.
    u32 sub_shape = 0;
};

// A shape swept along a displacement: the first thing it meets.
struct ShapeHit3D {
    Body3D body{};
    u64 user_id = 0;
    // 0 to 1 along the displacement (time of impact as a share of it).
    f32 fraction = 0.0f;
    // Where the shape's origin is at that time.
    Vec3f position{};
    // On the surface of the body that was hit, in the world.
    Vec3f point{};
    // The hit surface's normal, unit, pointing toward the swept shape.
    Vec3f normal{};
    // Above zero when the shape already overlapped the body at the start (fraction 0).
    f32 penetration = 0.0f;
    u32 sub_shape = 0;
};

// A body a shape overlaps.
struct OverlapHit3D {
    Body3D body{};
    u64 user_id = 0;
    // The deepest contact found with that body: how far (m) the shape would have to move
    // along `normal` to be clear, and the point on the body where it is deepest.
    f32 penetration = 0.0f;
    Vec3f point{};
    Vec3f normal{};
};

struct ClosestPoint3D {
    Body3D body{};
    u64 user_id = 0;
    Vec3f point{};   // on the body's surface (or the query point itself when it is inside)
    f32 distance = 0.0f;
};

// What a contact event is.
enum class ContactKind3D : u8 {
    Begin,   // the pair touches now and did not at the last step
    Persist, // still touches (only reported when persist events are on)
    End,     // no longer touches (also when a body of it is destroyed)
};

// One body pair's contact, as the step saw it. `a` is the body with the lower body number
// (slot index), so a pair always comes the same way round; `normal` points from a to b.
struct ContactEvent3D {
    Body3D a{};
    Body3D b{};
    u64 user_a = 0;
    u64 user_b = 0;
    ContactKind3D kind = ContactKind3D::Begin;
    // Either body is a sensor: nothing was pushed, this only says that they overlap.
    bool sensor = false;
    // Begin and Persist: the mean of the contact points (world), the normal from a to b, how
    // fast they were closing along it before the solver ran (m/s, negative when parting),
    // and how many contact points the manifold had. Zero on End.
    //
    // A pair that falls asleep together is still touching: Jolt drops such contacts but no End is
    // reported for them, and no Begin when they are woken and still touch. End comes when a
    // woken body leaves the other, or a body is destroyed. (A static sensor does not see sleeping
    // bodies, so the same rule applies to what sleeps inside one.)
    Vec3f point{};
    Vec3f normal{};
    f32 closing_speed = 0.0f;
    u32 point_count = 0;
};

enum class Constraint3DKind : u8 {
    Fixed,    // the two bodies keep their relative placing
    Distance, // two points keep a distance within [min, max], optionally springy
    Point,    // two points are one (a ball joint)
    Hinge,    // one axis of rotation, optional limits, friction and motor
    Cone,     // a ball joint with a swing cone and twist limits (a shoulder, a neck)
    Slider,   // one axis of translation, optional limits, friction and motor
};

enum class Motor3DMode : u8 { Off, Velocity, Position };

// A constraint to make, in world space as the bodies are NOW: the anchors and axes are
// converted to each body's own space, so the constraint holds the bodies as they stand.
struct Constraint3DDesc {
    Constraint3DKind kind = Constraint3DKind::Fixed;
    // An invalid `a` is the world (a fixed point); `b` must be a body.
    Body3D a{};
    Body3D b{};
    // The anchor in the world. Distance: the point on a; `point_b` is the point on b.
    Vec3f point{};
    Vec3f point_b{};
    // Hinge: the axis; Cone: the twist axis; Slider: the axis of sliding (world, any length).
    Vec3f axis{0.0f, 1.0f, 0.0f};

    // Distance: the allowed range. A negative bound is the distance at creation (min = max =
    // that: a rod). A frequency above 0 makes the limits springs (Hz, damping ratio).
    f32 min_distance = -1.0f;
    f32 max_distance = -1.0f;
    f32 spring_frequency = 0.0f;
    f32 spring_damping = 0.0f;

    // Hinge: limits in degrees from the pose at creation, min in [-180, 0], max in [0, 180].
    // Cone: swing is the half angle of the cone about `axis`, twist the range about it.
    // Slider: min and max are metres along `axis` from the creation pose (min <= 0 <= max).
    bool limits = false;
    f32 min_angle = -180.0f;
    f32 max_angle = 180.0f;
    f32 swing_angle = 45.0f;
    f32 min_position = -1e30f;
    f32 max_position = 1e30f;

    // Hinge, Cone, Slider: friction (N m or N) when no motor drives it.
    f32 max_friction = 0.0f;

    // Breaks (is disabled, and reported by drain_constraint_events) when the force (or torque)
    // it applied in a step exceeded this; 0 for never. Force is the constraint impulse of the
    // last collision step over that step's length: an estimate, good to a few percent.
    f32 break_force = 0.0f;
    f32 break_torque = 0.0f;

    // Whether the two bodies it joins still collide with each other. Off by default (a door and its post, the links
    // of a chain, a limb on its trunk: they would fight the joint), which also saves their contact work.
    // Needs contact events on in the World3DConfig (the filter runs in the contact callback).
    bool collide_connected = false;

    u64 user_id = 0;
};

// What a constraint is now.
struct Constraint3DInfo {
    Constraint3DKind kind = Constraint3DKind::Fixed;
    Body3D a{};
    Body3D b{};
    // The anchors, in the world as the bodies are now.
    Vec3f point_a{};
    Vec3f point_b{};
    Vec3f axis{};     // hinge, cone, slider: the axis in the world
    bool broken = false;
    bool enabled = true;
    u64 user_id = 0;
    // Hinge: the angle (degrees) from the pose at creation. Slider: metres. Distance: the
    // current distance.
    f32 value = 0.0f;
};

// A constraint that broke in a step.
struct ConstraintEvent3D {
    Constraint3D constraint{};
    Body3D a{};
    Body3D b{};
    u64 user_id = 0;
    f32 force = 0.0f;
    f32 torque = 0.0f;
};

struct Body3DDesc {
    Shape3D shape;
    Body3DMotion motion = Body3DMotion::Dynamic;
    Vec3f position{};
    Quatf rotation{};
    Vec3f linear_velocity{};
    Vec3f angular_velocity{};
    // Dynamic bodies: mass in kg when above zero, else density (kg / m^3, water
    // is 1000) times the shape's volume. The inertia follows from the shape
    // (scaled to the mass).
    f32 mass = 0.0f;
    f32 density = 1000.0f;
    f32 friction = 0.2f;    // 0 to 1; a contact uses sqrt(a * b)
    f32 restitution = 0.0f; // 0 to 1; a contact uses the larger of the two
    f32 linear_damping = 0.0f;  // a fraction of the speed lost per second, as Box2D's
    f32 angular_damping = 0.0f;
    f32 gravity_factor = 1.0f;
    u8 layer = layers3d::Auto;
    // Reports contacts but does not push (it is for trigger volumes; reports
    // arrive with P2b).
    bool sensor = false;
    // Dynamic bodies at rest fall asleep and cost nothing until touched.
    bool allow_sleeping = true;
    // Swept against static geometry, so that a fast small body does not tunnel.
    bool continuous = false;
    // Starts awake (dynamic and kinematic bodies).
    bool start_active = true;
    // Yours: not read by the engine; hashed into state_hash().
    u64 user_id = 0;
};

struct World3DConfig {
    Vec3f gravity{0.0f, -9.81f, 0.0f};
    // Collision steps per step(): each is dt / collision_steps long, with its own
    // collision detection and solve. 1 is right at 60 Hz and below; raise it
    // for fast thin things instead of making dt smaller.
    i32 collision_steps = 1;
    // Capacities (they are reserved at creation and not grown).
    u32 max_bodies = 10240;
    u32 max_body_pairs = 65536;
    u32 max_contact_constraints = 20480;
    // The step's scratch memory; past it the step falls back to the heap.
    u32 temp_memory_bytes = 16u * 1024u * 1024u;
    // Where steps run: nullptr is default_job_system(). Jolt cuts a step into
    // jobs and the calling thread works on them too; keep the pool alive as long
    // as the world. A JobSystem's `max_background` limits how many workers help.
    JobSystem* jobs = nullptr;
    // Runs the whole step on the calling thread (no jobs; for tests and tools).
    bool single_threaded = false;
    // Contact events (begin and end of each body pair's touching, sensor overlaps). Off costs
    // nothing. Persist events (one a step for every touching pair) are for tools and tests:
    // they cost a record a pair a step.
    bool contact_events = true;
    bool persist_events = false;
    // The events kept between drains (past it the oldest are dropped and counted).
    u32 max_events = 1u << 16;
};

class World3D {
public:
    explicit World3D(World3DConfig config = {});
    ~World3D();

    World3D(const World3D&) = delete;
    World3D& operator=(const World3D&) = delete;

    // True when Jolt was built for cross-platform determinism (kin's build always
    // does; a build that changed that says so here).
    static bool cross_platform_deterministic();

    const World3DConfig& config() const;

    // Gravity of every body (scaled by its gravity_factor).
    void set_gravity(Vec3f gravity);
    Vec3f gravity() const;

    // Advances by `dt` seconds in collision_steps collision steps. Call with a
    // fixed dt (the engine's fixed step). Forces and impulses added since the
    // last step are applied and forces then cleared.
    void step(f32 dt);

    // Bodies. A body from an invalid shape, a dynamic body of a mesh or height
    // field, a Static or Kinematic... in layer 0 (see layers3d) or a non-finite
    // number is refused: an invalid handle comes back.
    Body3D create_body(const Body3DDesc& desc);
    // Does nothing for a handle that is not (or no longer) a body.
    void destroy_body(Body3D body);
    bool contains(Body3D body) const;
    i32 body_count() const;
    // Bodies awake now.
    i32 active_body_count() const;

    // Moves a body at once (a teleport: nothing is swept, contacts are found at
    // the next step). Velocities are left alone.
    void set_position(Body3D body, Vec3f position);
    void set_rotation(Body3D body, Quatf rotation);
    void set_linear_velocity(Body3D body, Vec3f velocity);
    void set_angular_velocity(Body3D body, Vec3f velocity);
    // A kinematic body to `position` and `rotation` by the end of the next step
    // of `dt` (it gets the velocities that do it, so what it touches is pushed).
    void move_kinematic(Body3D body, Vec3f position, Quatf rotation, f32 dt);

    // Applied in the next step, then cleared (a force must be added every step
    // that it should act; an impulse is a change of momentum, once). The `_at`
    // forms take a world point. All wake a sleeping body.
    void add_force(Body3D body, Vec3f force);
    void add_force_at(Body3D body, Vec3f force, Vec3f world_point);
    void add_torque(Body3D body, Vec3f torque);
    void add_impulse(Body3D body, Vec3f impulse);
    void add_impulse_at(Body3D body, Vec3f impulse, Vec3f world_point);
    void add_angular_impulse(Body3D body, Vec3f impulse);

    // Of the body's origin (not of its centre of mass; see centre_of_mass_position).
    Vec3f position(Body3D body) const;
    Quatf rotation(Body3D body) const;
    Vec3f centre_of_mass_position(Body3D body) const;
    Vec3f linear_velocity(Body3D body) const;
    Vec3f angular_velocity(Body3D body) const;
    Body3DMotion motion(Body3D body) const;
    u8 layer(Body3D body) const;
    u64 user_id(Body3D body) const;
    void set_user_id(Body3D body, u64 user_id);
    f32 mass(Body3D body) const; // 0 for a static or kinematic body
    Shape3D shape(Body3D body) const;

    // --- helpers for bodies that something else drives ---
    // A body to a place at once with no velocity and awake: a respawn, a spawn point. (set_position and
    // set_rotation do the same but keep the velocity.)
    void teleport(Body3D body, Vec3f position, Quatf rotation);
    // Makes a body reach `position` and `rotation` by the end of the next step of `dt`, by what suits its
    // motion: a kinematic body as move_kinematic does, a dynamic one by setting the velocities that get there
    // (it still collides on the way and is stopped by what it meets), a static one by a teleport.
    void follow_transform(Body3D body, Vec3f position, Quatf rotation, f32 dt);
    // A body's layer (the rules of create_body: only static bodies may be in layer 0). Contacts it has keep
    // going until the pair is tested again.
    bool set_layer(Body3D body, u8 layer);
    bool is_sensor(Body3D body) const;
    // A sensor is not pushed by or pushing anything but reports overlaps. Bodies already inside get a Begin
    // event at the next step when they start to count as overlapping.
    void set_sensor(Body3D body, bool sensor);
    // Swaps a body's shape (a crouch, a different scale: build a shape of the new size). A dynamic body keeps
    // its mass and gets the new shape's inertia; a mesh or height field is refused for a dynamic body. The
    // body is woken. Returns false when refused.
    bool set_shape(Body3D body, const Shape3D& shape);
    // The live bodies, by body number.
    std::vector<Body3D> bodies() const;

    // --- queries ---
    // The first body along the segment from `from` to `to`.
    std::optional<RayHit3D> raycast(Vec3f from, Vec3f to, const QueryFilter3D& filter = {}) const;
    // Every body the segment crosses (a convex body once, a mesh once for the nearest of its triangles on each
    // sub shape), nearest first, ties by body number then sub shape.
    std::vector<RayHit3D> raycast_all(Vec3f from, Vec3f to, const QueryFilter3D& filter = {}) const;
    // `shape` placed at `position` and `rotation` (its origin, as a body's) swept by `displacement`: the first
    // thing it meets, with the time of impact. Not for meshes or height fields. A shape that starts overlapping
    // reports fraction 0 and a penetration. Ties go to the lower body number.
    std::optional<ShapeHit3D> shape_cast(const Shape3D& shape, Vec3f position, Quatf rotation, Vec3f displacement,
                                         const QueryFilter3D& filter = {}) const;
    // The bodies `shape` at `position` and `rotation` overlaps, sorted by body number (one entry for each).
    std::vector<OverlapHit3D> overlap(const Shape3D& shape, Vec3f position, Quatf rotation,
                                      const QueryFilter3D& filter = {}) const;
    // The bodies whose solid contains `point`, by body number (a mesh or height field has no inside).
    std::vector<Body3D> point_inside(Vec3f point, const QueryFilter3D& filter = {}) const;
    // The nearest surface within `max_distance` of `point`; a point inside a solid is at distance 0 on it.
    std::optional<ClosestPoint3D> closest_point(Vec3f point, f32 max_distance, const QueryFilter3D& filter = {}) const;

    // --- events ---
    // The contact events of the steps since the last drain: each step's batch sorted by (a, b, kind) with
    // generations after, batches in step order. They name bodies by handle and by user id (the ECS puts the
    // entity's id there).
    std::span<const ContactEvent3D> events() const;
    // Takes them (and clears).
    std::vector<ContactEvent3D> drain_events();
    // Events dropped because `max_events` was reached since the world was made.
    u64 dropped_events() const;
    // Constraints that broke, by step then constraint number.
    std::vector<ConstraintEvent3D> drain_constraint_events();

    // --- constraints ---
    // An invalid handle for an invalid body, the same body twice, or a non-finite number. Constraints are
    // destroyed with either of their bodies.
    Constraint3D create_constraint(const Constraint3DDesc& desc);
    void destroy_constraint(Constraint3D constraint);
    bool contains(Constraint3D constraint) const;
    i32 constraint_count() const;
    Constraint3DInfo constraint_info(Constraint3D constraint) const;
    // Hinge (target in degrees or degrees a second; max_force a torque in N m) and slider (metres, m/s, N).
    // `max_force` must be above 0 for a mode but Off.
    void set_constraint_motor(Constraint3D constraint, Motor3DMode mode, f32 target, f32 max_force);
    // A disabled constraint does nothing (a broken one is disabled; enabling it mends it).
    void set_constraint_enabled(Constraint3D constraint, bool enabled);
    // The constraints' live handles, by number.
    std::vector<Constraint3D> constraints() const;

    // --- characters (character3d.hpp) ---
    // A capsule controller with its feet at desc.position. An invalid handle for a non-finite number, a radius or
    // height that is not positive (or a height under 2 x radius) or a layer out of range. The inner body is made now
    // (it takes the next body number).
    Character3D create_character(const CharacterDesc3D& desc);
    // Does nothing for a handle that is not (or no longer) a character; the body goes with it.
    void destroy_character(Character3D character);
    bool contains(Character3D character) const;
    i32 character_count() const;
    // The live characters, by number: the order step() updates them in.
    std::vector<Character3D> characters() const;
    // The command for the coming steps (held until set again). Takes effect at the next step().
    void set_character_input(Character3D character, const CharacterInput3D& input);
    CharacterInput3D character_input(Character3D character) const;
    // Everything readable about it, as the last step() left it (a default state for a dead handle).
    CharacterState3D character(Character3D character) const;
    // What it touches (see CharacterContact3D), sorted by body number.
    std::vector<CharacterContact3D> character_contacts(Character3D character) const;
    // To a place at once: velocity cleared, the up vector kept, the ground looked for at once. (The next step moves
    // it from there.)
    void teleport_character(Character3D character, Vec3f feet);
    // Makes `up` the character's explicit up vector (any length but zero; zero goes back to against the world's
    // gravity). It turns toward it at CharacterDesc3D::up_blend_rate, or at once when `instant`.
    void set_character_up(Character3D character, Vec3f up, bool instant = false);
    // Crouches or stands at once. Standing is refused (false, and it stays crouched) when the standing capsule
    // would hit something above. The next step's input.crouch decides again.
    bool set_character_crouch(Character3D character, bool crouch);
    // Changes how it is simulated while it lives: the numbers of the descriptor but the shape, layer and position
    // (radius, heights, layer and mask are fixed at creation).
    void set_character_desc(Character3D character, const CharacterDesc3D& desc);
    // The events of the steps since the last drain (landings, leaving the ground, head bumps, contacts begun and ended),
    // each step's batch in character order, then in the order the kinds are listed.
    std::span<const CharacterEvent3D> character_events() const;
    std::vector<CharacterEvent3D> drain_character_events();

    // --- grab and climb (traversal3d.hpp) ---
    // What a body's surface is to a hand: grippable, how well, how slippery, how tiring. Data on the body: ice or wet rock is
    // a material, not code. Not in the hash or the saved state (it is the level's setup, as shapes are); set it when the
    // body is made, the same in every run. Does nothing for a handle that is not a body.
    void set_surface_material(Body3D body, const SurfaceMaterial3D& material);
    SurfaceMaterial3D surface_material(Body3D body) const;
    // Marks a body as an authored hold (an own grip quality; a fixed grip point if asked). Returns false for a body that
    // is not one.
    bool set_hold(Body3D body, const HoldDesc3D& hold);
    void clear_hold(Body3D body);
    bool is_hold(Body3D body) const;
    HoldDesc3D hold(Body3D body) const;
    // What a hand could take hold of from `probe.origin` along `probe.direction`, best first (score, then body number and
    // place): a fan of sphere sweeps and rays against the layers of `probe.mask`, each hit made a candidate by the hold,
    // ledge and surface providers (one candidate a hit). Reads the world as the last step left it; nothing is held.
    std::vector<GripCandidate3D> probe_grips(const GripProbeDesc3D& probe) const;
    // The limbs' command for the coming steps (held until set again), what they hold and the resource as the last step
    // left it. A character with CharacterDesc3D::traversal.enabled false has none (a default state).
    void set_traversal_input(Character3D character, const TraversalInput3D& input);
    TraversalInput3D traversal_input(Character3D character) const;
    TraversalState3D traversal(Character3D character) const;
    // Sets the resource's value (clamped to 0..max; an item that gives stamina, a respawn). 0 counts as exhausted; the
    // recovery rule is the resource's own.
    void set_traversal_resource(Character3D character, f32 value);

    // Sleeping. activate() wakes a body (its island with it, at the next step);
    // deactivate() puts it to sleep at once.
    bool is_active(Body3D body) const;
    void activate(Body3D body);
    void deactivate(Body3D body);

    // Which layer pairs collide (symmetric; all do at first except sensor with
    // sensor). Bodies already touching keep their contacts until they part.
    void set_layers_collide(u8 a, u8 b, bool collide);
    bool layers_collide(u8 a, u8 b) const;
    // The layers that layer `layer` collides with, as a mask.
    Layer3DMask collision_mask(u8 layer) const;

    // Rebuilds the broad phase for fast queries after many bodies were created
    // or destroyed together (a level loaded). Costly: not every frame.
    void optimize_broad_phase();

    // A hash of everything the step produced, in a stable order: the bodies in
    // creation-number order, each with its number, generation, user id, motion,
    // layer, whether it is awake, position, rotation, linear and angular
    // velocity. Floats go in bit for bit (no rounding; every NaN as one NaN,
    // -0 apart from +0), integers as little-endian bytes, through FNV-1a 64, so
    // that a hash is the same on every platform and run.
    u64 state_hash() const;

    // The world's dynamic state (bodies' transforms and velocities, sleeping,
    // contact caches) as bytes, for a rollback. restore_state() takes bytes
    // from this world with the same set of bodies (none created or destroyed
    // since, shapes, layers and gravity as they were; constraints likewise,
    // with their motors and whether they broke) and returns false, doing
    // nothing, otherwise. Pending events are dropped by a restore. A restored world steps to the same bits as the saved
    // one did.
    std::vector<u8> save_state() const;
    bool restore_state(std::span<const u8> state);

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace kin
