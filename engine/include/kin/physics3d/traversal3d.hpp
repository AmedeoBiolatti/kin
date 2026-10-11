#pragma once

// Grab and climb for kin/physics3d (experimental, docs/physics_3d.md "Grab and climb"): a general way for a character to
// hang from the world by its hands, shimmy along it, pull up over a ledge and let go. Nothing here knows about a mountain
// or a game: a surface is any collider, and what makes one grippable, slippery or tiring is data on its body.
//
//     kin::CharacterDesc3D desc;
//     desc.traversal.enabled = true;                        // off: the controller is exactly what it was
//     const kin::Character3D c = world.create_character(desc);
//     world.set_surface_material(ice, {.slip = 1.0f, .drain = 2.0f});
//     ...each fixed step:
//     world.set_character_input(c, {.move = {0, 1}});       // while hanging: x along the surface, y up it
//     world.set_traversal_input(c, {.grab = {true, true}, .reach = to_the_wall});
//     world.step(dt);
//     const kin::TraversalState3D t = world.traversal(c);   // hanging, the grips, the resource
//
// Four layers, each usable alone:
//  1. PROBE. World3D::probe_grips(): from a reach origin along a direction, what a hand could take hold of. A fan of
//     sphere sweeps (shape_cast) and rays (raycast) against the layers asked for; each hit goes through the providers
//     (below), which turn it into a candidate with a quality, a surface frame and a tag. Pure: nothing is held.
//  2. GRIP. One per hand (a small array: a third limb is data, not a rewrite): reaching, held, slipping, released. The
//     anchor is stored in the GRABBED BODY'S OWN SPACE, so a hold on a platform or a crate goes where it goes.
//  3. BODY. The character has a mode: walking, hanging or mantling. Hanging is kinematic: its feet are driven from the
//     anchors with a damped swing (a pure function of state and commands, nothing random). A dynamic body that is
//     held is loaded with the character's weight at the anchor; a kinematic one carries the character.
//  4. RESOURCE. Resource3D: a value that drains at rates by mode, by surface, and comes back after a delay. It is
//     stamina for the grips and also what sprinting spends.
//
// Providers share one interface (probe, attach, move, detach; GripProvider3D below): the generic surface (any collider
// that is grippable, subject to its SurfaceMaterial3D and the layer mask), the ledge (an edge found by a ray down past
// the top, with the capsule's clearance over it) and the authored hold (HoldDesc3D on a body). A rope would be another.
//
// Determinism: a pure function of (state, commands, dt). The state is in World3D::state_hash(), save_state() and
// restore_state() (a restored world repeats the exact trajectory); the number of workers makes no difference to it.

#include <kin/core/math3d.hpp>
#include <kin/core/types.hpp>
#include <kin/physics3d/handles3d.hpp>

#include <array>
#include <span>
#include <vector>

