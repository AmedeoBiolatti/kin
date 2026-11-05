#pragma once

// 2D skeletons on entities (kin/anim/skeleton2d.hpp; docs/skeleton_2d.md): a
// rig drawn as its deformed mesh, a clip or blend space playing on it (with
// crossfades), a layer clip over part of it, two-bone IK, and child entities
// following its attachments.
//
//     actor.set(kin::SkeletonRenderer2D{.asset = {"rigs/hero.kinskel"}});
//     actor.set(kin::SkeletonAnimator2D{.animation = "walk"});
//     // each step, after Assets2D::bind
//     kin::update_skeletons2d(world, dt);

#include <kin/anim/skeleton2d.hpp>
#include <kin/ecs/render.hpp>

#include <memory>
#include <string>

namespace kin {

class Assets2D;

struct SkeletonRenderer2D {
    ComponentAssetRef asset; // .kinskel
    std::shared_ptr<const SkeletonAsset2D> skeleton;
    Texture texture;
    SkeletonPose2D pose; // private per instance, including prefab copies
    Color tint = colors::white;
    i32 layer = layer_value(RenderLayer::World);
    i32 order = 0;
    bool visible = true;
    bool y_sort = false;
};

struct SkeletonAnimator2D {
    std::string animation;
    f32 speed = 1;
    f32 time = 0;
    bool playing = true;
    bool loop = true;
    bool finished = false;
    std::string current;
    f32 fade_seconds = 0;
    std::string blend_space;
    Vec2f blend_position{};
    f32 blend_cycle_seconds = 1;
    SkeletonPose2D fade_from;
    SkeletonPose2D base_pose; // excludes layers and IK, including during interrupted fades
    f32 fade_elapsed = 0;
    std::shared_ptr<const SkeletonAsset2D> current_rig;
};

struct SkeletonLayer2D {
    std::string animation;
    std::string bone; // empty: the whole rig
    f32 speed = 1;
    f32 time = 0;
    f32 weight = 1;
    bool playing = true;
    bool loop = true;
    bool additive = false;
    bool include_children = true;
    std::string current;
};

struct SkeletonIK2D {
    std::string root;
    std::string tip;
    Vec2f target{}; // world coordinates
    f32 bend = 1;
    f32 weight = 1;
    bool enabled = true;
};

// Put on a direct child of a SkeletonRenderer2D entity. Uses the rig's named
// attachment; the child's Transform2D follows it (its own children follow too).
struct SkeletonAttachment2D {
    std::string attachment;
    Vec2f offset{};
    f32 rotation = 0;
};

void register_skeleton2d_components(EcsWorld& world);
void bind_skeletons2d(EcsWorld& world, Assets2D& assets);
// Call once per simulation step after asset binding, before render collection.
// Animator fields can also be driven by Kin property animations/state machines.
void update_skeletons2d(EcsWorld& world, f32 dt);
bool submit_skeleton2d(RenderQueue& queue, flecs::entity entity, const WorldTransform& transform,
                       const SkeletonRenderer2D& renderer, const RenderView* view = nullptr);

} // namespace kin
