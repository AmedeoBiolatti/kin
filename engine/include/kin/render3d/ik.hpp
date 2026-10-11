#pragma once

// Inverse kinematics for kin's experimental 3D (docs/3d_plan.md, P1b): solvers that bend a posed skeleton so that a hand
// reaches a hold, a foot stands on the ground, a head looks at something. Plain data and maths on the CPU, no GPU.
//
// Every solver takes the skeleton, the LOCAL pose (what the clips produced) and the joints' MODEL-SPACE world matrices of that
// pose (compute_world_matrices: the entity's own transform is not in them), changes the pose's local rotations (and, for a
// stretch or a pelvis, local translations) and keeps `world` in step, recomputing only the joints below the ones it moved. So
// solvers chain: each sees what the one before it did, and the matrices the shader wants are one multiplication a joint away
// (world_to_joint_matrices). Targets are points of the model's space; the ECS hook (Animator3D::ik, kin/ecs/render3d.hpp)
// converts world-space targets with the entity's matrix.
//
//     kin::Pose pose = player.pose();
//     std::vector<kin::Mat4f> world;
//     kin::compute_world_matrices(skeleton, pose, world);
//     kin::TwoBoneOptions arm;
//     arm.pole_mode = kin::PoleMode::Direction;
//     arm.pole = {0, 0, -1};                                  // the elbow points back
//     kin::solve_two_bone(skeleton, pose, world, {shoulder, elbow, hand}, hold_position, arm);
//     kin::world_to_joint_matrices(skeleton, world, matrices); // for draw_skinned
//
// Nothing here is a hidden state: the pose is the input, so solving it again gives the same pose (idempotence), and a weight
// of zero returns it bit for bit. The two stateful helpers (look-at smoothing, foot placement's pelvis) take a state object
// that the caller keeps between frames.

#include <kin/core/math3d.hpp>
#include <kin/core/types.hpp>
#include <kin/render3d/skeleton.hpp>

#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace kin {

// --- matrices kept in step with the pose ------------------------------------------------------------------------------

// Recomputes the world matrix of `joint` and of every joint below it from `pose` (a parent before its children), after a
// local TRS of `joint` or of one of its ancestors changed. `world` holds joint_count() matrices.
void update_world_below(const Skeleton& skeleton, const Pose& pose, std::span<Mat4f> world, i32 joint);

// The matrices the vertex shader applies, from the world matrices: world * inverse bind, a joint at a time (what
// compute_joint_matrices makes, without walking the hierarchy again). `out` may be `world` itself.
void world_to_joint_matrices(const Skeleton& skeleton, std::span<const Mat4f> world, std::span<Mat4f> out);

// The rotation of a matrix's linear part, with scale and shear taken out (Gram-Schmidt on its columns: exact for R * S).
Quatf rotation_of(const Mat4f& m);

// The shortest rotation that turns the direction `from` onto `to` (any lengths but zero). Exactly opposite directions give a
// half turn about some axis perpendicular to `from` (chosen the same way each time).
Quatf rotation_between(Vec3f from, Vec3f to);

// --- two bones: an arm to a hold, a leg to the ground ----------------------------------------------------------------

// Three joints of one chain: `root` (shoulder, hip), `mid` (elbow, knee) and `end` (hand, foot). `mid` is below `root` and
// `end` below `mid`; joints between them (twist joints) are carried along and keep their local pose.
struct TwoBoneChain {
    i32 root = -1;
    i32 mid = -1;
    i32 end = -1;
};

enum class PoleMode : u8 {
    None,      // keep the side the elbow is on in the pose it is given
    Position,  // a point of the model's space the elbow points to (seen from the root)
    Direction, // a direction in the model's space the elbow points along
};

struct TwoBoneOptions {
    // 0 leaves the pose as it is, bit for bit; 1 reaches; between, the rotations are blended (a hand half way).
    f32 weight = 1.0f;
    PoleMode pole_mode = PoleMode::None;
    Vec3f pole{};
    // How much longer than itself the chain may get to reach: 1 never stretches; 1.2 up to 20% (a soft clamp: the stretch
    // eases in as the target passes the full reach, and tends to the limit and never beyond).
    f32 max_stretch = 1.0f;
    // The least the mid joint stays bent from a straight chain, in degrees: a knee that never locks. A target past that
    // reach is reached as nearly as the chain can (the end on the line to the target).
    f32 min_bend_degrees = 0.0f;
    // Also turn the end joint (a hand to a hold's surface, a foot to a slope): its model-space orientation, blended in by
    // `rotation_weight` on top of `weight`.
    bool match_rotation = false;
    Quatf end_rotation{};
    f32 rotation_weight = 1.0f;

