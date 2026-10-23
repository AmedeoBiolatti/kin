#pragma once

// A capsule character controller for kin/physics3d (experimental, docs/physics_3d.md "Character controller"): Jolt's
// CharacterVirtual behind kin's own types (no Jolt header here). The controller is made and driven through a World3D
// (World3D::create_character, set_character_input, character, ...); this header is the data.
//
//     kin::CharacterDesc3D desc;
//     desc.position = {0, 1, 0};                       // the feet
//     const kin::Character3D c = world.create_character(desc);
//     ...each fixed step:
//     world.set_character_input(c, {.move = {0, 1}, .forward = {0, 0, -1}, .jump = pressed});
//     world.step(dt);                                   // advances the world and every character
//     const kin::CharacterState3D s = world.character(c);
//
// What it is:
//  * A pure function of (its state, the input command, dt). No clock, no random numbers; the same bits with no workers,
//    1 or 4 (tested), and covered by World3D::state_hash(), save_state() and restore_state() (a restored world repeats
//    the exact trajectory). The input is a command, never device state: a few numbers a netcode layer can send.
//  * Stepped by World3D::step(): the characters update BEFORE the world's step, in ascending handle index (Jolt's
//    own recommendation: a character is moved by a swept test against the world as the last step left it, kinematic
//    platforms already carry the velocity this step will give them, and what the character pushes gets its impulse
//    before the solver runs). Then the world steps.
//  * Gravity is the character's own: its "up" is a vector (by default against the world's gravity, following
//    set_gravity; or set explicitly: a wall, a ceiling, a planet), the force |world gravity| x gravity_scale along
//    -up. The up vector turns smoothly (CharacterDesc3D::up_blend_rate), so walking onto a wall is a turn, not a jump.
//  * The feet are the origin: position() is the point under the middle of the capsule's base.
//  * Pushed from outside (World3D::add_character_impulse, set_character_velocity): what was given since the last step goes
//    into the velocity the next step starts from, before the command is applied (a velocity first, replacing, then the
//    impulses / mass), and is cleared. From there it is the controller's own speed and the command works on it as on any:
//    on the ground the part across `up` is braked toward the wish at acceleration or deceleration x ground_friction (a
//    shove slides and stops, far on ice); in the air it is kept (air_acceleration steers it toward the wish) and gravity
//    acts along -up. A push toward the ground it stands on is held by it; one that sends it away from the ground at 0.1 m/s
//    or more launches it (it leaves the ground as after a jump: no stick to the floor, no coyote time, no jump of its own
//    that step). Hanging (grab and climb), it is the swing's (the grips' spring brings it back) and what it falls with if
//    it lets go; mantling, it is dropped.
//  * It has a body in the world (a kinematic capsule in its layer, never asleep) so that the rest of the physics sees
//    it: dynamic bodies are stopped by it, sensors report it, queries hit it (ignore it with `body`). It counts in
//    World3D::body_count() and is in bodies(); do not destroy, move or reshape it through the body calls.
//
// Grab and climb is on top of it (traversal3d.hpp, off by default: CharacterDesc3D::traversal.enabled): two hands take hold
// of the world, the character hangs from them (kinematic, with a swing), shimmies, pulls up over a ledge or lets go.
//
// Not here: ragdolls, ropes (later), character against character pushing (they stop each other).

#include <kin/core/math3d.hpp>
#include <kin/core/types.hpp>
#include <kin/physics3d/handles3d.hpp>
#include <kin/physics3d/traversal3d.hpp>

namespace kin {

// A character in a World3D: an index and a generation, as Body3D is.
struct Character3D {
    u32 index = UINT32_MAX;
    u32 generation = 0;

    bool valid() const { return index != UINT32_MAX; }
    explicit operator bool() const { return valid(); }
    friend constexpr bool operator==(Character3D, Character3D) = default;
};

struct CharacterDesc3D {
    // Where the feet start.
    Vec3f position{};
    // Which way is up for this character: a vector (any length). Zero: against the world's gravity, always (and
    // so following World3D::set_gravity); set_character_up later makes it explicit.
    Vec3f up{};

