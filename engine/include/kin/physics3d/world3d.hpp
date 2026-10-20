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
// (tested, 1 worker against 4 against none; not in a step that overflowed a
// capacity, see StepReport3D). The library is built with Jolt's
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
#include <kin/physics3d/ragdoll3d.hpp>
#include <kin/physics3d/rope3d.hpp>
#include <kin/physics3d/shape3d.hpp>

#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace kin {
class SceneQueries3D;
namespace physics3d_detail {
class RagdollSet;
class RopeSet;
}
namespace detail {
// Internal typed access to caller-owned raw or scene-hit arrays. No temporary
// result list is needed when the physics record is a member of a larger hit.
template<class T> struct QueryOutput3D {
    void* data=nullptr;std::size_t capacity=0;
    T& (*at)(void*,std::size_t)=nullptr;
    T& operator[](std::size_t i)const{return at(data,i);}
};
}
struct BoundedQueryResult3D {
    std::size_t count=0;
    bool overflow=false;
    bool valid=true; // false for invalid input or an unavailable scene endpoint
};

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
    // Exclude every body carrying this nonzero user id (an ECS entity, including
    // its character/ragdoll/rope bodies). Independent of collision groups.
    u64 ignore_user_id = 0;
    // Sweeps only: include initial convex overlap while moving outward and
    // compute its penetration. False retains the legacy directional behavior.
    bool initial_overlap = false;
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
    // Opaque native sub-shape key used to break equal-distance ties.
    u32 sub_shape_id = 0;
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
    // Opaque native sub-shape key used to break equal-fraction ties (shape_cast_all).
    u32 sub_shape_id = 0;
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
    u32 sub_shape = 0; // top-level compound child, in authored order
    u32 sub_shape_id = 0; // opaque key for equal-penetration ties
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

// A body that woke or fell asleep (World3DConfig::activation_events): what a game that only follows awake bodies, or plays
// a sound when a pile settles, wants to know.
struct ActivationEvent3D {
    Body3D body{};
    u64 user_id = 0;
    // Woke (made awake counts: a body made with start_active, a push, a contact) or fell asleep (deactivate() counts).
    bool active = false;
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

    // Solver iterations for the island this constraint is in (0: the world's 10 velocity and 2 position steps; the island
    // runs the most any of its constraints asks for). A chain of very different masses (a rope with a climber on it) needs
    // many.
    u32 velocity_steps = 0;
    u32 position_steps = 0;