    // --- joint limits of the mid joint and the twist of the root (all off by default: the pose is then what it always was).
    //
    // The most the mid joint bends from a straight chain, in degrees (a knee that folds to 150 and no further; 180: no limit).
    // A target nearer than the chain can then fold to is aimed at through the point its own axis adds to it, as with unequal
    // bones (a hole round the root), so the end stands at the chord of the fold, toward the target, and the pose is continuous
    // in the target. `reached` is false there.
    f32 max_bend_degrees = 180.0f;
    // The mid joint is a hinge about `hinge_axis` (in ITS OWN local space: a knee's X): it only bends one way about it, the
    // positive way (the rotation of the upper bone's direction onto the lower's, about the axis). A pose that is straight, or
    // bent the wrong way by a clip, takes its plane from the hinge, so that a knee is never driven backwards by a pole
    // behind it: the root turns to carry the hinge round to the pole instead (see `max_twist_degrees`).
    bool hinge = false;
    Vec3f hinge_axis{1.0f, 0.0f, 0.0f};
    // The most the root joint turns about its own bone from what the clips did (the swing-twist decomposition of the root's
    // turn about the bone's axis), in degrees; 180: no limit. Past it the elbow's side falls short of the pole's (the end
    // still reaches the target), and eases back toward the clips' side as the pole goes round behind, so that nothing flips.
    // Needs a plane to measure from: a `hinge`, or a pose that is bent (with a pose that is nearly straight and no hinge the
    // limit fades with how bent it is).
    f32 max_twist_degrees = 180.0f;
    // With a TwoBoneState (and a dt): the elbow's side swings toward the pole's at no more than this many degrees a second
    // (0: at once). This is the one treatment that makes the side continuous in TIME where a pole passes through the target's
    // axis (see TwoBoneState).
    f32 bend_swing_degrees_per_second = 0.0f;
};

// What to keep between frames for `bend_swing_degrees_per_second`: the side the elbow was on, as a direction in the space of
// the root joint's parent. The caller keeps it; the first call (valid false) starts from the side the solver wants.
struct TwoBoneState {
    Vec3f bend{};
    f32 sense = 1.0f; // which way round the target's axis the last swing went (used when the two sides are exactly opposite)
    bool valid = false;
};

struct TwoBoneResult {
    bool solved = false;   // false: nothing was done (weight 0, a bad chain, a zero-length bone)
    bool reached = false;  // the end joint is at the target (to rounding) at weight 1
    f32 stretch = 1.0f;    // the factor the bones were lengthened by
    f32 bend_degrees = 0.0f; // how far the mid joint is bent from straight in the solution
};

// Rotates the root and mid joints (local rotations; the pose's local translations and scales are kept, bar a stretch) so
// that the end joint reaches `target`, or stands as near as the chain can on the line to it. Solved in the space of the
// root joint's parent, so a scaled or turned model, or a non-uniform scale above the chain, is exact. Degenerate input
// (target at the root, on the chain's axis, a pole on the axis, a straight chain) falls back to the bend plane of the pose
// given and varies continuously with the target. The side a straight chain with no pole bends to is the rest pose's bend
// (what the rig's author built), else the hinge's, else -Y of the root's parent.
//
// A non-uniform scale on the root joint itself (or on a joint between the root and the mid joint) makes the forearm's drawn
// length depend on how it is turned, which no rotation of rigid bones can follow: the solve is repeated from its own result
// (at most 8 times) until the end is on its goal, which it is to 1e-5 of the reach for scales within a factor of about 2.
// `state` and `dt` are for `bend_swing_degrees_per_second`; both may be left out.
TwoBoneResult solve_two_bone(const Skeleton& skeleton, Pose& pose, std::span<Mat4f> world, const TwoBoneChain& chain, Vec3f target,
                             const TwoBoneOptions& options = {}, TwoBoneState* state = nullptr, f32 dt = 0.0f);

// --- look at: a head, or a spine to a neck to a head ------------------------------------------------------------------

// One joint of a look-at chain, root to tip. The joints' `weight`s say how the turn is shared (a spine 0.2, a neck 0.3, a
// head 0.5: normalised here). The limits are on the turn this joint contributes, seen from the aimer: its yaw (about its up
// axis), pitch up and pitch down, in degrees; the default is no limit.
struct LookAtJoint {
    i32 joint = -1;
    f32 weight = 1.0f;
    f32 max_yaw_degrees = 180.0f;
    f32 max_pitch_up_degrees = 180.0f;
    f32 max_pitch_down_degrees = 180.0f;
};