    // The capsule: `height` is feet to top while standing (at least 2 x radius), `crouch_height` crouching.
    f32 radius = 0.3f;
    f32 height = 1.8f;
    f32 crouch_height = 1.0f;
    // Pushes what is under it and sets how hard a heavy body is to push: it can push with `push_strength`
    // (newtons, as Jolt's max strength). A box that needs more than this does not move and blocks it.
    f32 mass = 70.0f;
    f32 push_strength = 150.0f;

    // Ground it can stand on: surfaces at most this steep (degrees from flat, measured from `up`). Steeper ones
    // are slid down.
    f32 max_slope_degrees = 45.0f;
    // Stairs: it steps up what is at most this high (0 turns stairs off).
    f32 max_step_height = 0.4f;
    // Stick to the ground: leaving the ground over a hump or down a ramp, it is put back on it when it is within
    // this distance (0 turns it off; the character then hops).
    f32 step_down_distance = 0.5f;

    // Speeds: what a full input makes it do (m/s relative to the ground it stands on). sprint_speed is the largest it
    // gives by itself: ground_speed / sprint_speed is 0 to 1 for an animation blend.
    f32 walk_speed = 4.0f;
    f32 sprint_speed = 7.0f;
    f32 crouch_speed = 2.0f;
    // How fast it gets there (m/s^2) on the ground, how fast it slows down with no input, and in the air (0: none).
    f32 acceleration = 40.0f;
    f32 deceleration = 40.0f;
    f32 air_acceleration = 8.0f;
    // Friction. Ground: scales acceleration and deceleration on the ground (1 normal, 0.05 ice). Wall: Coulomb friction
    // against steep surfaces it is pressed to (0 slides along a wall without losing speed, 1 holds a falling character
    // against it).
    f32 ground_friction = 1.0f;
    f32 wall_friction = 0.0f;
    // Gravity along -up is |world gravity| x this.
    f32 gravity_scale = 1.0f;
    // The speed up that a jump gives: the apex is jump_speed^2 / (2 g) above the take-off (5 m/s: 1.27 m at 9.81).
    f32 jump_speed = 5.0f;
    // Options, off by default. Coyote time: a jump is still allowed this many seconds after walking off an edge.
    // Jump buffer: a jump pressed this long before landing happens on landing.
    f32 coyote_seconds = 0.0f;
    f32 jump_buffer_seconds = 0.0f;

    // How fast the up vector turns to its target: the fraction of the remaining turn made in a second (a time
    // constant of 1 / rate). 0 turns at once.
    f32 up_blend_rate = 8.0f;

    // The layer of its body (layers3d::Character) and what it collides with: a layer in the mask is hit, others are
    // passed through (also by the character's own movement; other bodies still obey the world's layer table).
    u8 layer = layers3d::Character;
    Layer3DMask collides_with = all_layers3d;

    // Grab and climb (traversal3d.hpp): off by default, which leaves everything above as it was.
    TraversalDesc3D traversal{};