    // Whether the two bodies it joins still collide with each other. Off by default (a door and its post, the links
    // of a chain, a limb on its trunk: they would fight the joint), which also saves their contact work.
    // The filter runs in Jolt's contact validation, which is called whether contact events are on or not.
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

// What a body belongs to (World3D::owner_of). A character's, ragdoll's or rope's bodies are that thing's to destroy, reshape
// and switch between kinematic and dynamic: destroy_body, set_shape and set_motion refuse them (false, and a line in the log),
// as they would leave it holding a dead or changed body (a rope's tube collapsing to the origin). Queries, events, forces and
// the other calls take a ragdoll's or rope's bodies as any body; the calls that change a body leave a character's own alone
// (the character moves it), and queries and events take it as any body.
enum class BodyOwner3D : u8 {
    None,      // the game's: made by create_body
    Character, // a character controller's own capsule
    Ragdoll,   // a bone of a ragdoll
    Rope,      // a segment of a rope
};

struct Body3DDesc {
    Shape3D shape;
    Body3DMotion motion = Body3DMotion::Dynamic;
    Vec3f position{};
    Quatf rotation{};
    Vec3f linear_velocity{};
    Vec3f angular_velocity{};
    // Dynamic and kinematic bodies: mass in kg when above zero, else density (kg / m^3,
    // water is 1000) times the shape's volume. The inertia follows from the shape
    // (scaled to the mass). A kinematic body has them for when set_motion makes it
    // dynamic (a mesh or height field has none).
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
    // Bodies that share a non-zero group never collide with each other (the bones of a ragdoll, the segments of a rope), unless
    // both have `group_self_collision`. Queries that ignore a body also ignore the bodies of its group, and so does a
    // character's own movement (set_group on its body).
    u32 group = 0;
    bool group_self_collision = false;
    // Never collides with static bodies (a rope that passes through the level) / with anything (a marker that only follows).
    bool ignore_static = false;
    bool ignore_all = false;
    // Solver iterations for this body's island (0: the world's): a chain of very different masses wants more.
    u8 velocity_steps = 0;
    u8 position_steps = 0;
    // Yours: not read by the engine; hashed into state_hash().
    u64 user_id = 0;
};

struct World3DConfig {
    Vec3f gravity{0.0f, -9.81f, 0.0f};
    // Collision steps per step(): each is dt / collision_steps long, with its own
    // collision detection and solve. 1 is right at 60 Hz and below; raise it
    // for fast thin things instead of making dt smaller.
    i32 collision_steps = 1;
    // Capacities (they are reserved at creation and not grown). Past max_body_pairs (pairs of bodies near enough to be
    // tested) or max_contact_constraints (contact manifolds with a dynamic body: one a touching pair of convex shapes, more
    // for compounds and meshes; it also sizes the contact cache), Jolt drops the contacts that did not fit for that step:
    // step() says so (StepReport3D), the log says it once a world, and overflowed_steps() counts it.
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
    // Contact events (begin and end of each body pair's touching, sensor overlaps), opt-in. Off costs
    // nothing: a game that drains them and throws them away should turn them off (each is recorded in Jolt's
    // callback under a lock; the character, ragdoll, rope and constraint events and collide_connected do not
    // depend on them). Persist events (one a step for every touching pair) are for tools and tests:
    // they cost a record a pair a step.
    bool contact_events = false;
    bool persist_events = false;
    // Bodies waking and falling asleep (ActivationEvent3D). Off costs nothing (no listener is given to Jolt); on, each change
    // is recorded (under a lock while a step runs). A character's own capsule never sleeps and is not reported.
    bool activation_events = false;
    // The events kept between drains: past it the newest are dropped (the earlier ones are kept) and counted by
    // dropped_events(). The same for the character and ragdoll events, each list on its own.
    u32 max_events = 1u << 16;
};

// What a step had to leave out because a capacity of World3DConfig was reached: Jolt dropped the contacts that did not fit
// for that step (bodies may sink into each other or through the floor). All false in a step that fitted. Which contacts
// are dropped depends on the order of the worker threads, so such a step is not deterministic across worker counts: size
// the capacities for the busiest scene (Jolt does not say how many pairs or constraints a step used).
struct StepReport3D {
    bool manifold_cache_full = false;      // the contact cache: raise max_contact_constraints
    bool body_pair_cache_full = false;     // raise max_body_pairs
    bool contact_constraints_full = false; // raise max_contact_constraints
    bool any() const { return manifold_cache_full || body_pair_cache_full || contact_constraints_full; }
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
    // last step are applied and forces then cleared. Returns what the step had to
    // leave out (all false when it fitted; a dt that is not positive and finite
    // does nothing and reports nothing); last_step() keeps it.
    StepReport3D step(f32 dt);
    // The report of the last step that ran.
    const StepReport3D& last_step() const;

