#pragma once

#include <kin/core/types.hpp>

#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace kin {

enum class PhysicsBodyType {
    Static,
    Dynamic,
    Kinematic,
};

enum class PhysicsShapeType {
    Box,
    Circle,
    Capsule, // a box with round ends: the ends of its longer side
    Polygon, // any simple polygon (a concave one is cut in triangles)
    Chain,   // a line through points (a loop: closed): it has no inside
};

enum class PhysicsContactEventType {
    Begin,
    End,
};

enum class PhysicsDebugShapeType {
    Polygon,
    Circle,
    Chain, // a line through its points
};

enum class PhysicsJointType {
    Hinge,    // they turn about one point (Box2D's revolute joint)
    Slider,   // one slides along an axis of the other, neither turning (prismatic)
    Distance, // two points kept apart: a rod, a rope (limits), a spring
    Weld,     // held as they are (springy, with a frequency)
    Wheel,    // a hinge on a spring along an axis: a car's wheel
};

struct PhysicsBody {
    u32 index = UINT32_MAX;
    u32 generation = 0;

    bool valid() const { return index != UINT32_MAX; }
    explicit operator bool() const { return valid(); }
    friend constexpr bool operator==(PhysicsBody, PhysicsBody) = default;
};

struct PhysicsFixture {
    u32 index = UINT32_MAX;
    u32 generation = 0;

    bool valid() const { return index != UINT32_MAX; }
    explicit operator bool() const { return valid(); }
    friend constexpr bool operator==(PhysicsFixture, PhysicsFixture) = default;
};

struct PhysicsJoint {
    u32 index = UINT32_MAX;
    u32 generation = 0;

    bool valid() const { return index != UINT32_MAX; }
    explicit operator bool() const { return valid(); }
    friend constexpr bool operator==(PhysicsJoint, PhysicsJoint) = default;
};

// Lengths, positions, velocities, forces and impulses given to a PhysicsWorld,
// and those it gives back, are in the game's units (gravity in units/s²);
// masses in kilograms, densities in kilograms per square metre. Box2D is
// tuned for bodies of 0.1 to 10 metres and moves none more than 2 metres a
// step: a game whose units are points (a 32 point crate) says how many make a
// metre, and the world converts. One (the default): the units are metres.
struct PhysicsWorldDef {
    Vec2f gravity{};
    f32 units_per_meter = 1.0f;
    // Threads a step is solved on, the calling one included (Box2D 3 only,
    // KIN_PHYSICS_BOX2D_V3: 2.4 has one). 0: KIN_PHYSICS_WORKERS when set,
    // else one per core, up to 8. Its results are the same however many.
    i32 workers = 0;
};

struct PhysicsBodyDef {
    PhysicsBodyType type = PhysicsBodyType::Static;
    Vec2f position{};
    f32 rotation = 0.0f; // radians, as Box2D (Transform2D is degrees; the ECS sync converts)
    Vec2f linear_velocity{};
    f32 angular_velocity = 0.0f;
    f32 linear_damping = 0.0f;
    f32 angular_damping = 0.0f;
    f32 gravity_scale = 1.0f;
    bool fixed_rotation = false;
    u64 user_id = 0;
    // A body at rest falls asleep (and stops being simulated) unless this is
    // off; one that never sleeps keeps seeing what comes to touch it.
    bool allow_sleep = true;
    // A character's (moved by move_character, which keeps it clear of what
    // stops it): static and kinematic bodies don't push it (dropping through
    // a one-sided platform it stood on, nothing holds it up). Dynamic bodies
    // still bump into it, and sensors see it.
    bool character = false;
};

