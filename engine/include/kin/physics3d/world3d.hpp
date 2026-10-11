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

#include <kin/core/jobs.hpp>
#include <kin/core/math3d.hpp>
#include <kin/core/types.hpp>
#include <kin/physics3d/shape3d.hpp>

#include <memory>
#include <span>
#include <vector>

namespace kin {

enum class Body3DMotion : u8 {
    Static,    // never moves (moved by set_position it stays where put; costs nothing)
    Kinematic, // moved by the caller (velocity or move_kinematic), pushes dynamic bodies
    Dynamic,   // moved by forces, contacts and gravity
};

// A body in a World3D: an index and a generation, so that a handle to a
// destroyed body never names the body that took its place.
struct Body3D {
    u32 index = UINT32_MAX;
    u32 generation = 0;

    bool valid() const { return index != UINT32_MAX; }
    explicit operator bool() const { return valid(); }
    friend constexpr bool operator==(Body3D, Body3D) = default;
};

// Collision layers: a body is in one of 32, and a world holds a 32 x 32 table of
// which pairs collide. A set of layers is a 32-bit mask, bit n for layer n, also
// what queries filter by (a ray that ignores the character's layer, ...).
using Layer3DMask = u32;

namespace layers3d {
// Layer numbers. 0 is for static bodies only (it is its own broad phase tree);
// the rest are free for any body but are named for what they are usually used for.
inline constexpr u8 Static = 0;
inline constexpr u8 Dynamic = 1;
inline constexpr u8 Kinematic = 2;
inline constexpr u8 Character = 3;
// Detects but does not collide: sensors pair with nothing else's layer by
// default (a trigger volume sits here). Events come with P2b's queries.
inline constexpr u8 Sensor = 4;
inline constexpr u8 User0 = 5; // 5 to 31 are the game's
inline constexpr u8 Count = 32;
// "The layer that goes with the motion type": Static, Kinematic or Dynamic.
inline constexpr u8 Auto = 255;
} // namespace layers3d

constexpr Layer3DMask layer_bit(u8 layer) { return layer < 32 ? (Layer3DMask{1} << layer) : 0; }
inline constexpr Layer3DMask all_layers3d = 0xFFFFFFFFu;

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
    // since, shapes, layers and gravity as they were) and returns false, doing
    // nothing, otherwise. A restored world steps to the same bits as the saved
    // one did.
    std::vector<u8> save_state() const;
    bool restore_state(std::span<const u8> state);

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace kin