    // Bodies. A body from an invalid shape, a dynamic body of a mesh or height
    // field, a Static or Kinematic... in layer 0 (see layers3d) or a non-finite
    // number is refused: an invalid handle comes back.
    Body3D create_body(const Body3DDesc& desc);
    // False, doing nothing, for a handle that is not (or no longer) a body, or a body that belongs to a character,
    // ragdoll or rope (BodyOwner3D: destroy that instead).
    bool destroy_body(Body3D body);
    // What the body belongs to (None for a dead handle).
    BodyOwner3D owner_of(Body3D body) const;
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
    // What it was made with (Body3DDesc::mass) or last given by set_mass, also while kinematic; 0 for a static body, a
    // character's, or a kinematic one without (a mesh, a height field, no volume).
    f32 mass(Body3D body) const;
    // The rest of Body3DDesc, live: what a contact, gravity and damping use from the next step on. Each returns false,
    // changing nothing, for a dead handle, a number that is not finite or out of range (the log says so once a world), or a
    // body that belongs to a character, ragdoll or rope (BodyOwner3D: their descriptors own these numbers; a line in the log
    // once a world). Not in state_hash() or save_state(): like a shape, a layer and the gravity they are the body's setup,
    // which a restore needs as it was saved.
    //  * set_mass: above 0, for a dynamic body or a kinematic one that can have a mass (not a mesh or height field); the
    //    inertia is the shape's at that mass. A mass added for the step (set_added_mass) stays on top. Wakes the body.
    //  * set_gravity_factor (any finite number: 0 floats, below 0 rises), set_linear_damping and set_angular_damping (0 or
    //    more, a fraction of the speed a second): dynamic and kinematic bodies (a kinematic one keeps them for set_motion).
    //    The gravity factor wakes the body.
    //  * set_friction and set_restitution (0 or more): every body, a static one too (a floor that freezes over). Friction
    //    wakes the body, and for a static or kinematic one what touches it (a crate asleep on a slope that turns to ice
    //    slides); restitution only matters at the next impact.
    bool set_mass(Body3D body, f32 mass);
    bool set_gravity_factor(Body3D body, f32 factor);
    bool set_linear_damping(Body3D body, f32 damping);
    bool set_angular_damping(Body3D body, f32 damping);
    bool set_friction(Body3D body, f32 friction);
    bool set_restitution(Body3D body, f32 restitution);
    // As they are now (0 for a dead handle; the gravity factor and dampings are 0 for a static body, which has none).
    f32 gravity_factor(Body3D body) const;
    f32 linear_damping(Body3D body) const;
    f32 angular_damping(Body3D body) const;
    f32 friction(Body3D body) const;
    f32 restitution(Body3D body) const;
    // The shape it was made with or last given by set_shape: the same handle (`==` holds), kept with the body, so asking for every
    // body every frame costs a reference count. A character's own body has no handle of the game's: a wrapper of Jolt's shape.
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
    // body is woken. Returns false when refused, also for a body that belongs to a character, ragdoll or rope.
    bool set_shape(Body3D body, const Shape3D& shape);
    // The live bodies, by body number.
    std::vector<Body3D> bodies() const;
    // A dynamic body becomes kinematic (a ragdoll animated by a clip: the body follows and pushes) or a kinematic one
    // dynamic, with the mass it was made with. Velocities are kept. Returns false for a static body, a motion of Static,
    // a kinematic body without a mass (a mesh or height field, a shape of no volume) made dynamic, or a body that belongs
    // to a character, ragdoll or rope.
    bool set_motion(Body3D body, Body3DMotion motion);
    // A mass (kg) added to a dynamic body at its centre for the next step only, like a force (set it every step it should
    // count; the step clears it): the inertia of something that hangs from the body (a climber on a rope; the weight comes
    // with gravity acting on the whole). The body's own mass is what it was made with. 0 takes it off.
    void set_added_mass(Body3D body, f32 extra);
    f32 added_mass(Body3D body) const;
    // Bodies of one non-zero group do not collide with each other; 0 clears it. Also for a character's own body (it then
    // passes through, and its queries ignore, that group's bodies: its ragdoll).
    void set_group(Body3D body, u32 group);
    u32 group(Body3D body) const;
    // The body collides with nothing while set (a character that gave its place to a ragdoll).
    void set_no_collisions(Body3D body, bool no_collisions);
    // Moves a Point constraint's anchor on one side (0: body a, 1: body b) to `local_point`, in that body's own frame (origin and
    // rotation; the world's for a side that is fixed to the world), without making a new constraint: a rope that changes length.
    // False for another kind of constraint, a stale handle or a side that is not 0 or 1.
    bool set_constraint_anchor(Constraint3D constraint, i32 side, Vec3f local_point);

