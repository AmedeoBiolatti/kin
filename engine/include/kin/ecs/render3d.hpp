#pragma once

// 3D entities (experimental, docs/3d_plan.md): a place in the world, composed
// through the flecs hierarchy as Transform2D is, and a mesh to draw there.
//
//     world.entity().set(kin::Transform3D{.position = {2, 0, 0}})
//                   .set(kin::MeshRenderer{.mesh = crate, .material = {.color = brown}});
//     kin::WorldRenderState3D state{world};
//     ...each frame:
//     state.propagate_transforms();
//     state.collect(scene);   // one instanced draw a mesh and material
//     r3d.render(view, scene);
//
// A character: a skinned model and an animator, which a system steps and draws (docs/rendering_3d.md, Skinned meshes):
//
//     auto hero = std::make_shared<const kin::Model>(r3d.load_model("models/hero.glb"));
//     world.entity().set(kin::Transform3D{}).set(kin::SkinnedMeshRenderer{.model = hero})
//                   .set(kin::Animator3D{.clip = "Walk"});
//     ...each fixed step (or frame):  state.animate(dt);   // clips, state machine, crossfades, blends
//     ...each frame:                  state.propagate_transforms(); state.collect(scene);
//
// Inverse kinematics on top of the clips (docs/rendering_3d.md, Inverse kinematics): constraints on the animator, solved in
// collect() after the clips and before the joint matrices, from targets the game sets each frame:
//
//     auto& hand = animator.add_ik({.name = "right_hand", .kind = kin::IKKind::TwoBone,
//                                   .joints = {"shoulder_R", "elbow_R", "hand_R"}, .fade_seconds = 0.15f});
//     ...each frame:  entity.get_mut<kin::Animator3D>()->ik("right_hand").set_target(hold).set_weight(grabbing ? 1.0f : 0.0f);

#if !defined(KIN_EXPERIMENTAL_3D)
#error "kin's 3D rendering is experimental: configure kin with -DKIN_EXPERIMENTAL_3D=ON"
#endif

#include <kin/anim/state_machine.hpp>
#include <kin/core/math3d.hpp>
#include <kin/ecs/world.hpp>
#include <kin/render3d/animation3d.hpp>
#include <kin/render3d/ik.hpp>
#include <kin/render3d/mesh.hpp>
#include <kin/render3d/model.hpp>
#include <kin/render3d/scene3d.hpp>

