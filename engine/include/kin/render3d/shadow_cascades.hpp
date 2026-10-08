#pragma once

// Cascaded shadow maps' maths (docs/3d_plan.md, G2a), apart from the GPU so
// that it can be tested: where the view's depth range is cut, and for each
// slice an orthographic view from the sun that holds it.

#include <kin/core/math3d.hpp>
#include <kin/core/types.hpp>
#include <kin/render3d/camera3d.hpp>
#include <kin/render3d/mesh.hpp>

#include <array>

namespace kin {

constexpr u32 max_shadow_cascades = 4;

// One cascade: the slice of the camera's view it covers, and the sun's view of it.
struct ShadowCascade {
    Mat4f view_projection{}; // world to the cascade's clip space (reverse Z, as the camera's)
    f32 slice_near = 0.0f;   // the view depths it covers: from the end of the one before, less its blend
    f32 slice_far = 0.0f;
    f32 blend_from = 0.0f;   // from here to slice_far it fades into the next cascade
    f32 texel_world = 0.0f;  // a texel's side in world units (the larger of its two)
    f32 depth_range = 1.0f;  // world units from the near plane to the far one
};

struct ShadowCascades {
    u32 count = 0; // 0: none (no area, no sun)
    std::array<ShadowCascade, max_shadow_cascades> cascade{};
};

// The sun's view over `area` and what holds it, for a shadow map: the area's
// corners in the view (lo, hi) and the depth range that spans them with a margin.
struct SunBox {
    Mat4f light_view{};
    Vec3f lo{}, hi{};
    f32 z_near = 0.01f, z_far = 1.0f;
};

// `to_sun`: unit, toward the sun. Both the single shadow map and the cascades
// use it, so that their boxes are the same.
SunBox fit_sun_box(const Bounds3& area, Vec3f to_sun);

// The depths where `count` cascades end, between `near` and `far`: a blend
// of the logarithmic split (a share of the view for each doubling of the
// distance: the resolution a pixel needs) and the linear one (for when the
// log one would give the first cascade nothing to do). `lambda`: 1 is
// the log split. Returns count + 1 values, from `near` to `far`.
std::array<f32, max_shadow_cascades + 1> cascade_splits(f32 near, f32 far, u32 count, f32 lambda = 0.6f);

// The cascades for `camera` seen `aspect` wide over high, lit from `to_sun`
// (unit, toward the sun), reaching `distance` from the camera (clamped to its
// far plane) over `resolution` texels a side. Each cascade's box is the
// bounding sphere of its slice of the view, so that it keeps its size
// as the camera turns, snapped to whole texels in the sun's space so that
// edges don't shimmer as it moves. `area` limits it, and is what the
// depth range spans: whatever casts into the slice from the sun's side.
ShadowCascades fit_shadow_cascades(const Camera3D& camera, f32 aspect, Vec3f to_sun, const Bounds3& area, u32 cascades,
                                   f32 distance, u32 resolution);

// The view of `camera` cut to the depths [near, far], for the cascade's slice.
Camera3D slice_of(const Camera3D& camera, f32 near, f32 far);

} // namespace kin