// A shape on a body, from the body's origin: centred at `offset` and turned
// by `angle` there (a box's, a capsule's; a polygon's or a chain's points
// are from `offset`, turned by `angle` about it).
struct PhysicsShape {
    PhysicsShapeType type = PhysicsShapeType::Box;
    Vec2f half_extents{0.5f, 0.5f}; // a box's; a capsule's (the box it fills)
    Vec2f offset{};
    f32 radius = 0.5f; // a circle's
    f32 angle = 0.0f;  // radians
    std::vector<Vec2f> points{}; // a polygon's corners, a chain's points
    bool loop = false;      // a chain's: back from its last point to its first
    // A platform to jump up through. A chain: it stops what comes from one
    // side of it only, the left going from point to point (on screen, y down:
    // drawn left to right, its top): wound clockwise, a loop stops what comes
    // from outside. Any other shape: it stops only what comes down onto its
    // top (its up is -y, turned with it), what touches it from below or from
    // the side passing through. Two-sided (the default), it stops all.
    bool one_sided = false;

    static PhysicsShape box(Vec2f half_extents, Vec2f offset = {});
    static PhysicsShape circle(f32 radius, Vec2f offset = {});
    static PhysicsShape capsule(Vec2f half_extents, Vec2f offset = {});
    static PhysicsShape polygon(std::vector<Vec2f> points, Vec2f offset = {});
    static PhysicsShape chain(std::vector<Vec2f> points, bool loop = false, Vec2f offset = {});
};

struct PhysicsFixtureDef {
    PhysicsShape shape{};
    f32 density = 1.0f;
    f32 friction = 0.2f;
    f32 restitution = 0.0f;
    bool sensor = false;
    u16 category_bits = 0x0001;
    u16 mask_bits = 0xFFFF;
    u64 tag = 0;
};

struct PhysicsFilter {
    u16 category_bits = 0xFFFF;
    u16 mask_bits = 0xFFFF;
    bool include_sensors = true;
    PhysicsBody ignore{}; // not found: a body asking what is around it
};

struct PhysicsContactEvent {
    PhysicsContactEventType type = PhysicsContactEventType::Begin;
    PhysicsBody body_a{};
    PhysicsBody body_b{};
    PhysicsFixture fixture_a{};
    PhysicsFixture fixture_b{};
    bool sensor = false;
    bool has_point = false;
    Vec2f point{};
    Vec2f normal{};
};

struct PhysicsContact {
    PhysicsBody body_a{};
    PhysicsBody body_b{};
    PhysicsFixture fixture_a{};
    PhysicsFixture fixture_b{};
    bool sensor = false;
};

struct PhysicsRaycastHit {
    PhysicsBody body{};
    PhysicsFixture fixture{};
    Vec2f point{};
    Vec2f normal{};
    f32 fraction = 0.0f;
};

// What a moving shape ran into (PhysicsWorld::cast_shape, move_character).
struct PhysicsShapeHit {
    PhysicsBody body{};
    PhysicsFixture fixture{};
    Vec2f point{};   // where they touch, on what was hit
    Vec2f normal{};  // out of what was hit, towards the shape (length 1)
    f32 fraction = 0.0f; // how much of its way the shape went before touching it
};

// A character moved by its game (PhysicsWorld::move_character): it goes where
// it is told, as far as it can, and slides along what stops it, rather than
// being pushed about by the simulation.
struct PhysicsCharacterDef {
    // Its shape, from where it is: a box (its corners a little round, so it
    // slides over seams), a circle or a capsule; a polygon is taken as its
    // outline's hull. A chain moves nothing.
    PhysicsShape shape = PhysicsShape::box({0.5f, 0.5f});
    Vec2f up{0.0f, -1.0f}; // on screen (y down), up is -y
    // A surface it touches is a floor when it is at most this steep (radians,
    // from level), a ceiling when it is that close to facing down, else a wall.
    f32 max_slope = 0.7853982f;
    // On a floor when the move starts and not going up, it is kept on it when
    // the floor is at most this far below (units): it walks down slopes and
    // steps rather than off them. 0: it goes off them, falling.
    f32 snap = 0.0f;
    // Moving straight down onto a sloped floor (only falling, or standing),
    // it stops there rather than sliding down the slope.
    bool stop_on_slope = true;
    // One-sided shapes (PhysicsShape::one_sided) stop it when it comes down
    // onto them; with this off they let it through (to drop down off one).
    bool one_way = true;
    // A dynamic body it walks into is pushed on at its speed times this (a
    // crate it shoves). 0: such bodies stop it as walls do.
    f32 push = 1.0f;
    // How close it stays to what it touches (units); 0: 0.02 metres (Box2D's
    // contacts begin closer than that, so they leave it alone).
    f32 skin = 0.0f;
    i32 max_slides = 4; // surfaces it slides along in one move, at most
    // What stops it (sensors never do): set `ignore` to its own body.
    PhysicsFilter filter{};
};

