#pragma once

// 3D entities (experimental, docs/3d_plan.md): a place in the world, composed
// through the flecs hierarchy as Transform2D is, and a mesh to draw there.
//
//     world.entity().set(kin::Transform3D{.position = {2, 0, 0}})
//                   .set(kin::MeshRenderer3D{.mesh = crate, .material = {.color = brown}});
//     kin::WorldRenderState3D state{world};
//     ...each frame:
//     state.propagate_transforms();
//     state.collect(scene);   // one instanced draw a mesh and material
//     r3d.render(view, scene);
//
// A character: a skinned model and an animator, which a system steps and draws (docs/rendering_3d.md, Skinned meshes):
//
//     auto hero = std::make_shared<const kin::Model3D>(r3d.load_model("models/hero.glb"));
//     world.entity().set(kin::Transform3D{}).set(kin::SkinnedMeshRenderer3D{.model = hero})
//                   .set(kin::Animator3D{.clip = "Walk"});
//     ...each fixed step (or frame):  state.animate(dt);   // clips, state machine, crossfades, blends
//     ...each frame:                  state.propagate_transforms(); state.collect(scene);
//
// Inverse kinematics on top of the clips (docs/rendering_3d.md, Inverse kinematics): constraints on the animator,
// solved in collect() after the clips and before the joint matrices, from targets the game sets each frame:
//
//     auto& hand = animator.add_ik({.name = "right_hand", .kind = kin::IKKind3D::TwoBone,
//                                   .joints = {"shoulder_R", "elbow_R", "hand_R"}, .fade_seconds = 0.15f});
//     ...each frame:  entity.get_mut<kin::Animator3D>()->ik("right_hand").set_target(hold).set_weight(grabbing ? 1.0f :
//     0.0f);

#if !defined(KIN_EXPERIMENTAL_3D)
#error "kin's 3D rendering is experimental: configure kin with -DKIN_EXPERIMENTAL_3D=ON"
#endif

#include <flecs.h>
#include <kin/anim/state_machine.hpp>
#include <kin/core/math3d.hpp>
#include <kin/ecs/component.hpp>
#include <kin/ecs/world.hpp>
#include <kin/render3d/animation3d.hpp>
#include <kin/render3d/ik3d.hpp>
#include <kin/render3d/mesh3d.hpp>
#include <kin/render3d/model3d.hpp>
#include <kin/render3d/scene3d.hpp>
#include <kin/render3d/timeofday3d.hpp>
#include <kin/scene/joint_attachment3d.hpp>
#include <memory>
#include <optional>
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

// Optional world-space placement used only by collect(). Simulation, hierarchy,
// animation/IK targets and particles continue to use WorldTransform3D. Does not
// propagate to children; clear/disable it when no presentation override is needed.
struct RenderTransform3D {
    Mat4f matrix{};
    bool enabled = false;
};

// A mesh drawn at the entity's WorldTransform3D.
struct MeshRenderer3D {
    Mesh3D mesh;
    Material3D material{};
    Color tint = colors::white;
    bool visible = true;
    // What pick says of it, and how its frames are matched for the velocity under temporal anti-aliasing. 0: the
    // entity's id (the low 32 bits of flecs's: an index that is recycled inherits the dead entity's history).
    u32 id = 0;
};

// A complete static model, placed by Transform3D. `asset` is authored relative
// to Assets3D::root(); resolve_render3d_assets binds it on the renderer thread.
// `model` is runtime-only and may also be supplied directly from C++. With an
// Animator3D on the entity, a model whose animations move plain nodes (a door, a
// fan: Model3D::node_skeleton) plays its clips on them; without one it is drawn at rest.
struct ModelRenderer3D {
    std::shared_ptr<const Model3D> model;
    Color tint = colors::white;
    bool visible = true;
    u32 id = 0; // zero: use the entity's ID for every model part (its low 32 bits: a recycled index inherits the dead
                // one's history)
    ComponentAssetRef asset;
};

struct MaterialBinding3D;
// Optional material patches for one static/skinned model. Only asset/variant
// are authored; binding is immutable runtime data shared through Assets3D.
struct ModelMaterials3D {
    ComponentAssetRef asset;
    std::string variant;
    std::shared_ptr<const MaterialBinding3D> binding;
};

