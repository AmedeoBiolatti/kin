#pragma once

// Ragdolls for kin/physics3d (experimental, docs/physics_3d.md "Ragdolls"): a body per bone, joined by constraints, that
// takes over a skinned character's pose from an animation clip (and gives it back). PASSIVE: the pose comes from the
// physics bodies, nothing drives them (no motors); a "get up" is a blend back to a clip, the clip and the place to stand are
// the game's.
//
//     kin::RagdollDesc desc = kin::auto_ragdoll_desc(skeleton, skeleton.rest_pose());
//     const kin::RagdollHandle3D r = world.create_ragdoll(desc, bone_world_matrices);
//     ...each fixed step:
//     world.set_ragdoll_targets(r, animated_pose_in_world);   // what the kinematic bodies follow while it animates
//     world.step(dt);
//     // hit by something:
//     world.start_ragdoll(r, {.blend_seconds = 0.15f, .impulse_bone = chest, .impulse = {0, 0, -300}});
//     ...
//     world.ragdoll_pose(r, bones);                           // weight, position, rotation of each bone
//     world.ragdoll_get_up(r, 0.8f);                          // blend back to the clip over 0.8 s
//
// States (RagdollMode3D): Animated (the bodies are KINEMATIC and follow the clip's pose, so characters and projectiles can
// hit them; or parked, see RagdollDesc::animated_bodies), Ragdoll (dynamic; the pose comes from the bodies), Blend (dynamic,
// a held share of both), GetUp (the bodies are kinematic on the clip again; the pose blends from where they lay to the clip
// over a time; then Animated). A bone with
// weight 0 (per bone, `set_ragdoll_bone_weights` / `set_ragdoll_subtree`) stays kinematic in every state: a partial ragdoll
// (an arm, a tail) leaves the rest animated.
//
// Determinism: the bodies and constraints are ordinary ones made in bone order; the state (mode, blend, timers) is in
// World3D::state_hash(), save_state() and restore_state() (a restored world repeats the exact trajectory), and nothing
// depends on the number of workers.

#include <kin/core/math3d.hpp>
#include <kin/core/types.hpp>
#include <kin/physics3d/handles3d.hpp>

#include <span>
#include <string>
#include <vector>

namespace kin {

class Skeleton3D;
struct Pose3D;

// A ragdoll in a World3D: an index and a generation, as Body3D is.
struct RagdollHandle3D {
    u32 index = UINT32_MAX;
    u32 generation = 0;

    bool valid() const { return index != UINT32_MAX; }
    explicit operator bool() const { return valid(); }
    friend constexpr bool operator==(RagdollHandle3D, RagdollHandle3D) = default;
};

enum class RagdollShape3D : u8 { Capsule, Box, Sphere };

// How a bone hangs from its parent's body.
enum class RagdollJoint3D : u8 {
    Cone,  // swing-twist: a half angle of swing about the twist axis and a range of twist (a shoulder, a hip, a neck)
    Hinge, // one axis, limits in degrees from the pose the ragdoll is made in (a knee, an elbow)
    Fixed, // welded (a hand to a forearm, a stiff head)
};

// One bone with a body. Everything is in the BONE'S OWN FRAME: the origin is the joint (where the bone meets its parent), the
// axes are the joint's world rotation at the pose the ragdoll is made from. The body sits at the joint with the joint's
// rotation, so that the bone's pose is the body's pose exactly.
struct RagdollBoneDesc3D {
    std::string name;
    // The skeleton joint this bone is (kept for the ECS and the tests; the physics does not read it).
    i32 joint = -1;
    // Index of the parent bone in RagdollDesc::bones (always lower than this one), or -1 for the root. A bone whose skeleton
    // parent has no body hangs from its nearest ancestor that has.
    i32 parent = -1;

    RagdollShape3D shape = RagdollShape3D::Capsule;
    // The shape's centre in the bone's frame and how it is turned there (a capsule's axis is its +Y).
    Vec3f offset{};
    Quatf rotation{};
    f32 radius = 0.05f;
    f32 half_height = 0.1f;     // capsule: half of the straight part
    Vec3f half_extents{0.05f, 0.1f, 0.05f}; // box

    f32 mass = 0.0f;            // kg when above zero, else density times the shape's volume
    f32 density = 1000.0f;
    f32 friction = 0.6f;
    f32 restitution = 0.0f;
    f32 linear_damping = 0.1f;
    f32 angular_damping = 1.0f;

