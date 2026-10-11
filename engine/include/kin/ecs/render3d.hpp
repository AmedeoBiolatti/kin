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

#if !defined(KIN_EXPERIMENTAL_3D)
#error "kin's 3D rendering is experimental: configure kin with -DKIN_EXPERIMENTAL_3D=ON"
#endif

#include <kin/anim/state_machine.hpp>
#include <kin/core/math3d.hpp>
#include <kin/ecs/world.hpp>
#include <kin/render3d/animation3d.hpp>
#include <kin/render3d/mesh.hpp>
#include <kin/render3d/model.hpp>
#include <kin/render3d/scene3d.hpp>

#include <flecs.h>

#include <memory>
#include <string>
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

private:
    flecs::observer _transform_observer;
    flecs::query<WorldTransform3D, const Transform3D, const WorldTransform3D> _transforms;
    flecs::query<const WorldTransform3D, const MeshRenderer> _meshes;
    flecs::query<const SkinnedMeshRenderer, Animator3D, Transform3D*> _animators; // the transform is optional (a pointer)
    flecs::query<const WorldTransform3D, const SkinnedMeshRenderer, const Animator3D*> _skinned; // the animator is optional
    std::vector<Mat4f> _pose_scratch; // collect()'s matrices of one entity, reused
    struct Bucket {
        Mesh mesh;
        Material3D material;
        std::vector<MeshInstance> instances;
    };
    std::vector<Bucket> _buckets; // kept between frames, emptied each collect
};

} // namespace kin