// A point light at the entity's WorldTransform3D (P3b; opt-in: no light components, no lights).
// WorldRenderState3D::collect adds each to Scene3D::lights.points, with the entity's id as the light's id (so its
// shadow slot is kept while it stays important).
struct PointLight3D {
    LinearRgb color{1.0f, 1.0f, 1.0f}; // linear; as PointLightSource3D::color
    f32 radius = 5.0f;
    f32 size = 0.01f;
    LightShadow3D shadow{};
    bool enabled = true;
};

// A spot light at the entity's WorldTransform3D, shining down the entity's own -Z axis (the way a camera looks; turn
// the entity to aim it, or parent it to a camera or a hand). collect() adds each to Scene3D::lights.spots.
struct SpotLight3D {
    LinearRgb color{1.0f, 1.0f, 1.0f};
    f32 radius = 10.0f;
    f32 size = 0.01f;
    f32 inner_angle = 20.0f; // degrees; see SpotLightSource3D
    f32 outer_angle = 30.0f;
    LightShadow3D shadow{};
    bool enabled = true;
};

// A particle emitter at the entity's WorldTransform3D (P3c; opt-in: no emitters, no particles). The simulation is the
// component's own state (`system`: its seed, its desc, its live particles), stepped by
// WorldRenderState3D::simulate_particles and drawn by collect(). The emitter's shape and directions are in the entity's
// space (a cone's axis is the entity's +Y: turn the entity to aim it).
struct ParticleEmitter3D {
    ParticleSystem3D system{};
    bool enabled = true;             // false: no births, and not drawn (the live particles are kept)
    bool emitting = true;            // false: stop births while existing particles finish visibly
    ComponentAssetRef texture_asset; // empty: procedural disc, or a C++ texture
    bool texture_nearest = false;
    std::string bound_texture; // runtime-only; empty refs clear only authored bindings
};

// A skinned model (a character) drawn at the entity's WorldTransform3D, in the pose of the entity's Animator3D (the
// rest pose without one). Not MeshRenderer3D: a mesh renderer's entities are instances of one mesh and merge into one
// draw, and a character is a model of several parts with a skeleton and a pose of its own.
struct SkinnedMeshRenderer3D {
    // Its parts, skeleton and clips. Shared: the entity keeps it alive (the animator points into it).
    std::shared_ptr<const Model3D> model;
    Color tint = colors::white;
    bool visible = true;
    // What pick says of every part of it, and how its frames are matched for the velocity under temporal anti-aliasing.
    // 0: the entity's id (the low 32 bits of flecs's), so that pick names the entity and two characters do not swap
    // histories; an index that is recycled inherits the dead entity's history.
    u32 id = 0;
    // Optional authored reference; model stays runtime-only. Resolve before
    // animation/physics setup. An empty reference leaves C++ models untouched.
    ComponentAssetRef asset;
};

// What a constraint aims at: a point of the world, a point carried by another entity (its WorldTransform3D of the
// current frame, so it follows it; an entity that is gone or has no WorldTransform3D counts as no target), or nothing.
struct IKTarget3D {
    enum class Kind : u8 { None, World, Entity, SceneEntity };
    Kind kind = Kind::None;
    // World: the point. Entity: an offset in that entity's own space (a hold's grip point).
    Vec3f position{};
    flecs::entity_t entity = 0;
    SceneEntity3D scene_entity; // scene generation and pool lease checked every animation/solve
};

enum class IKKind3D : u8 {
    TwoBone,    // joints: root, mid, end (shoulder, elbow, hand). The end goes to the target.
    LookAt,     // joints: root to tip (spine, neck, head). The tip's aim axis turns to the target.
    FootPlacer, // joints: the pelvis, then hip, knee, ankle for each leg. The `ground` query puts the feet; no target.
    Fabrik, // joints: root to tip, any number (a tail, a rope, a spine). The tip goes to the target; `pole` turns the
            // bulge.
};

// One constraint of an Animator3D. Joint names are looked up in the model's skeleton when the animator binds to it (and
// again when the model changes); one that is not found makes the constraint do nothing. Everything is plain data, so a
// constraint can be built in a prefab-like table and changed per frame through the setters.
struct IKConstraint3D {
    // Where a pole is given, for a two-bone chain (and the knees of a foot placer).
    enum class Pole : u8 {
        None,           // the elbow stays on the side the clips bend it to
        WorldPosition,  // a point of the world the elbow points to
        ModelDirection, // a direction in the model's space (the entity's own axes) the elbow points along
    };