    // --- queries ---
    // The first body along the segment from `from` to `to`.
    std::optional<RayHit3D> raycast(Vec3f from, Vec3f to, const QueryFilter3D& filter = {}) const;
    // Every body the segment crosses (a convex body once, a mesh once for the nearest of its triangles on each
    // sub shape), nearest first, ties by body number then sub shape.
    std::vector<RayHit3D> raycast_all(Vec3f from, Vec3f to, const QueryFilter3D& filter = {}) const;
    // No result-list allocation. Keep the nearest output.size() hits in the same
    // order as raycast_all; overflow says more hits existed. Empty output probes
    // for any hit. Only [0,count) is written; invalid input sets valid=false.
    BoundedQueryResult3D raycast_all(Vec3f from, Vec3f to, std::span<RayHit3D> output,
                                    const QueryFilter3D& filter = {}) const;
    // `shape` placed at `position` and `rotation` (its origin, as a body's) swept by `displacement`: the first
    // thing it meets, with the time of impact. Not for meshes or height fields. A shape that starts overlapping
    // reports fraction 0 and a penetration. Ties go to the lower body number.
    std::optional<ShapeHit3D> shape_cast(const Shape3D& shape, Vec3f position, Quatf rotation, Vec3f displacement,
                                         const QueryFilter3D& filter = {}) const;
    // Every body the swept shape meets, each once at its first time of impact (a compound or mesh body at the earliest of
    // its sub shapes), by fraction, ties by body number then native sub-shape key. The bounded form allocates no result
    // list and keeps the first output.size() of that order (overflow: more bodies were met; an empty output probes for any).
    // Only [0, count) is written; invalid input (as shape_cast refuses) sets valid = false.
    std::vector<ShapeHit3D> shape_cast_all(const Shape3D& shape, Vec3f position, Quatf rotation, Vec3f displacement,
                                           const QueryFilter3D& filter = {}) const;
    BoundedQueryResult3D shape_cast_all(const Shape3D& shape, Vec3f position, Quatf rotation, Vec3f displacement,
                                        std::span<ShapeHit3D> output, const QueryFilter3D& filter = {}) const;
    // The bodies `shape` at `position` and `rotation` overlaps, sorted by body number (one entry for each).
    std::vector<OverlapHit3D> overlap(const Shape3D& shape, Vec3f position, Quatf rotation,
                                      const QueryFilter3D& filter = {}) const;
    // Bounded, unique-body results: retain the lowest body numbers, with each
    // body's deepest contact (ties by native sub-shape key). No result-list allocation.
    BoundedQueryResult3D overlap(const Shape3D& shape, Vec3f position, Quatf rotation,
                                std::span<OverlapHit3D> output, const QueryFilter3D& filter = {}) const;
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
    // Reuses the caller's previous event storage for the next step. Replaces output's contents.
    void drain_events(std::vector<ContactEvent3D>& output);
    // The bodies that woke or fell asleep since the last drain (World3DConfig::activation_events; empty when it is off), in
    // the order they changed: the calls between steps in call order (a push, activate(), a body made awake), the work of
    // step() around the simulation likewise (characters pushing, ragdolls and ropes), and what the simulation itself
    // changed (contacts waking bodies, bodies falling asleep) as one group by body number, where it ran in the step.
    std::span<const ActivationEvent3D> activation_events() const;
    std::vector<ActivationEvent3D> drain_activation_events();
    void drain_activation_events(std::vector<ActivationEvent3D>& output);
    // Contact, character, ragdoll and activation events dropped because `max_events` was reached since the world was made.
    u64 dropped_events() const;
    // Steps since the world was made in which a capacity was reached (StepReport3D::any()).
    u64 overflowed_steps() const;
    // Constraints that broke, by step then constraint number.
    std::vector<ConstraintEvent3D> drain_constraint_events();
    void drain_constraint_events(std::vector<ConstraintEvent3D>& output);

    // --- constraints ---
    // An invalid handle for an invalid body, the same body twice, or a non-finite number. Constraints are
    // destroyed with either of their bodies.
    Constraint3D create_constraint(const Constraint3DDesc& desc);
    void destroy_constraint(Constraint3D constraint);
    bool contains(Constraint3D constraint) const;
    i32 constraint_count() const;
    Constraint3DInfo constraint_info(Constraint3D constraint) const;
    // The linear impulse (N s) a Point constraint applied to its second body (b) in the last collision step: divide by the
    // collision step's length (step dt / collision_steps) for a force. Zero for other kinds.
    Vec3f constraint_impulse(Constraint3D constraint) const;
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
    // it from there.) A velocity or impulses given before it are dropped.
    void teleport_character(Character3D character, Vec3f feet);
    // Pushes from outside (wind, a knock back, a blast, a launch pad), taken by the next step and then cleared: see
    // character3d.hpp "Pushed from outside" for how they combine with the command. An impulse (N s) changes the velocity by
    // impulse / CharacterDesc3D::mass; impulses add up until the step. A velocity (m/s, in the world: the ground's motion
    // included) replaces the one the character has, and the impulses given before it. Either, when it sends the character
    // away from the ground at 0.1 m/s or more, launches it: it leaves the ground as a jump does. A dead or disabled
    // character, or a vector that is not finite, does nothing (and the log says so once a world). In state_hash() and
    // save_state() while they wait.
    void add_character_impulse(Character3D character, Vec3f impulse);
    void set_character_velocity(Character3D character, Vec3f velocity);
    // Makes `up` the character's explicit up vector (any length but zero; zero goes back to against the world's
    // gravity). It turns toward it at CharacterDesc3D::up_blend_rate, or at once when `instant`.
    void set_character_up(Character3D character, Vec3f up, bool instant = false);
    // Crouches or stands at once. Standing is refused (false, and it stays crouched) when the standing capsule
    // would hit something above. The next step's input.crouch decides again.
    bool set_character_crouch(Character3D character, bool crouch);
    // Hands the place over to something else (a ragdoll): a disabled character is not updated (no input, no events), its
    // body collides with nothing, and anything it held is let go. Re-enable it with teleport_character first or after: it
    // starts from rest, in the air, at the place it was left. A handle that is not a character does nothing.
    void set_character_enabled(Character3D character, bool enabled);
    bool character_enabled(Character3D character) const;
    // Changes how it is simulated while it lives: the numbers of the descriptor but the shape, layer and position
    // (radius, heights, layer and mask are fixed at creation).
    void set_character_desc(Character3D character, const CharacterDesc3D& desc);
    // The events of the steps since the last drain (landings, leaving the ground, head bumps, contacts begun and ended),
    // each step's batch in character order, then in the order the kinds are listed.
    std::span<const CharacterEvent3D> character_events() const;
    std::vector<CharacterEvent3D> drain_character_events();
    void drain_character_events(std::vector<CharacterEvent3D>& output);

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