struct LookAtOptions {
    // In the last joint's local space: the axis that is to point at the target (a glTF character faces +Z), the axis that
    // is up for the yaw and pitch limits, and the point of the head the sight goes from (the eyes).
    Vec3f aim_axis{0.0f, 0.0f, 1.0f};
    Vec3f up_axis{0.0f, 1.0f, 0.0f};
    Vec3f eye_offset{};
    f32 weight = 1.0f; // 0: untouched bit for bit; the turn is scaled by it
    // The goal direction is followed, not jumped to: an exponential approach with this time constant (0: at once) and at
    // most this speed in degrees a second (0: any). Needs a LookAtState and a dt.
    f32 smooth_seconds = 0.0f;
    f32 max_speed_degrees = 0.0f;
    // A target further round than a joint's limit pulls the head back toward forward over this many degrees instead of
    // holding it at the limit, so that a target passing behind the head does not make it flip sides.
    f32 release_degrees = 30.0f;
};

// What to keep between frames: the smoothed direction the head is following, in the space the target is given in.
struct LookAtState {
    Vec3f direction{};
    bool valid = false;
};

struct LookAtResult {
    bool solved = false;
    f32 goal_degrees = 0.0f;    // how far the aim had to turn to face the (smoothed) target
    f32 applied_degrees = 0.0f; // how far it did turn, after limits and weight
};

// Turns the chain so that the last joint's `aim_axis` points at `target` (its `eye_offset` seen from there), the turn shared by
// the weights, limited per joint, scaled by the weight and smoothed by `state` over `dt`. The shortest turn is used, so
// nothing twists. `state` may be null (no smoothing).
LookAtResult solve_look_at(const Skeleton& skeleton, Pose& pose, std::span<Mat4f> world, std::span<const LookAtJoint> chain, Vec3f target,
                           const LookAtOptions& options = {}, LookAtState* state = nullptr, f32 dt = 0.0f);

// --- FABRIK: tails, spines, a rope held by a hand ---------------------------------------------------------------------

struct FabrikOptions {
    f32 weight = 1.0f;
    u32 max_iterations = 16;
    f32 tolerance = 1e-4f;   // in the model's units: the tip is this near the target
    bool fix_root = true;    // the first joint stays where it is; false lets the chain's start follow the target
    // A pole: the solved chain is turned about the line from its first joint to its tip (which moves neither end nor any
    // length) so that the chain's bulge points toward it, as a two-bone's elbow does. A straight chain has no bulge and a pole
    // on the line says nothing: both fade out smoothly. Direction: a model-space direction; Position: a point of the model's
    // space seen from the first joint.
    PoleMode pole_mode = PoleMode::None;
    Vec3f pole{};
};

struct FabrikResult {
    bool solved = false;
    bool reached = false;
    u32 iterations = 0;
    f32 error = 0.0f; // the tip's distance from the target
};

// Forward-and-backward reaching IK for any number of joints. `chain` lists joints root to tip, each below the one before it;
// bone lengths are those of the pose and are kept. Each joint but the last is turned (the shortest turn) onto the new
// direction of its bone; the tip keeps its local rotation.
FabrikResult solve_chain_fabrik(const Skeleton& skeleton, Pose& pose, std::span<Mat4f> world, std::span<const i32> chain, Vec3f target,
                                const FabrikOptions& options = {});

// --- foot placement ---------------------------------------------------------------------------------------------------

struct GroundHit {
    Vec3f position{};
    Vec3f normal{0.0f, 1.0f, 0.0f};
};
// Where the ground is along the segment from `from` to `to` (the first hit), in whatever space the planner works in. P2's
// physics raycast is one of these.
using GroundQuery = std::function<std::optional<GroundHit>(Vec3f from, Vec3f to)>;

struct FootLeg {
    TwoBoneChain leg;      // hip, knee, ankle
    f32 foot_height = 0.1f; // how far the ankle is above the sole of the foot
    // The toe (the ball of the foot: a child of the ankle), or -1. With it, a second ground query is made for the leg, from
    // the animated toe, and the toe turns to that ground's normal (see FootPlacementOptions::toe_max_tilt_degrees).
    i32 toe = -1;
    f32 toe_height = 0.03f; // how far the toe joint (the ball) is above the sole
};

struct FootPlacementOptions {
    Vec3f up{0.0f, 1.0f, 0.0f};
    // The height (along `up`) of the floor the clips were made for; a hit above it lifts the foot by the difference.
    f32 ground_level = 0.0f;
    // The ray runs from `ray_up` above the animated ankle to `ray_down` below the animated sole.
    f32 ray_up = 0.5f;
    f32 ray_down = 0.6f;
    // How far the pelvis may go down (the lowest foot's drop) and up (all feet on higher ground; 0: never).
    f32 max_pelvis_drop = 0.5f;
    f32 max_pelvis_raise = 0.0f;
    // The slope the foot follows is clamped to this many degrees from `up`, and not followed at all by a foot in the air
    // (more than `tilt_blend_height` of sole above the ground), easing between.
    f32 max_tilt_degrees = 30.0f;
    f32 tilt_blend_height = 0.15f;
    // The pelvis eases to its offset, and a foot eases down onto ground that fell away (it rises at once: no sinking into
    // a step), with this time constant. 0: no smoothing.
    f32 smooth_seconds = 0.1f;