#include <flecs.h>

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kin {

// Where an entity sits relative to its parent (flecs ChildOf), or to the
// world without one: scaled, then turned, then moved to `position`.
struct Transform3D {
    Vec3f position{};
    Quatf rotation{};
    Vec3f scale{1.0f, 1.0f, 1.0f};

    Mat4f matrix() const { return Mat4f::trs(position, rotation, scale); }
};

// Transform3D composed through the hierarchy (WorldRenderState3D::propagate_transforms):
// the entity's own space to the world's. Added with Transform3D.
struct WorldTransform3D {
    Mat4f matrix{};

    Vec3f position() const { return {matrix.m[12], matrix.m[13], matrix.m[14]}; }
};

// A mesh drawn at the entity's WorldTransform3D.
struct MeshRenderer {
    Mesh mesh;
    Material3D material{};
    Color tint = colors::white;
    bool visible = true;
};

// A skinned model (a character) drawn at the entity's WorldTransform3D, in the pose of the entity's Animator3D (the rest
// pose without one). Not MeshRenderer: a mesh renderer's entities are instances of one mesh and merge into one draw, and a
// character is a model of several parts with a skeleton and a pose of its own.
struct SkinnedMeshRenderer {
    // Its parts, skeleton and clips. Shared: the entity keeps it alive (the animator points into it).
    std::shared_ptr<const Model> model;
    Color tint = colors::white;
    bool visible = true;
    // What pick says of every part of it, and how its frames are matched for the velocity under temporal anti-aliasing.
    // 0: the entity's id (the low 32 bits of flecs's), so that pick names the entity and two characters do not swap histories.
    u32 id = 0;
};

// What a constraint aims at: a point of the world, a point carried by another entity (its WorldTransform3D of the current
// frame, so it follows it; an entity that is gone or has no WorldTransform3D counts as no target), or nothing.
struct IKTarget {
    enum class Kind : u8 { None, World, Entity };
    Kind kind = Kind::None;
    // World: the point. Entity: an offset in that entity's own space (a hold's grip point).
    Vec3f position{};
    flecs::entity_t entity = 0;
};

enum class IKKind : u8 {
    TwoBone,    // joints: root, mid, end (shoulder, elbow, hand). The end goes to the target.
    LookAt,     // joints: root to tip (spine, neck, head). The tip's aim axis turns to the target.
    FootPlacer, // joints: the pelvis, then hip, knee, ankle for each leg. The `ground` query puts the feet; no target.
};

// One constraint of an Animator3D. Joint names are looked up in the model's skeleton when the animator binds to it (and again
// when the model changes); one that is not found makes the constraint do nothing. Everything is plain data, so a constraint can
// be built in a prefab-like table and changed per frame through the setters.
struct IKConstraint {
    // Where a pole is given, for a two-bone chain (and the knees of a foot placer).
    enum class Pole : u8 {
        None,           // the elbow stays on the side the clips bend it to
        WorldPosition,  // a point of the world the elbow points to
        ModelDirection, // a direction in the model's space (the entity's own axes) the elbow points along
    };

    std::string name;
    IKKind kind = IKKind::TwoBone;
    std::vector<std::string> joints;
    // The goal weight 0..1 and how long a change of it takes to take effect (0: at once); the weight in force eases linearly
    // in that time and is shaped by a smoothstep, so a hand lifts to a hold and lets go without a jerk.
    f32 weight = 1.0f;
    f32 fade_seconds = 0.0f;

    IKTarget target;
    Pole pole = Pole::None;
    Vec3f pole_vector{};

    // Two-bone: stretch, minimum bend (its weight and pole are taken from the fields above); with `match_rotation` the end
    // joint is turned to `target_rotation` (a world-space orientation: a hand to a hold's surface) by `rotation_weight`.
    TwoBoneOptions two_bone;
    bool match_rotation = false;
    Quatf target_rotation{};
    f32 rotation_weight = 1.0f;

    // Look-at: per joint (parallel to `joints`: weight and limits; `joint` is ignored) and the options.
    std::vector<LookAtJoint> look_links;
    LookAtOptions look_at;

    // Foot placer: the ankle's height above the sole for each leg (0.1 when missing), the options, and the ground as the game
    // knows it, in WORLD space (a physics raycast); the knees follow the pole as above (none: they keep the clips' bend).
    // `foot.up`, `ground_level` and the heights are in the model's space.
    std::vector<f32> foot_heights;
    FootPlacementOptions foot;
    GroundQuery ground;

    // --- the per-frame API (each returns the constraint, to chain)
    IKConstraint& set_target(Vec3f world_position) {
        target = {IKTarget::Kind::World, world_position, 0};
        return *this;
    }
    IKConstraint& set_target(flecs::entity entity, Vec3f local_offset = {}) {
        target = {IKTarget::Kind::Entity, local_offset, entity.id()};
        return *this;
    }
    IKConstraint& clear_target() {
        target = {};
        return *this;
    }
    IKConstraint& set_weight(f32 goal) {
        weight = goal;
        return *this;
    }
    IKConstraint& set_pole_position(Vec3f world_position) {
        pole = Pole::WorldPosition;
        pole_vector = world_position;
        return *this;
    }
    IKConstraint& set_pole_direction(Vec3f model_direction) {
        pole = Pole::ModelDirection;
        pole_vector = model_direction;
        return *this;
    }
    IKConstraint& set_target_rotation(Quatf world_rotation, f32 weight_of_rotation = 1.0f) {
        match_rotation = true;
        target_rotation = world_rotation;
        rotation_weight = weight_of_rotation;
        return *this;
    }

    // --- set by the system; not for setting
    f32 current = 0.0f;       // the weight in force before the smoothstep
    bool target_valid = false; // the target exists this frame
    LookAtState look_state;   // the followed direction, in world space
    Vec3f last_target{};      // the last world point the target gave (a target that goes away is let go of from there)
    bool has_last_target = false;
    FootPlacementState foot_state;
    std::vector<i32> resolved; // joint indices of `joints`
    const Skeleton* resolved_for = nullptr;
};

// What plays on a skinned entity, as data: a clip by name, or a state machine (kin/anim's AnimationStateMachine, driven by
// `params`) that chooses it, with crossfades and 1D blends by a parameter; layers over it with masks. WorldRenderState3D::animate
// steps it. A state `s` plays the blend space `blend_spaces[s]` if there is one, else the clip `machine->states[s]` (the
// state's own name if the machine gives none).
struct Animator3D {
    // One point of a 1D blend: `clip` plays at `position` of the parameter.
    struct BlendEntry {
        std::string clip;
        f32 position = 0.0f;
    };
    // Two or more clips blended by the float parameter `parameter` of `params` (speed: walk at 1.5, run at 4.5).
    struct BlendSpace {
        std::string parameter;
        std::vector<BlendEntry> entries;
    };
    // A layer over the base: `clip` on the joints of `mask_joint` and below (all, when empty), by `weight`; additive layers
    // add what the clip changes from its first frame (a lean, a breath).
    struct LayerConfig {
        std::string clip;
        std::string mask_joint;
        bool include_root = true;
        f32 weight = 1.0f;
        f32 speed = 1.0f;
        bool loop = true;
        bool additive = false;
    };
    // How long the crossfade of a transition `from` to `to` takes; an empty name matches any.
    struct Fade {
        std::string from, to;
        f32 seconds = 0.2f;
    };

    // Without a machine: the base clip's name (changing it crossfades over `fade_seconds`).
    std::string clip;
    f32 speed = 1.0f;
    bool loop = true;
    bool playing = true;
    f32 fade_seconds = 0.2f;
    std::vector<LayerConfig> layers;
    // The base layer's root motion moved onto the entity's Transform3D (its translation, turned by the entity's rotation, and
    // its rotation when `root_motion_axes.rotation`); the root joint is then held at rest in those axes.
    bool apply_root_motion = false;
    // Or only held: the root joint stays at rest in those axes (the character animates in place) and the entity is not moved.
    bool remove_root_motion = false;
    RootMotionAxes root_motion_axes{};

    // With a machine: the state it is in (empty: machine->initial), its parameters (set them from game code: speed,
    // grounded, climbing; triggers are consumed by the transition that takes them), blend spaces by state, fade lengths.
    std::shared_ptr<const AnimationStateMachine> machine;
    AnimationParams params;
    std::string state;
    std::unordered_map<std::string, BlendSpace> blend_spaces;
    std::vector<Fade> fades;

    // Inverse kinematics, applied after the clips and layers, in this order, each seeing the one before's result (see
    // IKConstraint). An animator with none is exactly what it was without this field.
    std::vector<IKConstraint> ik_constraints;
    IKConstraint& add_ik(IKConstraint constraint) { return ik_constraints.emplace_back(std::move(constraint)); }
    // The constraint called `name`, or null.
    IKConstraint* find_ik(std::string_view name) {
        for (IKConstraint& c : ik_constraints) {
            if (c.name == name) {
                return &c;
            }
        }
        return nullptr;
    }
    // The constraint called `name`; one is made (a two-bone with no joints, which does nothing) when there is none, so that
    // `animator.ik("left_hand").set_target(...)` is always valid.
    IKConstraint& ik(std::string_view name) {
        if (IKConstraint* found = find_ik(name)) {
            return *found;
        }
        IKConstraint made;
        made.name = std::string(name);
        return ik_constraints.emplace_back(std::move(made));
    }

    // After collect() (or apply_ik()) with constraints: the pose they made (the clips' pose with the IK on it), and the world
    // matrices of its joints in the model's space, for gameplay that wants to know where the hand is. Empty without constraints.
    Pose ik_pose;
    std::vector<Mat4f> ik_world;
    f32 ik_dt = 0.0f; // the time animate() has stepped since the constraints were last solved (smoothing uses it). Not for setting.

    // Set by animate(): the player, and what it was bound to and plays. Not for setting.
    AnimationPlayer3D player;
    const Model* bound = nullptr;
    std::string applied;
    std::vector<std::string> applied_layers;
};

class WorldRenderState3D {
public:
    explicit WorldRenderState3D(flecs::world& world);
    explicit WorldRenderState3D(EcsWorld& world) : WorldRenderState3D(world.raw()) {}

    // Every WorldTransform3D from its Transform3D and its parent's, parents first.
    void propagate_transforms();
    // A draw into `scene` for each mesh and material the MeshRenderers use, all
    // their entities instances of it (in the order the world keeps them); and each SkinnedMeshRenderer's model in its
    // animator's pose (draw_skinned, one range of matrices for all its parts).
    void collect(Scene3D& scene);
    // Steps every Animator3D by `dt` seconds (call it once per fixed step, or per frame): binds it to its entity's model,
    // runs its state machine (one transition a step), crossfades, blends, advances the clips and layers, and moves the
    // entity by root motion if asked. Deterministic: the same dt, parameters and clips give the same poses, byte for byte.
    void animate(f32 dt);
    // Solves every animator's IK constraints against the current WorldTransform3Ds (call after propagate_transforms()) and
    // fills `ik_pose` / `ik_world`. collect() does this itself; call it earlier only to read an IK'd joint before drawing.
    // Calling it more than once a frame is harmless (the smoothing advances only once per animate()), it just costs again.
    void apply_ik();

private:
    // One animator's constraints over `base` (the clips' pose); `world` is its entity's matrix.
    static void solve_ik(Animator3D& animator, const Skeleton& skeleton, const Pose& base, const Mat4f& world, const flecs::world& ecs);
    flecs::observer _transform_observer;
    flecs::query<WorldTransform3D, const Transform3D, const WorldTransform3D> _transforms;
    flecs::query<const WorldTransform3D, const MeshRenderer> _meshes;
    flecs::query<const SkinnedMeshRenderer, Animator3D, Transform3D*> _animators; // the transform is optional (a pointer)
    flecs::query<const WorldTransform3D, const SkinnedMeshRenderer, Animator3D*> _skinned; // the animator is optional
    std::vector<Mat4f> _pose_scratch; // collect()'s matrices of one entity, reused
    struct Bucket {
        Mesh mesh;
        Material3D material;
        std::vector<MeshInstance> instances;
    };
    std::vector<Bucket> _buckets; // kept between frames, emptied each collect
};

} // namespace kin