    std::string name;
    IKKind3D kind = IKKind3D::TwoBone;
    std::vector<std::string> joints;
    // The goal weight 0..1 and how long a change of it takes to take effect (0: at once); the weight in force eases
    // linearly in that time and is shaped by a smoothstep, so a hand lifts to a hold and lets go without a jerk.
    f32 weight = 1.0f;
    f32 fade_seconds = 0.0f;

    IKTarget3D target;
    Pole pole = Pole::None;
    Vec3f pole_vector{};

    // Two-bone: stretch, minimum and maximum bend, the hinge and the twist limit (its weight and pole are taken from
    // the fields above); with `match_rotation` the end joint is turned to `target_rotation` (a world-space orientation:
    // a hand to a hold's surface) by `rotation_weight`. The knees of a foot placer take these too.
    TwoBoneOptions3D two_bone;
    bool match_rotation = false;
    Quatf target_rotation{};
    f32 rotation_weight = 1.0f;

    // Look-at: per joint (parallel to `joints`: weight and limits; `joint` is ignored) and the options.
    std::vector<LookAtJoint3D> look_links;
    LookAtOptions3D look_at;

    // Fabrik: tolerance, iterations, whether the first joint is fixed, its pole is `pole` above (the weight is taken
    // from above).
    FabrikOptions3D fabrik;

    // Foot placer: the ankle's height above the sole for each leg (0.1 when missing), the options, and the ground as
    // the game knows it, in WORLD space (a physics raycast); the knees follow the pole as above (none: they keep the
    // clips' bend). `foot.up`, `ground_level` and the heights are in the model's space. Foot locking (`foot.lock_feet`)
    // holds a planted foot in the world while the entity moves. `toe_joints` names a toe for each leg (an empty name:
    // none; the list may be shorter) and `toe_heights` the ball's height above the sole (0.03 when missing); each toe
    // costs the game's query one more call a frame. The query is called once a frame for each leg (and toe), however
    // many times the constraints are solved.
    std::vector<f32> foot_heights;
    std::vector<std::string> toe_joints;
    std::vector<f32> toe_heights;
    FootPlacementOptions3D foot;
    GroundQuery3D ground;

    // --- the per-frame API (each returns the constraint, to chain)
    IKConstraint3D& set_target(Vec3f world_position) {
        target = {IKTarget3D::Kind::World, world_position, 0};
        return *this;
    }
    IKConstraint3D& set_target(flecs::entity entity, Vec3f local_offset = {}) {
        target = {IKTarget3D::Kind::Entity, local_offset, entity.id()};
        return *this;
    }
    IKConstraint3D& set_scene_target(SceneEntity3D entity, Vec3f local_offset = {}) {
        target = {IKTarget3D::Kind::SceneEntity, local_offset, 0, entity};
        return *this;
    }
    IKConstraint3D& clear_target() {
        target = {};
        return *this;
    }
    IKConstraint3D& set_weight(f32 goal) {
        weight = goal;
        return *this;
    }
    IKConstraint3D& set_pole_position(Vec3f world_position) {
        pole = Pole::WorldPosition;
        pole_vector = world_position;
        return *this;
    }
    IKConstraint3D& set_pole_direction(Vec3f model_direction) {
        pole = Pole::ModelDirection;
        pole_vector = model_direction;
        return *this;
    }
    IKConstraint3D& set_target_rotation(Quatf world_rotation, f32 weight_of_rotation = 1.0f) {
        match_rotation = true;
        target_rotation = world_rotation;
        rotation_weight = weight_of_rotation;
        return *this;
    }

    // --- set by the system; not for setting
    f32 current = 0.0f;        // the weight in force before the smoothstep
    bool target_valid = false; // the target exists this frame
    LookAtState3D look_state;  // the followed direction, in world space
    Vec3f last_target{};       // the last world point the target gave (a target that goes away is let go of from there)
    bool has_last_target = false;
    FootPlacementState3D foot_state;
    TwoBoneState3D bend_state; // the side the elbow was on (two_bone.bend_swing_degrees_per_second)
    struct GroundCache {
        u64 epoch = ~u64{0};
        Vec3f from{}, to{};
        std::optional<GroundHit3D> hit;
    };
    std::vector<GroundCache> ground_cache; // the query's answers of this frame, by the order it was asked in
    std::vector<i32> resolved;             // joint indices of `joints`
    std::vector<i32> resolved_toes;        // joint indices of `toe_joints` (-1: none)
    // The skeleton `resolved` was looked up in. An address is not an identity (a skeleton freed and replaced can come
    // back at it), so this is only compared within one binding of the animator to a model: animate() clears it whenever
    // the animator rebinds.
    const Skeleton3D* resolved_for = nullptr;
};