// Where a move took a character, and what it found there.
struct PhysicsCharacterMove {
    Vec2f position{};
    // The velocity it was given, less what went into what stopped it (landing
    // stops its fall, a ceiling its rise, a wall what went into the wall).
    Vec2f velocity{};
    bool on_floor = false;
    bool on_wall = false;
    bool on_ceiling = false;
    Vec2f floor_normal{};
    Vec2f wall_normal{};
    PhysicsBody floor{};               // what it stands on
    std::vector<PhysicsShapeHit> hits; // what it ran into, in order
};

// body_a joined to body_b (none: to where it is in the world). Its angle,
// travel and motor are body_a's from body_b (a door's from its frame). Places
// in the world; angles in radians.
struct PhysicsJointDef {
    PhysicsJointType type = PhysicsJointType::Hinge;
    PhysicsBody body_a{};
    PhysicsBody body_b{};
    Vec2f anchor{};       // where they are joined (a distance joint's point on body_a)
    Vec2f other_anchor{}; // a distance joint's point on body_b
    Vec2f axis{1.0f, 0.0f}; // a slider's, a wheel's: what it moves along
    bool collide_connected = false;
    // Limits: a hinge's angles (from where it was made); a slider's or a
    // wheel's travel along its axis; a distance joint's shortest and longest
    // (a rope: 0 to its length).
    bool limit = false;
    f32 lower = 0.0f;
    f32 upper = 0.0f;
    // A motor: a hinge's or a wheel's turning (radians/s, up to a torque), a
    // slider's sliding (units/s, up to a force).
    bool motor = false;
    f32 motor_speed = 0.0f;
    f32 max_motor_force = 0.0f;
    f32 length = 0.0f; // a distance joint's; 0: how far apart its points are when made
    // A spring (a distance joint's, a wheel's, a weld's): how often it swings
    // (Hz; 0: none, it is stiff) and how soon that dies (1: at once).
    f32 frequency = 0.0f;
    f32 damping_ratio = 0.7f;
    u64 user_id = 0;
};

struct PhysicsDebugJoint {
    PhysicsJoint joint{};
    PhysicsJointType type = PhysicsJointType::Hinge;
    Vec2f anchor_a{};
    Vec2f anchor_b{};
};

struct PhysicsDebugShape {
    PhysicsDebugShapeType type = PhysicsDebugShapeType::Polygon;
    PhysicsBody body{};
    PhysicsFixture fixture{};
    PhysicsBodyType body_type = PhysicsBodyType::Static;
    bool sensor = false;
    bool awake = false;
    std::vector<Vec2f> points;
    Vec2f center{};
    f32 radius = 0.0f;
};

class PhysicsWorld {
public:
    explicit PhysicsWorld(PhysicsWorldDef def = {});
    ~PhysicsWorld();

    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;
    PhysicsWorld(PhysicsWorld&&) noexcept;
    PhysicsWorld& operator=(PhysicsWorld&&) noexcept;

    PhysicsBody create_body(const PhysicsBodyDef& def);
    void destroy_body(PhysicsBody body);
    bool is_alive(PhysicsBody body) const;

    // One fixture is one shape, made of as many Box2D shapes as it takes (a
    // capsule's three, a concave polygon's triangles): its contacts begin
    // when the first of them touches and end when the last stops. One that
    // takes none (a polygon with no area, a chain of one point) is made, and
    // touches nothing (shape_parts: 0).
    PhysicsFixture add_fixture(PhysicsBody body, const PhysicsFixtureDef& def);
    void destroy_fixture(PhysicsFixture fixture);
    bool is_alive(PhysicsFixture fixture) const;
    std::vector<PhysicsFixture> fixtures(PhysicsBody body) const;
    std::size_t fixture_count(PhysicsBody body) const; // fixtures(body).size(), with no list made
    PhysicsBody body_of(PhysicsFixture fixture) const; // none when it is gone
    u32 shape_parts(PhysicsFixture fixture) const;