    // --- toes (a FootLeg with a toe joint; off with none)
    // The toe turns to the normal of the ground under it (clamped to the same slope as the foot's, then to this many degrees
    // from its animated turn), only while the foot is on the ground, easing out over `tilt_blend_height` as the foot's tilt does.
    f32 toe_max_tilt_degrees = 40.0f;

    // --- foot locking (needs a FootPlacementState; off by default: a foot is then planned from the animated ankle each frame)
    // A foot whose animated sole is within `lock_height` of the ground is planted: its ankle target is held at the place it
    // was when it planted (in the world: the caller's `model_to_world`), while the body moves over it. It lets go when the
    // animated foot rises past `lock_release_height`, or the animated ankle is further than `lock_release_distance` from the
    // held place (the animation has moved on: the leg would be pulled out of its reach). It then eases back to the animated
    // ankle over `lock_replant_seconds` (a smoothstep), not at once.
    bool lock_feet = false;
    f32 lock_height = 0.02f;
    f32 lock_release_height = 0.06f;
    f32 lock_release_distance = 0.15f;
    f32 lock_replant_seconds = 0.15f;
};

// Kept between frames; the first call (valid false) starts from the plan itself.
struct FootPlacementState {
    f32 pelvis_offset = 0.0f;
    std::vector<f32> foot_offset;
    bool valid = false;
    // The foot locks, one a leg (see FootPlacementOptions::lock_feet). Positions are in the space `model_to_world` leads to.
    struct Lock {
        bool planted = false;
        f32 blend = 0.0f;       // 1 held at `point`, falling to 0 over the re-plant
        Vec3f point{};          // the held ankle target (world)
        Vec3f normal{0.0f, 1.0f, 0.0f};
        Quatf rotation{};       // the held foot orientation (world)
        f32 rise = 0.0f;        // the ground's height there above `ground_level`, for the pelvis
    };
    std::vector<Lock> locks;
};

struct FootTarget {
    bool grounded = false;     // the ray hit; otherwise `target` is the animated ankle
    Vec3f target{};            // where the ankle goes (model space)
    Quatf rotation{};          // the end orientation to give the two-bone solver (model space)
    f32 rotation_weight = 0.0f;
    Vec3f normal{0.0f, 1.0f, 0.0f}; // the (clamped) normal the foot follows
    bool locked = false;       // the target is the held place of a planted foot (or easing back from it)
    // The toe (a leg with a toe joint, the foot on the ground, and ground under the toe): the orientation to give it (model
    // space), and how much of the way (the foot's tilt blend).
    bool toe_grounded = false;
    Quatf toe_rotation{};
    f32 toe_weight = 0.0f;
    f32 toe_max_degrees = 0.0f; // how far from its animated turn it may go
};

struct FootPlan {
    std::vector<FootTarget> feet;  // one a leg
    f32 pelvis_offset = 0.0f;      // along `up`: negative lowers
};

// For each leg: casts the ground query from the animated ankle, puts the ankle target on the hit lifted by the foot height,
// tilts the foot to the (clamped) normal, and finds how far the pelvis must go down for the lowest foot (smoothed in
// `state`). Touches no pose: the targets go to solve_two_bone (apply_foot_placement does it). `state` may be null.
// `model_to_world` (optional): where the working space sits in a space that stays still while the character moves; foot
// locks are kept in it (null: the working space stays still itself). The ground query is called once for each leg, and once
// more for each leg that has a toe joint.
FootPlan plan_foot_placement(const Skeleton& skeleton, const Pose& pose, std::span<const Mat4f> world, std::span<const FootLeg> legs,
                             const GroundQuery& ground, const FootPlacementOptions& options = {}, FootPlacementState* state = nullptr,
                             f32 dt = 0.0f, const Mat4f* model_to_world = nullptr);

// Moves the pelvis joint along `up` by the plan's offset (its local translation, in its parent's space), then solves each
// grounded leg to its target with `leg_options` (its pole, stretch, weight; the foot's rotation from the plan), then turns each
// toe the plan has one for (by `leg_options.weight`, within `toe_max_tilt_degrees` of its animated turn).
void apply_foot_placement(const Skeleton& skeleton, Pose& pose, std::span<Mat4f> world, i32 pelvis_joint, std::span<const FootLeg> legs,
                          const FootPlan& plan, const TwoBoneOptions& leg_options = {}, Vec3f up = {0.0f, 1.0f, 0.0f});

} // namespace kin