// A pose that something else writes over the clips' and the constraints' (ragdoll physics:
// kin/physics3d/ragdoll3d.hpp): per joint a share (0 leaves the joint as the clips and IK made it, 1 takes the physics
// body's place) and that place in the WORLD. It is applied after the clips, layers and IK constraints and before the
// joint matrices, so that the skinned draw, the TAA velocity, the shadow pass and the shadow cache all see it. The
// rotation is taken from the body for every joint with a share; the translation too for the joints at the top of the
// shared part (their parent has less share), so that bone lengths and scale stay those of the pose. Empty (the default)
// changes nothing. WorldPhysicsState3D fills it for an entity with a Ragdoll3D; a game may fill it itself (a puppet).
struct PoseOverride3D {
    std::vector<f32> weights;    // by joint (the skeleton's joint count; any other size is ignored)
    std::vector<Vec3f> position; // world
    std::vector<Quatf> rotation;

    bool empty() const { return weights.empty(); }
    void clear() {
        weights.clear();
        position.clear();
        rotation.clear();
    }
};

// Shared, authored definitions. One playback slot per Animator3D; no action queue.
struct CharacterAction3D {
    std::string name, clip, mask_joint;
    ActionPlayback3D playback; // mask_joint index is resolved from the name
};
using CharacterActions3D = std::vector<CharacterAction3D>;

// What plays on a skinned entity, as data: a clip by name, or a state machine (kin/anim's AnimationStateMachine, driven
// by `params`) that chooses it, with crossfades and 1D blends by a parameter; layers over it with masks.
// WorldRenderState3D::animate steps it. A state `s` plays the blend space `blend_spaces[s]` if there is one, else the
// clip `machine->states[s]` (the state's own name if the machine gives none). It poses the first skin of a
// SkinnedMeshRenderer3D's model, or the node skeleton (Model3D::node_skeleton) of a ModelRenderer3D's: its clips,
// layers' masks and IK joints are looked up in that one.
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
    // A layer over the base: `clip` on the joints of `mask_joint` and below (all, when empty), by `weight`; additive
    // layers add what the clip changes from its first frame (a lean, a breath).
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
    // The base layer's root motion moved onto the entity's Transform3D (its translation, turned by the entity's
    // rotation, and its rotation when `root_motion_axes.rotation`); the root joint is then held at rest in those axes.
    bool apply_root_motion = false;
    // Or only held: the root joint stays at rest in those axes (the character animates in place) and the entity is not
    // moved.
    bool remove_root_motion = false;
    RootMotionAxes3D root_motion_axes{};

    // With a machine: the state it is in (empty: machine->initial), its parameters (set them from game code: speed,
    // grounded, climbing; triggers are consumed by the transition that takes them), blend spaces by state, fade
    // lengths.
    std::shared_ptr<const AnimationStateMachine> machine;
    AnimationParams params;
    std::string state;
    std::unordered_map<std::string, BlendSpace> blend_spaces;
    std::vector<Fade> fades;

    // Inverse kinematics, applied after the clips and layers, in this order, each seeing the one before's result (see
    // IKConstraint3D). An animator with none is exactly what it was without this field.
    std::vector<IKConstraint3D> ik_constraints;
    IKConstraint3D& add_ik(IKConstraint3D constraint) { return ik_constraints.emplace_back(std::move(constraint)); }
    // The constraint called `name`, or null.
    IKConstraint3D* find_ik(std::string_view name) {
        for (IKConstraint3D& c : ik_constraints) {
            if (c.name == name) {
                return &c;
            }
        }
        return nullptr;
    }
    const IKConstraint3D* find_ik(std::string_view name) const {
        for (const auto& constraint : ik_constraints)
            if (constraint.name == name)
                return &constraint;
        return nullptr;
    }
    // The constraint called `name`; one is made (a two-bone with no joints, which does nothing) when there is none, so
    // that `animator.ik("left_hand").set_target(...)` is always valid.
    IKConstraint3D& ik(std::string_view name) {
        if (IKConstraint3D* found = find_ik(name)) {
            return *found;
        }
        IKConstraint3D made;
        made.name = std::string(name);
        return ik_constraints.emplace_back(std::move(made));
    }

    // Physics (or any puppeteer) writing over the pose: see PoseOverride3D. The constraints' result and `ik_pose` /
    // `ik_world` below include it when it is set.
    PoseOverride3D pose_override;

    // After collect() (or apply_ik()) with constraints: the pose they made (the clips' pose with the IK on it), and the
    // world matrices of its joints in the model's space, for gameplay that wants to know where the hand is. Empty
    // without constraints.
    Pose3D ik_pose;
    std::vector<Mat4f> ik_world;
    f32 ik_dt = 0.0f; // the time animate() has stepped since the constraints were last solved (smoothing uses it). Not
                      // for setting.

    // Optional shared marker table; no per-entity string copies. Read after
    // animate(), before the next step. Dispatch drains; unhandled channels expire.
    std::shared_ptr<const CharacterActions3D> actions;
    std::shared_ptr<const AnimationEventTracks3D> event_tracks;
    AnimationEventBatch3D events;

    // Set by animate(): the player, and what it was bound to and plays. Not for setting.
    AnimationPlayer3D player;
    // The model the player is bound to. `bound` is only its address, kept for readers that compare it: a model freed
    // and replaced can come back at the same address, so what decides a rebind is `bound_model`'s owner (bound_to).
    const Model3D* bound = nullptr;
    std::weak_ptr<const Model3D> bound_model;
    bool bound_to(const std::shared_ptr<const Model3D>& model) const {
        return model && bound == model.get() && !bound_model.owner_before(model) && !model.owner_before(bound_model);
    }
    std::string applied;
    std::vector<std::string> applied_layers;
};