namespace kin {

class World3D;

// How many limbs a character can have grips with (the descriptor says how many it uses: two hands by default).
inline constexpr u32 max_grip_limbs3d = 4;

// Which provider made a grip.
enum class GripKind3D : u8 {
    Surface, // any grippable collider, where the hand met it
    Ledge,   // the top edge of a body, found by a ray down past it, with room above for the capsule when `clear`
    Hold,    // a body marked with HoldDesc3D: its own quality, and a fixed grip point when it says so
    Rope,    // a point of a rope (rope3d.hpp): the anchor is a segment's body and a place along it, as a share of its length
};

enum class GripPhase3D : u8 {
    Free,     // the limb holds nothing
    Reaching, // going for a candidate (the hand takes `reach_seconds` to arrive)
    Held,
    Slipping, // held, but sliding along the surface
    Released, // let go in this step (the state after is Free)
};

enum class TraversalMode3D : u8 {
    Walking,  // the ordinary controller
    Hanging,  // driven from the anchors of the held grips
    Mantling, // a short controlled move over a ledge
};

// Why a grip ended (CharacterEvent3D::reason of a GripReleased event).
enum class GripRelease3D : u8 {
    Input,     // the grab command was let go
    Drop,      // the drop command
    JumpOff,   // the jump-off command
    Exhausted, // the resource ran out
    Slipped,   // slid for too long, or off the surface
    Lost,      // the body is gone
    Mantle,    // the pull-up finished: the hands are free again
};

// Why a pull-up did not start (CharacterEvent3D::reason of a MantleBlocked event).
enum class MantleBlock3D : u8 {
    NoLedge, // no top edge above the hands
    NoRoom,  // there is a ledge but the capsule does not fit over it, or the way there is blocked
    Tired,   // not enough of the resource
};

// --- surfaces --------------------------------------------------------------------------------------------------------

// What a body's surface is to a hand (World3D::set_surface_material; the ECS component of the same name). The default is
// ordinary, grippable, rock.
struct SurfaceMaterial3D {
    // Hands may take hold of it (a hold on a smooth wall is a body of its own with HoldDesc3D). false: no surface grip, and
    // no ledge on it.
    bool grippable = true;
    // 0 to 1: scales the strength of a grip on it. Under TraversalDesc3D::min_grip it cannot be taken at all.
    f32 grip = 1.0f;
    // How fast a grip slides down it, as a share of TraversalDesc3D::slip_speed (0 holds; ice 1 and more).
    f32 slip = 0.0f;
    // Multiplies the resource's drain while a hand holds it (wet rock 2).
    f32 drain = 1.0f;
    // Yours: reported in candidates, grips and events (ice, wet, snow).
    u32 tag = 0;
};

// An authored hold: a body that is grippable by itself (a ledge you can see, a rung, a handle), whatever its surface
// material says. It is still a body in a layer: the probe's mask applies, and a sensor is never gripped.
struct HoldDesc3D {
    // Its own grip quality (a jug 1, a crimp 0.4); times the surface's `grip`.
    f32 quality = 1.0f;
    // Multiplies the surface's drain.
    f32 drain = 1.0f;
    // The hand goes to `anchor` (in the body's space) with `anchor_normal` as the surface there, instead of where it met
    // the shape (a rung's middle). A snapped hold does not shimmy or slip.
    bool snap = false;
    Vec3f anchor{};
    Vec3f anchor_normal{0.0f, 0.0f, 1.0f};
    // The grip may shimmy and slip along the shape like a surface (a long bar). A hold is a point otherwise.
    bool slide = false;
};

// --- the probe --------------------------------------------------------------------------------------------------------

struct GripProbeDesc3D {
    // The reach origin (a shoulder) and where the hand goes, in the world; `direction` any length but zero.
    Vec3f origin{};
    Vec3f direction{0.0f, 0.0f, -1.0f};
    // Which way is up: the ledge search and the surface frame use it. A floor-like surface (normal along `up`) is not a
    // grip (it is something to stand on), unless it is a hold.
    Vec3f up{0.0f, 1.0f, 0.0f};
    // The way the character faces, across `up`: the frame of a ceiling, and how the ledge clearance is measured.
    Vec3f forward{0.0f, 0.0f, -1.0f};
    f32 reach = 0.8f;
    // The sphere that is swept (a hand's tolerance) and how wide the fan of casts around `direction` is (the tangent of
    // its half angle: 0.45 about 24 degrees; 0 casts only along `direction`).
    f32 radius = 0.1f;
    f32 spread = 0.45f;
    // Layers that may be gripped (a layer in the mask), and one body that never is (the character's own). The mask is
    // also what the ledge's clearance check collides with.
    Layer3DMask mask = all_layers3d & ~layer_bit(layers3d::Character) & ~layer_bit(layers3d::Sensor);
    Body3D ignore{};
    // Providers wanted.
    bool surfaces = true;
    bool ledges = true;
    bool holds = true;
    // A candidate under this quality (quality x the surface's grip) is left out.
    f32 min_grip = 0.1f;
    // Ledge: how far above a hit a top edge is looked for, and the capsule that must fit over it (its radius and height;
    // 0 height: no clearance check, every ledge is `clear`).
    f32 ledge_search_height = 0.5f;
    f32 clearance_radius = 0.3f;
    f32 clearance_height = 1.8f;
    // The most candidates returned.
    u32 max_candidates = 8;
};

struct GripCandidate3D {
    Body3D body{};
    u64 user_id = 0;
    // On the surface, in the world, and its normal pointing out of the body toward the hand.
    Vec3f point{};
    Vec3f normal{0.0f, 0.0f, 1.0f};
    // The surface's frame as a character facing it sees it: `right` along it, `surface_up` up it (for a ceiling, the way
    // `forward` points).
    Vec3f right{1.0f, 0.0f, 0.0f};
    Vec3f surface_up{0.0f, 1.0f, 0.0f};
    GripKind3D kind = GripKind3D::Surface;
    // Quality (hold x surface grip, 0 to 1), the score candidates are sorted by (higher first), the surface's tag, drain
    // multiplier and slip.
    f32 quality = 1.0f;
    f32 score = 0.0f;
    u32 surface_tag = 0;
    f32 drain = 1.0f;
    f32 slip = 0.0f;
    // Distance from the probe's origin to `point`.
    f32 distance = 0.0f;
    // A snapped hold, or a ledge: shimmy and slip work on the surface only (see GripState3D::fixed).
    bool fixed = false;
    // Ledge: the edge's top, where the feet would stand (the capsule's bottom) when the hand is at the lip, and whether the
    // capsule fits over it. The body that is stood on may not be the one held (`ledge_body`).
    bool ledge = false;
    Vec3f ledge_top{};
    bool clear = false;
    Body3D ledge_body{};
};

// --- the resource -------------------------------------------------------------------------------------------------------

struct ResourceConfig3D {
    f32 max = 100.0f;
    // Units a second it comes back at, after it was last spent `regen_delay` seconds ago.
    f32 regen_rate = 25.0f;
    f32 regen_delay = 1.0f;
    // After running out it counts as exhausted until it has come back to this share of `max`.
    f32 recover_fraction = 0.15f;
};

// A generic limited quantity with per-mode drain (the caller passes the rate), a delay before it comes back and an
// exhausted state with hysteresis. Plain data: copy it, hash it. Stamina for the grips, and what sprinting spends.
struct Resource3D {
    f32 value = 0.0f;
    f32 max = 0.0f;
    // Seconds left before it comes back.
    f32 delay = 0.0f;
    bool exhausted = false;

