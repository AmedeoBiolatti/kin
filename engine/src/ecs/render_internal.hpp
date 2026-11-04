#pragma once

// How entity renderers are placed and queued, shared by render.cpp's
// renderers and those with files of their own (normal_sprite2d.cpp,
// skeleton2d.cpp).

#include <kin/core/affine.hpp>
#include <kin/ecs/render.hpp>

#include <algorithm>
#include <cmath>

namespace kin::render_detail {

// A point in the entity's own space, in the world.
inline Vec2f to_world(const WorldTransform& t, Vec2f local) {
    if (t.rotation == 0.0f && t.scale == Vec2f{1.0f, 1.0f}) {
        return {t.pos.x + local.x, t.pos.y + local.y};
    }
    const Vec2f turned = Affine2::trs({}, t.rotation, t.scale).apply_vector(local);
    return {t.pos.x + turned.x, t.pos.y + turned.y};
}

// The flags, mirrored again on each axis the transform scales negatively.
inline Flip flip_for(const WorldTransform& t, bool flip_x, bool flip_y) {
    return flip_of(flip_x != (t.scale.x < 0.0f), flip_y != (t.scale.y < 0.0f));
}

// The pivot as a fraction of the mirrored image: the same point of the world.
inline Vec2f mirrored_pivot(Vec2f pivot, Flip flip) {
    return {flips_x(flip) ? 1.0f - pivot.x : pivot.x, flips_y(flip) ? 1.0f - pivot.y : pivot.y};
}

// What a rectangle turned by `rotation` about `pivot` (a fraction of it) may cover.
inline Rectf turned_bounds(Rectf rect, f32 rotation, Vec2f pivot) {
    if (rotation == 0.0f) {
        return rect;
    }
    const Vec2f p{rect.x + rect.w * pivot.x, rect.y + rect.h * pivot.y};
    const f32 dx = std::max(std::abs(p.x - rect.x), std::abs(rect.x + rect.w - p.x));
    const f32 dy = std::max(std::abs(p.y - rect.y), std::abs(rect.y + rect.h - p.y));
    const f32 r = std::sqrt(dx * dx + dy * dy);
    return {p.x - r, p.y - r, 2 * r, 2 * r};
}

inline u64 pass_mask_for_layer(i32 layer) {
    if (layer >= layer_value(RenderLayer::Debug)) {
        return render_pass_mask::debug;
    }
    if (layer >= layer_value(RenderLayer::UI)) {
        return render_pass_mask::ui;
    }
    if (layer >= layer_value(RenderLayer::Effects)) {
        return render_pass_mask::effects;
    }
    return render_pass_mask::world;
}

inline RenderKey key_for(i32 layer, i32 order, bool y_sort, f32 y) {
    return {
        .layer = layer,
        .order = order,
        .y = y,
        .use_y = y_sort,
        .pass_mask = pass_mask_for_layer(layer),
    };
}

} // namespace kin::render_detail
