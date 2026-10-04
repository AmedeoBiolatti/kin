#pragma once

#include <kin/core/affine.hpp>
#include <kin/core/rng.hpp>
#include <kin/core/types.hpp>

namespace kin {

enum class RenderProjection {
    Screen2D,
    TopDown2D,
    Isometric2D,
    Perspective3D,
};

// `offset` is the world point at the viewport's top-left corner when the camera
// is neither zoomed nor turned; zoom and rotation work about the viewport's
// centre, so the world point there (center()) stays put.
struct Camera2D {
    Vec2f offset{};
    Vec2f viewport{640.0f, 360.0f};
    Vec2f shake_offset{};
    f32 zoom = 1.0f;     // screen units per world unit: 2 draws the world twice as big
    f32 rotation = 0.0f; // degrees, clockwise: the camera turns, so the world turns the other way

    void set_viewport(Vec2f size);
    Vec2f effective_offset() const;
    // The world point at the viewport's centre, and moving the camera to one.
    Vec2f center() const;
    void look_at(Vec2f world);
    // World to screen as a transform: Renderer2D::scoped_transform(camera.view_transform())
    // draws world coordinates through the camera.
    Affine2 view_transform() const;
    // No zoom and no rotation: the camera only moves the world.
    bool translation_only() const { return zoom == 1.0f && rotation == 0.0f; }
    Vec2f world_to_screen(Vec2f world) const;
    Vec2f screen_to_world(Vec2f screen) const;
    // The world rectangle the viewport shows (the box around it, when turned).
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