    // A joint goes with either of its bodies. None (invalid) when body_a is
    // gone, or body_b is given and gone, or they are one.
    PhysicsJoint create_joint(const PhysicsJointDef& def);
    void destroy_joint(PhysicsJoint joint);
    bool is_alive(PhysicsJoint joint) const;
    // Its limits, motor, length and spring as `def` says (not its bodies,
    // anchors or axis: those it was made with).
    void update_joint(PhysicsJoint joint, const PhysicsJointDef& def);
    // The force it pulled with over the last step (units, as forces are).
    Vec2f joint_force(PhysicsJoint joint) const;
    // A hinge's angle (radians, from where it was made); a slider's or a
    // wheel's travel along its axis; a distance joint's length now.
    f32 joint_value(PhysicsJoint joint) const;
    u64 joint_user_id(PhysicsJoint joint) const;

    u64 user_id(PhysicsBody body) const;
    u64 fixture_tag(PhysicsFixture fixture) const;

    // Angles in radians.
    void set_transform(PhysicsBody body, Vec2f position, f32 rotation);
    Vec2f position(PhysicsBody body) const;
    f32 rotation(PhysicsBody body) const;

    void set_linear_velocity(PhysicsBody body, Vec2f velocity);
    Vec2f linear_velocity(PhysicsBody body) const;
    void apply_force(PhysicsBody body, Vec2f force);
    void apply_impulse(PhysicsBody body, Vec2f impulse);

    void step(f32 dt, i32 velocity_iterations = 8, i32 position_iterations = 3);

    std::span<const PhysicsContactEvent> contacts() const;
    std::span<const PhysicsContact> current_contacts() const;
    void clear_contacts();

    // How fast the point of the body at `point` (in the world) is moving: its
    // velocity, and its turning's there.
    Vec2f point_velocity(PhysicsBody body, Vec2f point) const;

    std::vector<PhysicsFixture> query_point(Vec2f point, PhysicsFilter filter = {}) const;
    std::vector<PhysicsFixture> query_aabb(Rectf rect, PhysicsFilter filter = {}) const;
    std::vector<PhysicsRaycastHit> raycast(Vec2f from, Vec2f to, PhysicsFilter filter = {}) const;
    // The fixtures `shape` overlaps, placed at `at` (its offset and angle from
    // there). A chain overlaps nothing.
    std::vector<PhysicsFixture> query_shape(const PhysicsShape& shape, Vec2f at, PhysicsFilter filter = {}) const;
    // The first thing `shape` touches going from `from` to `to` without
    // turning; none when it gets there freely. What it touches at the start is
    // a hit only when it goes on into it, so a shape on a floor can be cast
    // along it. One-sided shapes are hit only by what comes down onto them
    // from above. A chain hits nothing.
    std::optional<PhysicsShapeHit> cast_shape(const PhysicsShape& shape, Vec2f from, Vec2f to, PhysicsFilter filter = {}) const;
    // A character at `position` moved by `velocity` for `dt` seconds, plus
    // `carry` (how far what it stands on takes it this step), sliding along
    // what stops it; `on_floor`: it stood on a floor before (for `snap`). Moving
    // bodies are met where they will be as they move this step (a platform
    // carries what stands on it). The character's own body is not moved: put
    // it at the move's position. Not const: what it pushes (`push`) is pushed.
    PhysicsCharacterMove move_character(const PhysicsCharacterDef& def, Vec2f position, Vec2f velocity, f32 dt, bool on_floor = false,
                                        Vec2f carry = {});
    std::vector<PhysicsDebugShape> debug_shapes(PhysicsFilter filter = {}) const;
    std::vector<PhysicsDebugJoint> debug_joints() const;

    // As the def said (above zero; one when it was not).
    f32 units_per_meter() const;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace kin
