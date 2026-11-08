#include <kin/ecs/skeleton2d.hpp>

#include <kin/renderer/renderer2d.hpp>

#include "../anim/skeleton2d_internal.hpp"
#include "render_internal.hpp"

#include <kin/ecs/assets2d.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string_view>

namespace kin {
namespace {

using namespace skeleton2d_detail;

constexpr std::string_view space_prefix = "@space:";

// Whether `current` names what the animator plays now (its clip, or its blend space).
bool playing_selection(const SkeletonAnimator2D& animator) {
    if (animator.blend_space.empty()) {
        return animator.current == animator.animation;
    }
    return animator.current.starts_with(space_prefix) &&
           std::string_view{animator.current}.substr(space_prefix.size()) == animator.blend_space;
}

// The animator's clip or blend space sampled into `pose` (local transforms
// only), crossfaded from what it played before; false when it has nothing
// to play.
bool animate(SkeletonAnimator2D& animator, const SkeletonAsset2D& rig, f32 dt, SkeletonPose2D& pose) {
    if (!playing_selection(animator)) {
        animator.fade_from.local = animator.current.empty() ? std::vector<BoneTransform2D>{} : animator.base_pose.local;
        animator.fade_elapsed = 0;
        animator.current =
            animator.blend_space.empty() ? animator.animation : std::string{space_prefix} + animator.blend_space;
        animator.time = 0;
        animator.finished = false;
    }
    const auto clip = rig.clips.find(animator.animation);
    const bool space = !animator.blend_space.empty() && rig.blend_spaces.contains(animator.blend_space) &&
                       std::isfinite(animator.blend_cycle_seconds) && animator.blend_cycle_seconds > 0;
    if ((!space && clip == rig.clips.end()) || !std::isfinite(animator.speed) || !std::isfinite(animator.time)) {
        return false;
    }
    if (animator.playing) {
        animator.time += dt * animator.speed;
    }
    const f32 duration = space ? animator.blend_cycle_seconds : clip->second.duration;
    animator.finished = !animator.loop && (animator.speed >= 0 ? animator.time >= duration : animator.time <= 0);
    animator.time = wrap_clip_time(animator.time, duration, animator.loop);
    if (space) {
        sample_blend_space_local(rig, animator.blend_space, animator.blend_position, animator.time / duration,
                                 animator.loop, pose);
    } else {
        sample_local(rig, animator.animation, animator.time, animator.loop, pose);
    }
    if (!animator.fade_from.local.empty() && std::isfinite(animator.fade_seconds) && animator.fade_seconds > 0) {
        if (animator.playing) {
            animator.fade_elapsed += dt;
        }
        blend_poses_local(rig, animator.fade_from, pose, animator.fade_elapsed / animator.fade_seconds, pose);
        if (animator.fade_elapsed >= animator.fade_seconds) {
            animator.fade_from = {};
        }
    } else {
        animator.fade_from = {};
    }
    animator.base_pose.local = pose.local;
    return true;
}

// The layer's clip over `pose` (local transforms only); false when it has none.
bool apply_layer(SkeletonLayer2D& layer, const SkeletonAsset2D& rig, f32 dt, SkeletonPose2D& pose) {
    if (layer.current != layer.animation) {
        layer.current = layer.animation;
        layer.time = 0;
    }
    if (layer.playing) {
        layer.time += dt * layer.speed;
    }
    const auto clip = rig.clips.find(layer.animation);
    if (clip == rig.clips.end()) {
        return false;
    }
    layer.time = wrap_clip_time(layer.time, clip->second.duration, layer.loop);
    SkeletonPose2D sampled;
    if (!sample_local(rig, layer.animation, layer.time, layer.loop, sampled)) {
        return false;
    }
    // The bone (with its children) or, without one, every bone.
    std::vector<u8> mask(rig.bones.size(), layer.bone.empty() ? 1 : 0);
    for (std::size_t i = 0; i < mask.size(); ++i) {
        const i32 parent = rig.bones[i].parent;
        if (rig.bones[i].name == layer.bone || (layer.include_children && parent >= 0 && mask[parent])) {
            mask[i] = 1;
        }
    }
    return layer_local(rig, sampled, layer.weight, mask, layer.additive, pose);
}

} // namespace

void register_skeleton2d_components(EcsWorld& world) {
    if (world.components().find("SkeletonRenderer2D")) {
        return;
    }
    world.components()
        .native<SkeletonRenderer2D>("SkeletonRenderer2D")
        .field("asset", &SkeletonRenderer2D::asset)
        .asset_type("Skeleton2D")
        .field("tint", &SkeletonRenderer2D::tint)
        .field("layer", &SkeletonRenderer2D::layer)
        .field("order", &SkeletonRenderer2D::order)
        .field("visible", &SkeletonRenderer2D::visible)
        .field("y_sort", &SkeletonRenderer2D::y_sort);
    world.components()
        .native<SkeletonAnimator2D>("SkeletonAnimator2D")
        .field("animation", &SkeletonAnimator2D::animation)
        .field("speed", &SkeletonAnimator2D::speed)
        .field("time", &SkeletonAnimator2D::time)
        .field("playing", &SkeletonAnimator2D::playing)
        .field("loop", &SkeletonAnimator2D::loop)
        .field("finished", &SkeletonAnimator2D::finished)
        .field("fade_seconds", &SkeletonAnimator2D::fade_seconds)
        .field("blend_space", &SkeletonAnimator2D::blend_space)
        .field("blend_position", &SkeletonAnimator2D::blend_position)
        .field("blend_cycle_seconds", &SkeletonAnimator2D::blend_cycle_seconds);
    world.components()
        .native<SkeletonLayer2D>("SkeletonLayer2D")
        .field("animation", &SkeletonLayer2D::animation)
        .field("bone", &SkeletonLayer2D::bone)
        .field("speed", &SkeletonLayer2D::speed)
        .field("time", &SkeletonLayer2D::time)
        .field("weight", &SkeletonLayer2D::weight)
        .field("playing", &SkeletonLayer2D::playing)
        .field("loop", &SkeletonLayer2D::loop)
        .field("additive", &SkeletonLayer2D::additive)
        .field("include_children", &SkeletonLayer2D::include_children);
    world.components()
        .native<SkeletonIK2D>("SkeletonIK2D")
        .field("root", &SkeletonIK2D::root)
        .field("tip", &SkeletonIK2D::tip)
        .field("target", &SkeletonIK2D::target)
        .field("bend", &SkeletonIK2D::bend)
        .field("weight", &SkeletonIK2D::weight)
        .field("enabled", &SkeletonIK2D::enabled);
    world.components()
        .native<SkeletonAttachment2D>("SkeletonAttachment2D")
        .field("attachment", &SkeletonAttachment2D::attachment)
        .field("offset", &SkeletonAttachment2D::offset)
        .field("rotation", &SkeletonAttachment2D::rotation);
}

void bind_skeletons2d(EcsWorld& world, Assets2D& assets) {
    // Read only, so nothing is marked written; those to change, changed after.
    struct Bound {
        flecs::entity entity;
        std::shared_ptr<const SkeletonAsset2D> rig;
        Texture texture;
    };
    std::vector<Bound> rebound;
    world.each([&](flecs::entity entity, const SkeletonRenderer2D& renderer) {
        std::shared_ptr<const SkeletonAsset2D> rig =
            renderer.asset.id.empty() ? renderer.skeleton : assets.skeleton2d(renderer.asset.id);
        if (!rig) {
            return; // a broken hot reload retains the last usable rig
        }
        Texture texture = rig->texture_asset.empty() ? renderer.texture : assets.texture(rig->texture_asset);
        if (rig != renderer.skeleton || !(texture == renderer.texture) ||
            renderer.pose.local.size() != rig->bones.size()) {
            rebound.push_back({entity, std::move(rig), std::move(texture)});
        }
    });
    for (Bound& bound : rebound) {
        EcsEntity owner{bound.entity}; // one from a prefab gets its own copy first
        SkeletonRenderer2D* renderer = owner.get_mut<SkeletonRenderer2D>();
        if (bound.rig != renderer->skeleton || renderer->pose.local.size() != bound.rig->bones.size()) {
            renderer->pose.reset(*bound.rig);
        }
        renderer->skeleton = std::move(bound.rig);
        renderer->texture = std::move(bound.texture);
        owner.modified<SkeletonRenderer2D>();
    }
    if (!rebound.empty()) {
        assets.changed();
    }
}

void update_skeletons2d(EcsWorld& world, f32 dt) {
    if (!std::isfinite(dt) || dt < 0) {
        throw std::invalid_argument("invalid skeletal animation dt");
    }
    std::vector<flecs::entity> entities;
    world.each([&](flecs::entity e, const SkeletonRenderer2D&) { entities.push_back(e); });
    for (const flecs::entity raw : entities) {
        EcsEntity e{raw};
        if (!e.get<SkeletonRenderer2D>()->skeleton) {
            continue;
        }
        // Own each component before keeping a pointer to any (owning one may move the others).
        e.get_mut<SkeletonRenderer2D>();
        if (e.has<SkeletonAnimator2D>()) {
            e.get_mut<SkeletonAnimator2D>();
        }
        if (e.has<SkeletonLayer2D>()) {
            e.get_mut<SkeletonLayer2D>();
        }
        SkeletonRenderer2D* r = e.get_mut<SkeletonRenderer2D>();
        const SkeletonAsset2D& rig = *r->skeleton;
        if (r->pose.local.size() != rig.bones.size()) {
            r->pose.reset(rig);
        }
        // Each step poses local transforms only; the pose is evaluated once, after them.
        bool posed = false;
        SkeletonAnimator2D* animator = e.get_mut<SkeletonAnimator2D>();
        if (animator) {
            if (animator->current_rig != r->skeleton) {
                animator->current_rig = r->skeleton;
                animator->current.clear();
                animator->fade_from = {};
                animator->base_pose = {};
                animator->fade_elapsed = 0;
            }
            posed = animate(*animator, rig, dt, r->pose);
        }
        if (SkeletonLayer2D* layer = e.get_mut<SkeletonLayer2D>();
            layer && !layer->animation.empty() && std::isfinite(layer->speed) && std::isfinite(layer->time) &&
            std::isfinite(layer->weight)) {
            if (!animator) {
                rest_local(rig, r->pose);
                posed = true;
            }
            posed = apply_layer(*layer, rig, dt, r->pose) || posed;
        }
        if (posed) {
            r->pose.evaluate(rig);
        }
        if (const SkeletonIK2D* ik = e.get<SkeletonIK2D>(); ik && ik->enabled) {
            const auto root = std::ranges::find(rig.bones, ik->root, &Bone2D::name);
            const auto tip = std::ranges::find(rig.bones, ik->tip, &Bone2D::name);
            if (root != rig.bones.end() && tip != rig.bones.end()) {
                const WorldTransform t = current_world_transform(e);
                const Vec2f target = Affine2::trs(t.pos, t.rotation, t.scale).inverse().apply(ik->target);
                solve_two_bone_ik2d(rig, r->pose, static_cast<u32>(root - rig.bones.begin()),
                                    static_cast<u32>(tip - rig.bones.begin()), target, ik->bend, ik->weight);
            }
        }
    }
    std::vector<std::pair<flecs::entity, Transform2D>> attachments;
    world.each([&](flecs::entity e, const SkeletonAttachment2D& attached) {
        const flecs::entity parent = e.parent();
        const SkeletonRenderer2D* r = parent ? parent.get<SkeletonRenderer2D>() : nullptr;
        if (!r || !r->skeleton) {
            return;
        }
        const std::optional<Affine2> bone = skeleton_attachment2d(*r->skeleton, r->pose, attached.attachment);
        if (!bone) {
            return;
        }
        const Affine2 m = *bone * Affine2::trs(attached.offset, attached.rotation);
        attachments.push_back(
            {e, Transform2D{.pos = m.apply({}),
                            .rotation = m.rotation_degrees(),
                            .scale = {std::hypot(m.a, m.b), std::copysign(std::hypot(m.c, m.d), m.determinant())}}});
    });
    for (const auto& [entity, t] : attachments) {
        EcsEntity{entity}.set(t);
    }
}

bool submit_skeleton2d(RenderQueue& queue, flecs::entity entity, const WorldTransform& transform,
                       const SkeletonRenderer2D& renderer, const RenderView* view) {
    KIN_DRAW_ENTITY(entity, "SkeletonRenderer2D");
    if (!renderer.visible || !renderer.skeleton || !renderer.texture ||
        renderer.pose.skin.size() != renderer.skeleton->bones.size()) {
        return false;
    }
    std::vector<ShaderVertex> vertices = deform_skeleton2d(*renderer.skeleton, renderer.pose, renderer.tint);
    if (vertices.empty() || renderer.skeleton->indices.empty()) {
        return false;
    }
    const Affine2 to = Affine2::trs(transform.pos, transform.rotation, transform.scale);
    f32 x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const Vec2f p = vertices[i].position = to.apply(vertices[i].position);
        x0 = i ? std::min(x0, p.x) : p.x;
        x1 = i ? std::max(x1, p.x) : p.x;
        y0 = i ? std::min(y0, p.y) : p.y;
        y1 = i ? std::max(y1, p.y) : p.y;
    }
    if (view && !render_view_visible(*view, {x0, y0, x1 - x0, y1 - y0})) {
        return false;
    }
    queue.custom(
        render_detail::key_for(renderer.layer, renderer.order, renderer.y_sort, transform.pos.y),
        [vertices = std::move(vertices), rig = renderer.skeleton, texture = renderer.texture](Renderer2D& target) {
            target.draw_textured_geometry(texture, vertices, rig->indices);
        },
        "Skeleton2D");
    return true;
}

} // namespace kin