    // Yours (not read by the engine; hashed, and the user id of its body: events name it).
    u64 user_id = 0;
};

// What the player (or an AI, or a recording) asks for in one step: a command. Held for every step until set again.
struct CharacterInput3D {
    // In the character's or the camera's frame: x to the right, y forward, each -1 to 1 (longer than 1 is cut to 1).
    Vec2f move{};
    // Which way "forward" is, in the world (the camera's or the character's facing); its part across `up` is used.
    // Moving "forward" with this along the up vector moves nothing.
    Vec3f forward{0.0f, 0.0f, -1.0f};
    // Level, not edge: a jump is made when it goes from false to true (and the character may jump).
    bool jump = false;
    bool crouch = false;
    bool sprint = false;
};

enum class CharacterGround3D : u8 {
    OnGround,    // standing on something it can stand on
    OnSteepSlope, // on a slope above max_slope_degrees but not moving down it much (wedged)
    Sliding,     // on a slope above max_slope_degrees and sliding down it
    InAir,       // falling, jumping, or touching something that does not hold it
};

// What the controller is, as the last step left it.
struct CharacterState3D {
    // The feet.
    Vec3f position{};
    // What the last step achieved (the distance it moved over dt; the ground's own motion included). A step up a stair
    // shows as a short vertical spike.
    Vec3f velocity{};
    // The up vector it is using now (turning toward its target).
    Vec3f up{0.0f, 1.0f, 0.0f};
    CharacterGround3D ground = CharacterGround3D::InAir;
    // The ground it stands on (or slides on): the body, its user id, the surface normal, the velocity of that point.
    // Invalid / zero in the air.
    Body3D ground_body{};
    u64 ground_user_id = 0;
    Vec3f ground_normal{0.0f, 1.0f, 0.0f};
    Vec3f ground_velocity{};
    // Speed over the ground along it (m/s, up part left out, the ground's own motion taken off): what an animation
    // blend wants. 0 in the air it is the horizontal speed.
    f32 ground_speed = 0.0f;
    // Rate of going up (+) or down (-) along `up` (m/s, the ground's included).
    f32 vertical_speed = 0.0f;
    bool crouching = false;
    // Crouch is wanted and the head would hit something: it stays crouched.
    bool stand_blocked = false;
    // Seconds since it was last on the ground (0 on it).
    f32 air_time = 0.0f;
    // The current standing / crouching height, the capsule's radius.
    f32 height = 0.0f;
    // Its body in the world (a kinematic capsule).
    Body3D body{};
    // Jumped in the last step.
    bool jumped = false;
    // Grab and climb: what the body is doing (Walking when traversal is off). While it is not Walking, `ground` is only
    // what the capsule touches (no Landed or LeftGround events come of it) and the walking fields above are the achieved
    // motion. World3D::traversal() has the grips and the resource.
    TraversalMode3D traversal_mode = TraversalMode3D::Walking;
};

// One thing the character touches (as the last step left it): sorted by body number, a body once.
struct CharacterContact3D {
    Body3D body{};
    u64 user_id = 0;
    // Where it touches and the surface's normal there, pointing out of the body toward the character.
    Vec3f point{};
    Vec3f normal{};
    // The body is a sensor (a trigger volume): it does not stop the character.
    bool sensor = false;
    // The surface is one it can stand on (not steeper than max_slope_degrees).
    bool walkable = false;
};

enum class CharacterEventKind3D : u8 {
    Landed,      // came onto the ground; `speed` is how fast it was falling
    LeftGround,  // left the ground (jumped or walked off); `speed` its speed up (+) or down (-) as it left
    HitHead,     // moving up, met a ceiling; `speed` how fast it was going up
    ContactBegin, // began to touch `body` (a sensor: entered it)
    ContactEnd,   // stopped touching `body`
    // Grab and climb (traversal3d.hpp). `hand` is the limb's number; `body` and `body_user_id` what it holds; `point` and
    // `normal` the anchor and its surface.
    Gripped,       // a hand took hold (`jumped` is not used)
    Slipped,       // a held grip began to slide (`speed` how fast)
    GripReleased,  // a hand let go; `reason` is a GripRelease3D
    Exhausted,     // the resource ran out (`speed` is the value it had a step before)
    MantleStarted, // the pull-up began; `point` is where the feet will stand
    MantleFinished,// the pull-up is done: walking again
    MantleBlocked, // a pull-up was asked for and could not be made; `reason` is a MantleBlock3D; still hanging
    Fell,          // the character let go of everything and is falling (not a jump off or a pull-up); `reason` as GripReleased
};

struct CharacterEvent3D {
    Character3D character{};
    u64 user_id = 0; // the character's
    CharacterEventKind3D kind = CharacterEventKind3D::Landed;
    // ContactBegin / ContactEnd: the body and its user id. Landed: the ground. HitHead: the ceiling.
    Body3D body{};
    u64 body_user_id = 0;
    bool sensor = false;
    Vec3f point{};
    Vec3f normal{};
    f32 speed = 0.0f;
    // LeftGround: it was the jump command that did it (and not an edge).
    bool jumped = false;
    // The grab and climb kinds: the limb, and why (see the kind).
    u8 hand = 0;
    u8 reason = 0;
};

} // namespace kin
