#pragma once
#include <kin/anim/skeleton2d.hpp>
#include <kin/ecs/render.hpp>

namespace kin {
class Assets2D;
struct SkeletonRenderer2D {
    ComponentAssetRef asset; // .kinskel
    std::shared_ptr<const SkeletonAsset2D> skeleton;
    Texture texture;
    SkeletonPose2D pose; // private per instance, including prefab copies
    Color tint = colors::white;
    i32 layer = layer_value(RenderLayer::World), order = 0;
    bool visible = true, y_sort = false;
};
struct SkeletonAnimator2D {
    std::string animation;
    f32 speed = 1, time = 0;
    bool playing = true, loop = true, finished = false;
    std::string current;
};
struct SkeletonIK2D {
    std::string root, tip;
    Vec2f target{}; // world coordinates
    f32 bend = 1, weight = 1;
    bool enabled = true;
};
// Put on a direct child of a SkeletonRenderer2D entity. Uses the rig's named
// attachment; the child's Transform2D follows it (its own children follow too).
struct SkeletonAttachment2D {
    std::string attachment;
    Vec2f offset{};
    f32 rotation = 0;
};

void register_skeleton2d_components(EcsWorld&);
void bind_skeletons2d(EcsWorld&, Assets2D&);
// Call once per simulation step after asset binding, before render collection.
// Animator fields can also be driven by Kin property animations/state machines.
void update_skeletons2d(EcsWorld&, f32 dt);
bool submit_skeleton2d(RenderQueue&, flecs::entity, const WorldTransform&, const SkeletonRenderer2D&,
                       const RenderView* view = nullptr);
} // namespace kin