    f32 fraction() const { return max > 0.0f ? value / max : 0.0f; }
    // One step of `dt`: `drain_per_second` units a second taken (0: it rests, and comes back after the delay). True in
    // the step it ran out (the rising edge of `exhausted`).
    bool step(const ResourceConfig3D& config, f32 dt, f32 drain_per_second);
    // A one-off cost (a grab, a pull-up). Takes what is there (never below 0), restarts the delay; true when it ran out.
    bool take(const ResourceConfig3D& config, f32 amount);
};

// --- the character ----------------------------------------------------------------------------------------------------

// A limb that can hold: where its shoulder is relative to the feet, in the character's frame (x right, y up along `up`, z
// forward along the way it faces).
struct GripLimbDesc3D {
    Vec3f shoulder{};
};

// CharacterDesc3D::traversal. Everything but `enabled` is read live (set_character_desc changes it).
struct TraversalDesc3D {
    // Off: no grips, no resource, no state in the hash; the controller is what it was before this existed.
    bool enabled = false;
    // Limbs in use (1 to max_grip_limbs3d) and where each shoulder is. The default is two hands.
    u32 limb_count = 2;
    std::array<GripLimbDesc3D, max_grip_limbs3d> limbs{{{{-0.2f, 1.45f, 0.0f}}, {{0.2f, 1.45f, 0.0f}}, {{-0.4f, 1.3f, 0.0f}}, {{0.4f, 1.3f, 0.0f}}}};

    // --- reach
    f32 reach = 0.8f;
    f32 probe_radius = 0.1f;
    f32 probe_spread = 0.45f;
    // Layers a hand can take hold of (the character's own layer and sensors are left out by default; holds are found even
    // in a sensor layer).
    Layer3DMask grip_mask = all_layers3d & ~layer_bit(layers3d::Character) & ~layer_bit(layers3d::Sensor);
    // The hand takes this long to arrive; a grip is `Held` after.
    f32 reach_seconds = 0.15f;
    // A new grip is not taken this near another hand's anchor.
    f32 min_hand_separation = 0.2f;
    // A surface under this quality (grip x surface grip) is not taken.
    f32 min_grip = 0.1f;
    // The ledge: how far above the hand a top edge is looked for (m).
    f32 ledge_search_height = 0.5f;