    // The joint to the parent body, at this bone's origin. `joint_axis` (in the bone's frame): the twist axis of a cone, the
    // axis of a hinge.
    RagdollJoint3D joint_kind = RagdollJoint3D::Cone;
    Vec3f joint_axis{0.0f, 1.0f, 0.0f};
    f32 swing_degrees = 50.0f;
    f32 twist_min_degrees = -30.0f;
    f32 twist_max_degrees = 30.0f;
    f32 hinge_min_degrees = -140.0f;
    f32 hinge_max_degrees = 0.0f;
    // Torque (N m) that resists a joint's motion: a settling limb that does not flop for ever.
    f32 joint_friction = 0.0f;

    // The share of the pose that comes from this body (times the ragdoll's blend). 0: the bone stays animated: a kinematic body
    // that follows the clip.
    f32 weight = 1.0f;
};

struct RagdollDesc {
    // Parent before child. The first bone with `parent` -1 is the root unless `root_bone` says otherwise.
    std::vector<RagdollBoneDesc3D> bones;
    // The layer of every bone (layers3d::Dynamic by default: the ragdoll meets what dynamic bodies meet).
    u8 layer = layers3d::Dynamic;
    // Bones that are not neighbours collide with each other when true (a folded arm against a chest). Neighbours never do
    // (they are joined). Off: all bones of one ragdoll skip each other, which is cheaper and does not jitter.
    bool self_collision = false;
    // Swept against static geometry, so that a fast bone does not pass through a floor or a wall.
    bool continuous = true;
    // Animated state: kinematic bodies that follow the clip's pose (what a bullet can hit). false parks them: no
    // collisions and no cost until the ragdoll starts.
    bool animated_bodies = true;
    // The ragdoll has settled when every dynamic body is slower than this (m/s, rad/s) for `settle_seconds`, or asleep.
    f32 settle_speed = 0.15f;
    f32 settle_angular_speed = 1.0f;
    f32 settle_seconds = 0.5f;
    // After settling the bodies are made kinematic and put to sleep: costs nothing, stays hittable, and does not move again
    // until the ragdoll is started, blended or told to get up (a corpse).
    bool freeze_on_settle = false;
    // Solver iterations for these bodies (0: the world's, which is 10 and 2): a chain of bodies and a floor wants more.
    u32 velocity_steps = 20;
    u32 position_steps = 6;
    f32 gravity_factor = 1.0f;
    // The root bone (the pelvis: the place a get-up stands at) and, in its frame, the way the character faces and its up axis.
    i32 root_bone = 0;
    Vec3f forward{0.0f, 0.0f, 1.0f};
    Vec3f up{0.0f, 1.0f, 0.0f};
    u64 user_id = 0;
};

// What auto_ragdoll_desc does.
struct AutoRagdollOptions {
    // A capsule's radius is this share of its bone's length, kept between the two limits (m).
    f32 radius_ratio = 0.22f;
    f32 min_radius = 0.025f;
    f32 max_radius = 0.12f;
    // A bone with no child gets a capsule this long (m) along its parent's direction (a head, a hand, a foot), this thick
    // by its length.
    f32 leaf_length = 0.15f;
    f32 leaf_radius_ratio = 0.4f;
    // Bones shorter than this (m) get no body (a helper joint).
    f32 min_bone_length = 0.02f;
    // Mass of the whole character (kg), shared between the bones by their volume.
    f32 total_mass = 70.0f;
    f32 swing_degrees = 50.0f;
    f32 twist_degrees = 30.0f;
    // Friction at every joint, N m for each kg of the bone it moves.
    f32 joint_friction = 0.5f;
    // A bone is a hinge when its name contains a pattern (case insensitive; the first match wins): the axis is in the
    // model's space at the pose given; the range in degrees from that pose (min in [-180, 0], max in [0, 180]). The default
    // table is for a character facing +Z in a T pose: knees bend about +X, elbows about +Y (the left one the other way
    // round to the right one). Another rig brings its own table.
    struct Override {
        std::string pattern;
        RagdollJoint3D kind = RagdollJoint3D::Hinge;
        Vec3f axis_model{1.0f, 0.0f, 0.0f};
        f32 min_degrees = -140.0f;
        f32 max_degrees = 0.0f;
    };
    std::vector<Override> overrides = {
        {"knee", RagdollJoint3D::Hinge, {1.0f, 0.0f, 0.0f}, 0.0f, 140.0f},
        {"elbow_l", RagdollJoint3D::Hinge, {0.0f, 1.0f, 0.0f}, -140.0f, 0.0f},
        {"elbow_r", RagdollJoint3D::Hinge, {0.0f, 1.0f, 0.0f}, 0.0f, 140.0f},
        {"elbow", RagdollJoint3D::Hinge, {0.0f, 1.0f, 0.0f}, -140.0f, 0.0f},
    };
    // The way the character faces and its up, in the model's space at the pose given: a leaf bone that points down (a foot)
    // gets its capsule along `forward` instead, and the desc's `forward` and `up` (the get-up's facing) come from these.
    Vec3f forward{0.0f, 0.0f, 1.0f};
    Vec3f up{0.0f, 1.0f, 0.0f};
    // Bone names (substrings, case insensitive) that get no body.
    std::vector<std::string> skip;
    u8 layer = layers3d::Dynamic;
    bool self_collision = false;
    // The root is the first root joint's first child with a body (a skeleton's "root" above the pelvis has none of its own
    // length) when this is empty; else the bone with this name (substring).
    std::string root_name = "pelvis";
};

// Capsules per bone from a skeleton at a pose (the bind pose): a capsule from each bone's joint to its main child (the one that
// continues its direction; the longest for a root), a short capsule for a leaf, radius from length by `radius_ratio`, swing-twist
// joints with the default limits and hinges where a name matches an override. The pose is the ragdoll's rest: build the
// ragdoll with the bones' world matrices of the same pose. `bone` i of the result keeps its skeleton joint in `joint`.
RagdollDesc auto_ragdoll_desc(const Skeleton3D& skeleton, const Pose3D& pose, const AutoRagdollOptions& options = {});

enum class RagdollMode3D : u8 { Animated, Ragdoll, Blend, GetUp };

// What start_ragdoll does besides changing the mode.
struct RagdollStart3D {
    // The blend from the clip's pose to the bodies' takes this long (0: at once).
    f32 blend_seconds = 0.0f;
    // When set, every body gets `velocity` (the character's), else it keeps what the kinematic follow gave it.
    bool set_velocity = false;
    Vec3f velocity{};
    // An impulse (N s) at a bone, from a hit, at a world point (the bone's body origin when `point_given` is false).
    i32 impulse_bone = -1;
    Vec3f impulse{};
    Vec3f impulse_point{};
    bool point_given = false;
};

struct RagdollState3D {
    RagdollMode3D mode = RagdollMode3D::Animated;
    // The share of the pose that comes from the bodies now (0 clip, 1 bodies), and where it is going.
    f32 alpha = 0.0f;
    f32 target_alpha = 0.0f;
    // Dynamic bodies and awake ones (of those), and the fastest body (m/s).
    u32 dynamic_bodies = 0;
    u32 awake_bodies = 0;
    f32 max_speed = 0.0f;
    bool settled = false;
    bool frozen = false;
};

// One bone as the pose writer needs it: the share of the body in the pose (the blend times the bone's weight; 0: leave the
// clip's pose alone) and the body's place, which is the bone's world matrix (translation and rotation, no scale).
struct RagdollBonePose3D {
    f32 weight = 0.0f;
    Vec3f position{};
    Quatf rotation{};
};

// Where a character that gets up from the ragdoll stands (World3D::ragdoll_get_up_place).
struct RagdollPlace3D {
    bool found = false;       // ground was found under the root bone
    Vec3f position{};         // on the ground under the root bone
    Quatf rotation{};         // facing about `up`: the root bone's forward, or its head direction when it lies face up or down
    bool face_down = false;   // the root bone's forward points down
    bool lying = false;       // the root bone's forward is nearly along up (on the back or belly)
    f32 height = 0.0f;        // of the root bone above the ground
};

enum class RagdollEventKind3D : u8 {
    Started,   // the bodies took over
    Settled,   // every dynamic body is slow or asleep (once a start)
    Recovered, // the blend back to the clip finished
    Blocked,   // a recovery was refused for want of room (reported by the ECS helper)
};

struct RagdollEvent3D {
    RagdollHandle3D ragdoll{};
    u64 user_id = 0;
    RagdollEventKind3D kind = RagdollEventKind3D::Started;
};

} // namespace kin
