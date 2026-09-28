#pragma once

#include <kin/core/rng.hpp>
#include <kin/core/types.hpp>

namespace kin {

enum class RenderProjection {
    Screen2D,
    TopDown2D,
    Isometric2D,
    Perspective3D,
};

struct Camera2D {
    Vec2f offset{};
    Vec2f viewport{640.0f, 360.0f};
    Vec2f shake_offset{};

    void set_viewport(Vec2f size);
    Vec2f effective_offset() const;
    Vec2f world_to_screen(Vec2f world) const;
    Vec2f screen_to_world(Vec2f screen) const;
    Rectf visible_rect(f32 padding = 0.0f) const;
    bool visible(Rectf rect, f32 padding = 0.0f) const;
    void add_shake(f32 strength, f32 duration, RngKey seed = make_key(1));
    void update(f32 dt);

private:
    f32 _shake_strength = 0.0f;
    f32 _shake_remaining = 0.0f;
    f32 _shake_duration = 0.0f;
    RngKey _shake_key = make_key(1);
};

struct RenderView {
    RenderProjection projection = RenderProjection::Screen2D;
    Rectf viewport{};
    const Camera2D* camera = nullptr;
    Rectf cull_rect{};
    f32 cull_padding = 0.0f;
    bool culling_enabled = false;
    bool output_space = false;
};

Rectf render_view_visible_rect(const RenderView& view);
bool render_view_visible(const RenderView& view, Rectf bounds);

} // namespace kin
