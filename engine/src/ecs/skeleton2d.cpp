#include <algorithm>
#include <cmath>
#include <kin/ecs/assets2d.hpp>
#include <kin/ecs/skeleton2d.hpp>
#include <stdexcept>

namespace kin {
void register_skeleton2d_components(EcsWorld& w) {
    if (w.components().find("SkeletonRenderer2D"))
        return;
    w.components()
        .native<SkeletonRenderer2D>("SkeletonRenderer2D")
        .field("asset", &SkeletonRenderer2D::asset)
        .asset_type("Skeleton2D")
        .field("tint", &SkeletonRenderer2D::tint)
        .field("layer", &SkeletonRenderer2D::layer)
        .field("order", &SkeletonRenderer2D::order)
        .field("visible", &SkeletonRenderer2D::visible)
        .field("y_sort", &SkeletonRenderer2D::y_sort);
    w.components()
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
    w.components()
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
    w.components()
        .native<SkeletonIK2D>("SkeletonIK2D")
        .field("root", &SkeletonIK2D::root)
        .field("tip", &SkeletonIK2D::tip)
        .field("target", &SkeletonIK2D::target)
        .field("bend", &SkeletonIK2D::bend)
        .field("weight", &SkeletonIK2D::weight)
        .field("enabled", &SkeletonIK2D::enabled);
    w.components()
        .native<SkeletonAttachment2D>("SkeletonAttachment2D")
        .field("attachment", &SkeletonAttachment2D::attachment)
        .field("offset", &SkeletonAttachment2D::offset)
        .field("rotation", &SkeletonAttachment2D::rotation);
}
void bind_skeletons2d(EcsWorld& w, Assets2D& assets) {
    std::vector<flecs::entity> entities;
    w.raw().each([&](flecs::entity e, const SkeletonRenderer2D&) { entities.push_back(e); });
    for (auto raw : entities) {
        EcsEntity e{raw};
        const auto old = *e.get<SkeletonRenderer2D>();
        const auto rig = old.asset.id.empty() ? old.skeleton : assets.skeleton2d(old.asset.id);
        if (!rig)
            continue; // a broken hot reload retains the last usable rig
        const auto texture = rig->texture_asset.empty() ? old.texture : assets.texture(rig->texture_asset);
        if (rig != old.skeleton || texture != old.texture || old.pose.local.size() != rig->bones.size()) {
            auto* r = e.get_mut<SkeletonRenderer2D>();
            if (rig != old.skeleton || r->pose.local.size() != rig->bones.size())
                r->pose.reset(*rig);
            r->skeleton = rig;
            r->texture = texture;
            assets.changed();
        }
    }
}
void update_skeletons2d(EcsWorld& w, f32 dt) {
    if (!std::isfinite(dt) || dt < 0)
        throw std::invalid_argument("invalid skeletal animation dt");
    std::vector<flecs::entity> entities;
    w.raw().each([&](flecs::entity e, const SkeletonRenderer2D&) { entities.push_back(e); });
    for (auto raw : entities) {
        EcsEntity e{raw};
        auto* r = e.get_mut<SkeletonRenderer2D>();
        if (!r->skeleton)
            continue;
        if (r->pose.local.size() != r->skeleton->bones.size())
            r->pose.reset(*r->skeleton);
        // Acquire owned components before retaining any component pointer.
        if (e.has<SkeletonAnimator2D>())
            e.get_mut<SkeletonAnimator2D>();
        if (e.has<SkeletonLayer2D>())
            e.get_mut<SkeletonLayer2D>();
        r = e.get_mut<SkeletonRenderer2D>();
        if (auto* a = e.get_mut<SkeletonAnimator2D>()) {
            if (a->current_rig != r->skeleton) {
                a->current_rig = r->skeleton;
                a->current.clear();
                a->fade_from = {};
                a->base_pose = {};
                a->fade_elapsed = 0;
            }
            const auto selection = a->blend_space.empty() ? a->animation : "@space:" + a->blend_space;
            if (selection != a->current) {
                a->fade_from = a->current.empty() ? SkeletonPose2D{} : a->base_pose;
                a->fade_elapsed = 0;
                a->current = selection;
                a->time = 0;
                a->finished = false;
            }
            const auto clip = r->skeleton->clips.find(a->animation);
            const bool space = !a->blend_space.empty() && r->skeleton->blend_spaces.contains(a->blend_space) &&
                               std::isfinite(a->blend_cycle_seconds) && a->blend_cycle_seconds > 0;
            if ((space || clip != r->skeleton->clips.end()) && std::isfinite(a->speed) && std::isfinite(a->time)) {
                if (a->playing)
                    a->time += dt * a->speed;
                const auto duration = space ? a->blend_cycle_seconds : clip->second.duration;
                a->finished = !a->loop && (a->speed >= 0 ? a->time >= duration : a->time <= 0);
                if (a->loop) {
                    a->time = std::fmod(a->time, duration);
                    if (a->time < 0)
                        a->time += duration;
                } else
                    a->time = std::clamp(a->time, 0.0f, duration);
                if (space)
                    sample_skeleton_blend_space2d(*r->skeleton, a->blend_space, a->blend_position, a->time / duration,
                                                  a->loop, r->pose);
                else
                    sample_skeleton2d(*r->skeleton, a->animation, a->time, a->loop, r->pose);
                if (!a->fade_from.local.empty() && std::isfinite(a->fade_seconds) && a->fade_seconds > 0) {
                    if (a->playing)
                        a->fade_elapsed += dt;
                    blend_skeleton_poses2d(*r->skeleton, a->fade_from, r->pose, a->fade_elapsed / a->fade_seconds,
                                           r->pose);
                    if (a->fade_elapsed >= a->fade_seconds)
                        a->fade_from = {};
                } else
                    a->fade_from = {};
                a->base_pose = r->pose;
            }
        }
        if (auto* layer = e.get_mut<SkeletonLayer2D>(); layer && !layer->animation.empty() &&
                                                        std::isfinite(layer->speed) && std::isfinite(layer->time) &&
                                                        std::isfinite(layer->weight)) {
            if (!e.has<SkeletonAnimator2D>())
                r->pose.reset(*r->skeleton);
            if (layer->current != layer->animation) {
                layer->current = layer->animation;
                layer->time = 0;
            }
            if (layer->playing)
                layer->time += dt * layer->speed;
            if (const auto clip = r->skeleton->clips.find(layer->animation); clip != r->skeleton->clips.end()) {
                if (layer->loop) {
                    layer->time = std::fmod(layer->time, clip->second.duration);
                    if (layer->time < 0)
                        layer->time += clip->second.duration;
                } else
                    layer->time = std::clamp(layer->time, 0.0f, clip->second.duration);
                SkeletonPose2D pose;
                if (sample_skeleton2d(*r->skeleton, layer->animation, layer->time, layer->loop, pose)) {
                    std::vector<u8> mask(r->skeleton->bones.size(), layer->bone.empty() ? 1 : 0);
                    for (std::size_t i = 0; i < mask.size(); ++i) {
                        const auto parent = r->skeleton->bones[i].parent;
                        if (r->skeleton->bones[i].name == layer->bone ||
                            (layer->include_children && parent >= 0 && mask[parent]))
                            mask[i] = 1;
                    }
                    layer_skeleton2d(*r->skeleton, pose, layer->weight, mask, layer->additive, r->pose);
                }
            }
        }
        if (const auto* ik = e.get<SkeletonIK2D>(); ik && ik->enabled) {
            const auto& bones = r->skeleton->bones;
            const auto root = std::ranges::find(bones, ik->root, &Bone2D::name),
                       tip = std::ranges::find(bones, ik->tip, &Bone2D::name);
            if (root != bones.end() && tip != bones.end()) {
                const auto t = current_world_transform(e);
                const auto target = Affine2::trs(t.pos, t.rotation, t.scale).inverse().apply(ik->target);
                solve_two_bone_ik2d(*r->skeleton, r->pose, static_cast<u32>(root - bones.begin()),
                                    static_cast<u32>(tip - bones.begin()), target, ik->bend, ik->weight);
            }
        }
    }
    std::vector<std::pair<flecs::entity, Transform2D>> attachments;
    w.raw().each([&](flecs::entity e, const SkeletonAttachment2D& a) {
        const auto parent = e.parent();
        const auto* r = parent ? parent.get<SkeletonRenderer2D>() : nullptr;
        if (!r || !r->skeleton)
            return;
        const auto bone = skeleton_attachment2d(*r->skeleton, r->pose, a.attachment);
        if (!bone)
            return;
        const auto m = *bone * Affine2::trs(a.offset, a.rotation);
        attachments.push_back(
            {e, Transform2D{.pos = m.apply({}),
                            .rotation = m.rotation_degrees(),
                            .scale = {std::hypot(m.a, m.b), std::copysign(std::hypot(m.c, m.d), m.determinant())}}});
    });
    for (const auto& [entity, t] : attachments)
        EcsEntity{entity}.set(t);
}
bool submit_skeleton2d(RenderQueue& queue, flecs::entity, const WorldTransform& t, const SkeletonRenderer2D& r,
                       const RenderView* view) {
    if (!r.visible || !r.skeleton || !r.texture || r.pose.skin.size() != r.skeleton->bones.size())
        return false;
    auto vertices = deform_skeleton2d(*r.skeleton, r.pose, r.tint);
    if (vertices.empty() || r.skeleton->indices.empty())
        return false;
    const auto to = Affine2::trs(t.pos, t.rotation, t.scale);
    Rectf bounds{};
    f32 x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    bool first = true;
    for (auto& v : vertices) {
        v.position = to.apply(v.position);
        if (first) {
            x0 = x1 = v.position.x;
            y0 = y1 = v.position.y;
            first = false;
        }
        x0 = std::min(x0, v.position.x);
        x1 = std::max(x1, v.position.x);
        y0 = std::min(y0, v.position.y);
        y1 = std::max(y1, v.position.y);
    }
    bounds = {x0, y0, x1 - x0, y1 - y0};
    if (view && !render_view_visible(*view, bounds))
        return false;
    const auto rig = r.skeleton;
    const auto texture = r.texture;
    const auto pass = r.layer >= layer_value(RenderLayer::Debug)     ? render_pass_mask::debug
                      : r.layer >= layer_value(RenderLayer::UI)      ? render_pass_mask::ui
                      : r.layer >= layer_value(RenderLayer::Effects) ? render_pass_mask::effects
                                                                     : render_pass_mask::world;
    queue.custom(
        {.layer = r.layer, .order = r.order, .y = t.pos.y, .use_y = r.y_sort, .pass_mask = pass},
        [vertices = std::move(vertices), rig, texture](Renderer2D& renderer) {
            renderer.draw_textured_geometry(texture, vertices, rig->indices);
        },
        "Skeleton2D");
    return true;
}
} // namespace kin
