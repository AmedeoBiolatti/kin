#include <kin/renderer/render_view.hpp>

#include <algorithm>

namespace kin {

void Camera2D::set_viewport(Vec2f size) {
    viewport = size;
}

Vec2f Camera2D::effective_offset() const {
    return {offset.x + shake_offset.x, offset.y + shake_offset.y};
}

Vec2f Camera2D::world_to_screen(Vec2f world) const {
    const Vec2f camera = effective_offset();
    return {world.x - camera.x, world.y - camera.y};
}

Vec2f Camera2D::screen_to_world(Vec2f screen) const {
    const Vec2f camera = effective_offset();
    return {screen.x + camera.x, screen.y + camera.y};
}

Rectf Camera2D::visible_rect(f32 padding) const {
    const Vec2f camera = effective_offset();
    return {
        camera.x - padding,
        camera.y - padding,
        viewport.x + padding * 2.0f,
        viewport.y + padding * 2.0f,
    };
}

bool Camera2D::visible(Rectf rect, f32 padding) const {
    const Rectf view = visible_rect(padding);
    return rect.x + rect.w >= view.x &&
           rect.y + rect.h >= view.y &&
           rect.x <= view.x + view.w &&
           rect.y <= view.y + view.h;
}

void Camera2D::add_shake(f32 strength, f32 duration, RngKey seed) {
    _shake_strength = std::max(0.0f, strength);
    _shake_duration = std::max(0.0f, duration);
    _shake_remaining = _shake_duration;
    _shake_key = seed;
}

void Camera2D::update(f32 dt) {
    if (_shake_remaining <= 0.0f || _shake_strength <= 0.0f) {
        shake_offset = {};
        _shake_remaining = 0.0f;
        return;
    }
    _shake_remaining = std::max(0.0f, _shake_remaining - dt);
    const f32 t = _shake_duration > 0.0f ? _shake_remaining / _shake_duration : 0.0f;
    auto [next, x_key] = split(_shake_key);
    auto [next_2, y_key] = split(next);
    _shake_key = next_2;
    const f32 strength = _shake_strength * t;
    shake_offset = {
        rng_f32(x_key, -strength, strength),
        rng_f32(y_key, -strength, strength),
    };
}

Rectf render_view_visible_rect(const RenderView& view) {
    if (view.camera) {
        return view.camera->visible_rect(view.cull_padding);
    }
    if (view.cull_rect.w > 0.0f && view.cull_rect.h > 0.0f) {
        return {
            view.cull_rect.x - view.cull_padding,
            view.cull_rect.y - view.cull_padding,
            view.cull_rect.w + view.cull_padding * 2.0f,
            view.cull_rect.h + view.cull_padding * 2.0f,
        };
    }
    return {};
}

bool render_view_visible(const RenderView& view, Rectf bounds) {
    if (!view.culling_enabled) {
        return true;
    }
    const Rectf visible = render_view_visible_rect(view);
    if (visible.w <= 0.0f || visible.h <= 0.0f) {
        return true;
    }
    return bounds.x + bounds.w >= visible.x &&
           bounds.y + bounds.h >= visible.y &&
           bounds.x <= visible.x + visible.w &&
           bounds.y <= visible.y + visible.h;
}

} // namespace kin