    // --- ragdolls (ragdoll3d.hpp) ---
    // A body for each bone of `desc` placed at the matrix of `bone_world[i]` (translation and rotation; the scale is left out and
    // `scale` multiplies the shapes' sizes and offsets), joined as the desc says. Starts Animated: kinematic bodies at that pose.
    // An invalid handle for a bad desc (no bones, a parent not lower, a shape that cannot be made) or a matrix count that is
    // not the bone count.
    RagdollHandle3D create_ragdoll(const RagdollDesc& desc, std::span<const Mat4f> bone_world, f32 scale = 1.0f);
    // Destroys its bodies and joints.
    void destroy_ragdoll(RagdollHandle3D ragdoll);
    bool contains(RagdollHandle3D ragdoll) const;
    i32 ragdoll_count() const;
    std::vector<RagdollHandle3D> ragdolls() const;
    // The bodies, by bone (invalid for a bone that was left out).
    std::vector<Body3D> ragdoll_bodies(RagdollHandle3D ragdoll) const;
    bool ragdoll_has_body(RagdollHandle3D ragdoll, Body3D body) const;
    const RagdollDesc* ragdoll_desc(RagdollHandle3D ragdoll) const;
    // Where the clip puts the bones, in the world (a matrix a bone): what the kinematic bodies follow in the next step.
    void set_ragdoll_targets(RagdollHandle3D ragdoll, std::span<const Mat4f> bone_world);
    // The bodies take over: dynamic, blending in over `blend_seconds`. Starting again while a get-up runs turns it round.
    void start_ragdoll(RagdollHandle3D ragdoll, const RagdollStart3D& start = {});
    // Holds `alpha` (0..1) of the pose from the bodies, reaching it over `seconds`: dynamic bodies while above 0.
    void blend_ragdoll(RagdollHandle3D ragdoll, f32 alpha, f32 seconds);
    // The pose blends from where the bodies lie now to the clip over `seconds` (the share goes to 0, Recovered at the end).
    // The bodies themselves are kinematic on the clip from the first step, so the character's own capsule and anything near
    // it are not in a fight with a lying body. The game has the clip playing and the character put where
    // ragdoll_get_up_place says.
    void ragdoll_get_up(RagdollHandle3D ragdoll, f32 seconds);
    // Back to Animated at once: kinematic bodies at the targets with no velocity.
    void animate_ragdoll(RagdollHandle3D ragdoll);
    // Per bone: 0 keeps it animated (kinematic) whatever the mode, 1 is the whole ragdoll's share. Missing entries are 1.
    void set_ragdoll_bone_weights(RagdollHandle3D ragdoll, std::span<const f32> weights);
    // `bone` and the bones below it at `weight`, every other bone at 0: a partial ragdoll (an arm, a tail).
    void set_ragdoll_subtree(RagdollHandle3D ragdoll, i32 bone, f32 weight = 1.0f);
    RagdollState3D ragdoll_state(RagdollHandle3D ragdoll) const;
    // The pose writer's input (`out` gets one entry a bone).
    void ragdoll_pose(RagdollHandle3D ragdoll, std::vector<RagdollBonePose3D>& out) const;
    // Where to stand a character that gets up: the ground under the root bone along `up` (within `max_distance`), against
    // the layers in `mask`, ignoring the ragdoll's own bodies and optional `ignore` (its controller capsule); and the way it faces.
    RagdollPlace3D ragdoll_get_up_place(RagdollHandle3D ragdoll, Vec3f up = {0.0f, 1.0f, 0.0f},
                                        Layer3DMask mask = all_layers3d, f32 max_distance = 3.0f, Body3D ignore = {}) const;
    // The ragdoll events since the last drain, each step's batch sorted by (ragdoll, kind).
    std::span<const RagdollEvent3D> ragdoll_events() const;
    std::vector<RagdollEvent3D> drain_ragdoll_events();
    void drain_ragdoll_events(std::vector<RagdollEvent3D>& output);