    // --- hanging
    // From the mean of the held anchors down to the feet; 0: the first shoulder's height + 0.25 (arms bent a little: the
    // other hand can reach a hold 0.55 m above the first).
    f32 hang_drop = 0.0f;
    // Between the capsule and the surface it hangs from (the radius is added).
    f32 wall_gap = 0.04f;
    // The swing: a damped spring about the hang place (Hz, damping ratio), and the furthest it may be from it (m). A
    // grab in mid-air keeps `grab_momentum` of the speed it had as the start of the swing.
    // Across `up` it is a pendulum (0.9 Hz, lightly damped); along `up` the arms are stiff (3 Hz, no overshoot).
    f32 swing_frequency = 0.9f;
    f32 swing_damping = 0.4f;
    f32 swing_vertical_frequency = 3.0f;
    f32 swing_vertical_damping = 1.0f;
    f32 max_swing = 0.9f;
    f32 grab_momentum = 0.4f;
    // The anchors' own speed along the surface (m/s): sideways, and up or down. The command is CharacterInput3D::move
    // (x right, y up) while hanging.
    f32 shimmy_speed = 1.0f;
    f32 climb_speed = 0.6f;
    // Slipping: the speed of a grip with a surface of slip 1 (m/s), and how long it may slip before it lets go.
    f32 slip_speed = 1.2f;
    f32 slip_release_seconds = 1.5f;
    // Jump off: m/s away from the surface and along `up`.
    f32 jump_off_speed = 3.0f;
    f32 jump_up_speed = 4.0f;
    // The pull-up: how long the move over the ledge takes, and how far inside the edge it ends (the capsule's radius is
    // added).
    f32 mantle_seconds = 0.7f;
    f32 mantle_inset = 0.1f;

    // --- the resource
    ResourceConfig3D resource{};
    // Units a second while hanging: from one hand (it takes the whole load), from two (each takes half); extra while moving
    // along the surface; and sprinting on the ground. Times the surface's drain, mean of the held grips'.
    f32 drain_one_hand = 12.0f;
    f32 drain_two_hands = 7.0f;
    f32 drain_moving = 4.0f;
    f32 drain_sprint = 20.0f;
    // Costs: taking hold, and a pull-up.
    f32 grab_cost = 2.0f;
    f32 pull_up_cost = 8.0f;
    // A pull-up needs this much of it (units).
    f32 pull_up_min = 8.0f;
    // Share of the hanging drain that counts while the feet stand on walkable ground (0: standing costs nothing).
    f32 ground_drain_factor = 0.0f;
    // Running out of it: the weakest hand lets go after this long and the next one `exhaust_interval` later, each grip's
    // strength going to 0 over `exhaust_fade` seconds.
    f32 exhaust_interval = 0.4f;
    f32 exhaust_fade = 0.3f;
    // While exhausted a character cannot sprint (the command is treated as released).
    bool sprint_needs_resource = true;
};

// What the player (or an AI, or a recording) asks of the limbs in one step: a command, held until set again.
struct TraversalInput3D {
    // The limb tries to hold when this is true (a level: let go to release; a hand that was made to let go by the
    // character's own state stays free until the command is released and given again).
    std::array<bool, max_grip_limbs3d> grab{};
    // The way the hands go for, in the world (the camera's forward, say); zero: the way the character faces
    // (CharacterInput3D::forward).
    Vec3f reach{};
    // Edges: made when the command goes from false to true.
    bool pull_up = false;
    bool jump_off = false;
    // Lets go of everything (a level: while true no hand holds).
    bool drop = false;
};

// One limb's grip as the last step left it.
struct GripState3D {
    GripPhase3D phase = GripPhase3D::Free;
    GripKind3D kind = GripKind3D::Surface;
    Body3D body{};
    u64 body_user_id = 0;
    // In the held body's own space (what is stored) and in the world now (from it).
    Vec3f local_point{};
    Vec3f local_normal{0.0f, 0.0f, 1.0f};
    Vec3f point{};
    Vec3f normal{0.0f, 0.0f, 1.0f};
    // The surface's frame as above.
    Vec3f right{1.0f, 0.0f, 0.0f};
    Vec3f surface_up{0.0f, 1.0f, 0.0f};
    // 0 to 1: quality x surface grip, faded by exhaustion.
    f32 strength = 0.0f;
    // Reaching: 0 to 1 of the way there.
    f32 reach_progress = 0.0f;
    // Slipping: the speed it slides at (m/s) and how long it has (s).
    f32 slip_speed = 0.0f;
    f32 slip_time = 0.0f;
    u32 surface_tag = 0;
    f32 drain = 1.0f;
    // It does not shimmy or slip (a snapped hold, a ledge's lip).
    bool fixed = false;
    // On a ledge: a pull-up from here goes over its top.
    bool ledge = false;
    f32 quality = 1.0f;
};

// The traversal of one character as the last step left it.
struct TraversalState3D {
    bool enabled = false;
    TraversalMode3D mode = TraversalMode3D::Walking;
    u32 limb_count = 0;
    std::array<GripState3D, max_grip_limbs3d> grips{};
    // Limbs holding (held or slipping).
    u32 held = 0;
    // The pull-up: 0 to 1 of the way over.
    f32 mantle_progress = 0.0f;
    // The swing's offset from the hang place (world), the hang place itself, and seconds spent hanging.
    Vec3f swing{};
    Vec3f hang_place{};
    f32 hang_time = 0.0f;
    // The mean of the held anchors (world) and the surface normal they average to; zero with no grip.
    Vec3f anchor_centre{};
    Vec3f surface_normal{};
    Resource3D resource{};