// Assets must already be resolved. Works before the first animate() as well as
// during ordinary fixed-step systems; invalid requests/busy slots return zero.
// Restart the same definition with Replace. Base locomotion continues underneath.
ActionToken3D play_action3d(EcsEntity actor, std::string_view name, ActionStart3D start = ActionStart3D::IfIdle);
bool cancel_action3d(EcsEntity actor, ActionToken3D token, f32 fade_seconds = -1);
ActionStatus3D action_status3d(EcsEntity actor, ActionToken3D token);

class AnimationEventDispatch;
// Consume once after animate(), e.g. in SystemPhase::Audio. Handlers receive the
// animated entity. 3D markers do not resolve the 2D target/offset fields. Returns
// the number of unhandled events; these are discarded, never kept in a queue.
u32 dispatch_animation_events3d(EcsWorld& world, const AnimationEventDispatch& dispatch);

class WorldRenderState3D {
public:
    explicit WorldRenderState3D(flecs::world& world);
    explicit WorldRenderState3D(EcsWorld& world) : WorldRenderState3D(world.raw()) { _ecs = &world; }

    // Every WorldTransform3D from its Transform3D and its parent's, parents first.
    void propagate_transforms();
    // (Also: a light in `scene` for each PointLight3D and SpotLight3D, at its entity's place.)
    // A draw into `scene` for each mesh and material the MeshRenderers use, all
    // their entities instances of it (in the order the world keeps them); and each SkinnedMeshRenderer3D's model in its
    // animator's pose (draw_skinned, one range of matrices for all its parts). A ModelRenderer3D's parts join the
    // MeshRenderers' draws, those that hang from an animated node placed by its animator's pose of them.
    void collect(Scene3D& scene);
    // Release reusable mesh/material buckets after bulk removal. ECS-owned
    // handles remain intact; the next collect rebuilds active buckets.
    void clear_collection_cache();
    // Steps every Animator3D by `dt` seconds (call it once per fixed step, or per frame): binds it to its entity's
    // model, runs its state machine (one transition a step), crossfades, blends, advances the clips and layers, and
    // moves the entity by root motion if asked. Deterministic: the same dt, parameters and clips give the same poses,
    // byte for byte.
    void animate(f32 dt);
    // Solves every animator's IK constraints against the current WorldTransform3Ds (call after propagate_transforms())
    // and fills `ik_pose` / `ik_world`. collect() does this itself; call it earlier only to read an IK'd joint before
    // drawing. Calling it more than once a frame is harmless (the smoothing advances only once per animate()), it just
    // costs again.
    void apply_ik();
    // Time of day (render3d/timeofday3d.hpp): a TimeOfDay3D component on any entity is a game clock.
    // advance_time_of_day steps each by `dt` real seconds (kin::advance: `speed` game seconds a second, hours wrapping
    // into days); apply_time_of_day puts the first one's sun, moonlight, ambient colours, sky moon and fog colour into
    // `scene` (kin::apply), and says whether there was one. Call the first once a frame (or fixed step) and the second
    // before render().
    void advance_time_of_day(f32 dt);
    bool apply_time_of_day(Scene3D& scene) const;
    // Particles and weather (render3d/particles3d.hpp, P3c). simulate_particles takes `dt` seconds for every
    // ParticleEmitter3D (each is placed at its entity's WorldTransform3D, so call propagate_transforms() first; `wind`:
    // null for still air); call it once a fixed step or frame. A Weather3D component on any entity is the game's
    // weather: advance_weather steps its clock by `dt` seconds, and apply_weather puts the first one into
    // `scene.weather`, dims `scene.lights` and thickens `scene.fog` by it from `base_fog` (call it after
    // apply_time_of_day; it says whether there was one). collect() draws the emitters' particles.
    void simulate_particles(f64 dt, const Wind3D* wind = nullptr);
    void advance_weather(f64 dt);
    bool apply_weather(Scene3D& scene, const Fog3D& base_fog) const;

private:
    // One animator's constraints over `base` (the clips' pose); `world` is its entity's matrix.
    void solve_ik(Animator3D& animator, const Skeleton3D& skeleton, const Pose3D& base, const Mat4f& world,
                  const flecs::world& ecs, u64 epoch);
    // The clips' pose with the constraints solved and the override applied, in animator.ik_pose / ik_world.
    void solve_pose(Animator3D& animator, const Skeleton3D& skeleton, const Pose3D& base, const Mat4f& world,
                    const flecs::world& ecs, u64 epoch);
    void prepare_joint_attachments();
    std::optional<Mat4f> draw_placement(flecs::entity entity, const WorldTransform3D& world,
                                        const RenderTransform3D* presentation) const;
    bool attachment_pose_prepared(EcsId entity) const;
    EcsWorld* _ecs = nullptr; // checked scene targets need the owning registry
    flecs::query<JointAttachment3D> _attachments;
    std::array<EcsId, max_joint_attachments3d> _attachment_poses{};
    std::size_t _attachment_pose_count = 0, _active_attachments = 0;
    u64 _attachment_frame = 0;
    flecs::observer _transform_observer;
    flecs::query<WorldTransform3D, const Transform3D, const WorldTransform3D> _transforms;
    flecs::query<const WorldTransform3D, const MeshRenderer3D, const RenderTransform3D*> _meshes;
    flecs::query<const WorldTransform3D, const ModelRenderer3D, const RenderTransform3D*, Animator3D*>
        _models; // the animator is optional
    flecs::query<const SkinnedMeshRenderer3D*, const ModelRenderer3D*, Animator3D, Transform3D*>
        _animators; // optional renderers/transform: clear stale events even after renderer removal
    flecs::query<const WorldTransform3D, const SkinnedMeshRenderer3D, Animator3D*, const RenderTransform3D*>
        _skinned; // the animator is optional
    flecs::query<TimeOfDay3D> _time_of_day;
    flecs::query<const WorldTransform3D, const PointLight3D, const RenderTransform3D*> _point_lights;
    flecs::query<const WorldTransform3D, const SpotLight3D, const RenderTransform3D*> _spot_lights;
    flecs::query<const WorldTransform3D, const ParticleEmitter3D> _emitters;
    flecs::query<Weather3D> _weather;
    std::vector<Mat4f> _pose_scratch; // collect()'s matrices of one entity, reused
    Pose3D _rest_pose;                // a skeleton's rest pose for a model with no animator, reused
    struct Bucket {
        Mesh3D mesh;
        Material3D material;
        std::vector<MeshInstance3D> instances;
        InstanceBatch3D batch; // bounded automatic retention for substantial opaque/masked buckets
        InstanceBatch3D spare; // another live collected scene, or the previous count
    };
    std::vector<Bucket> _buckets; // kept between frames, emptied each collect
    std::unordered_map<const IMeshBackend3D*, std::vector<std::size_t>> _bucket_lookup;
    bool _bucket_lookup_dirty = false;
    u64 _epoch =
        0; // advanced by animate() and propagate_transforms(): what the game's ground query said is good until it moves
};

} // namespace kin