    // --- ropes (rope3d.hpp) ---
    // A chain of capsules between the anchors. An invalid handle for a non-finite number, no length where one is needed, a
    // body anchor that is not a body, or a world out of room.
    RopeHandle3D create_rope(const RopeDesc3D& desc);
    // Destroys its bodies and joints.
    void destroy_rope(RopeHandle3D rope);
    bool contains(RopeHandle3D rope) const;
    i32 rope_count() const;
    std::vector<RopeHandle3D> ropes() const;
    // The polyline: segments + 1 joints with the direction there.
    std::vector<RopePoint3D> rope_points(RopeHandle3D rope) const;
    std::vector<RopeSegment3D> rope_segments(RopeHandle3D rope) const;
    RopeState3D rope_state(RopeHandle3D rope) const;
    // The closest point of the rope to `point`, by segment and parameter.
    std::optional<RopeClosest3D> rope_closest_point(RopeHandle3D rope, Vec3f point) const;
    // The point at `fraction` (-0.5 to 0.5 of the segment's length from its middle) of a segment, and the segment's direction.
    bool rope_point_at(RopeHandle3D rope, u32 segment, f32 fraction, Vec3f& point, Vec3f& tangent) const;
    // Which rope and segment a body is.
    std::optional<RopeSegmentRef3D> rope_segment_of(Body3D body) const;
    // The body of segment `segment` (invalid when out of range) and the rope's radius.
    Body3D rope_segment_body(RopeHandle3D rope, u32 segment) const;
    f32 rope_radius(RopeHandle3D rope) const;
    // The force the rope puts on its anchor (N, in the world; zero for a free end), and its size along the rope: positive
    // when it pulls the anchor toward the rope. Of the last step.
    Vec3f rope_anchor_force(RopeHandle3D rope, RopeEnd3D end) const;
    f32 rope_tension(RopeHandle3D rope, RopeEnd3D end) const;
    // Rest length at once (the chain is re-laid along itself), or moved toward `target` at `speed` m/s by the steps.
    void set_rope_length(RopeHandle3D rope, f32 length);
    void reel_rope(RopeHandle3D rope, f32 target, f32 speed);
    // Ropes that broke, by step then rope then joint.
    std::vector<RopeEvent3D> drain_rope_events();
    void drain_rope_events(std::vector<RopeEvent3D>& output);

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

    // The world's dynamic state (bodies' transforms, velocities, motion types, one-step added mass, sleeping,
    // contact caches) as bytes, for a rollback. restore_state() takes bytes
    // from this world with the same set of bodies (none created or destroyed
    // since, shapes, layers, gravity, masses, frictions, restitutions, dampings
    // and gravity factors as they were; constraints likewise,
    // with their motors and whether they broke) and returns false, doing
    // nothing, otherwise; also for bytes that were cut short or changed (the
    // stream ends with its length and a check of its bytes, read before anything
    // is touched). Pending events are dropped by a restore. A restored world steps to the same bits as the saved
    // one did.
    std::vector<u8> save_state() const;
    bool restore_state(std::span<const u8> state);

private:
    friend class SceneQueries3D;
    // The ragdoll and rope sets make their bodies with create_body, mark them as theirs, and then destroy, reshape and
    // switch them by these, which the public calls refuse for a body that belongs to something (BodyOwner3D).
    friend class physics3d_detail::RagdollSet;
    friend class physics3d_detail::RopeSet;
    void set_owner(Body3D body, BodyOwner3D owner);
    void owner_destroy_body(Body3D body);
    bool owner_set_shape(Body3D body, const Shape3D& shape);
    bool owner_set_motion(Body3D body, Body3DMotion motion);
    BoundedQueryResult3D raycast_into(Vec3f from,Vec3f to,detail::QueryOutput3D<RayHit3D> output,const QueryFilter3D& filter)const;
    BoundedQueryResult3D overlap_into(const Shape3D& shape,Vec3f position,Quatf rotation,detail::QueryOutput3D<OverlapHit3D> output,const QueryFilter3D& filter)const;
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace kin