    bool hanging() const { return mode != TraversalMode3D::Walking; }
};

// --- providers ---------------------------------------------------------------------------------------------------------

// What a probe's casts found before any provider looked at it.
struct RawGripHit3D {
    Body3D body{};
    u64 user_id = 0;
    Vec3f point{};
    Vec3f normal{};
    // Set by the provider that made a candidate of it (providers run in priority order: hold, ledge, surface), so that
    // one hit is one candidate.
    bool claimed = false;
};

// A grip that is attached: where it is on its body (body space), and what the surface is. What attach() makes and move()
// changes.
struct GripAnchor3D {
    Body3D body{};
    GripKind3D kind = GripKind3D::Surface;
    Vec3f local_point{};
    Vec3f local_normal{0.0f, 0.0f, 1.0f};
    f32 quality = 1.0f;
    u32 surface_tag = 0;
    f32 drain = 1.0f;
    f32 slip = 0.0f;
    bool fixed = false;
    bool ledge = false;
};

// The one interface the generic surface, the ledge and the authored hold implement (and a rope would).
class GripProvider3D {
public:
    virtual ~GripProvider3D() = default;
    virtual GripKind3D kind() const = 0;
    // Looks at the casts' hits (nearest first, one a body and place) and adds the candidates it makes. A provider that
    // does its own queries (a rope) may ignore `hits`.
    virtual void probe(const World3D& world, const GripProbeDesc3D& probe, std::span<RawGripHit3D> hits,
                       std::vector<GripCandidate3D>& out) const = 0;
    // The anchor of a candidate it made, in the body's own space.
    virtual GripAnchor3D attach(const World3D& world, const GripCandidate3D& candidate) const = 0;
    // Moves a held anchor by `delta` (world, in the surface's plane): re-projects it onto the surface there. False: it
    // would leave the surface (an edge, a gap) or the provider does not let it move; the anchor is left as it was.
    virtual bool move(const World3D& world, GripAnchor3D& anchor, Vec3f delta, Vec3f up, Vec3f forward) const = 0;
    // The grip is let go of (a rope would unspool here); nothing for the built-in ones.
    virtual void detach(const World3D& world, const GripAnchor3D& anchor) const { (void)world; (void)anchor; }
};

// The built-in provider of a kind (Rope-like providers would be registered later).
const GripProvider3D& grip_provider(GripKind3D kind);

// The surface frame a character facing a surface of `normal` sees: right along it, up it. A ceiling or a floor (the normal
// along `up`) has no "up it": it is `forward` (across `up`). Unit vectors.
struct SurfaceFrame3D {
    Vec3f right{1.0f, 0.0f, 0.0f};
    Vec3f up{0.0f, 1.0f, 0.0f};
};
SurfaceFrame3D surface_frame(Vec3f normal, Vec3f up, Vec3f forward);

} // namespace kin
