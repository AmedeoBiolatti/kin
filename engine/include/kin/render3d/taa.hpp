#pragma once

// Temporal anti-aliasing's maths (docs/3d_plan.md, G2d), apart from the GPU so
// that it can be tested: the sub-pixel jitter sequence and how a projection is
// moved by it.

#include <kin/core/math3d.hpp>
#include <kin/core/types.hpp>

namespace kin {

// The most frames a jitter sequence may run before it repeats.
constexpr u32 max_taa_jitter_samples = 16;

// Frame `frame`'s sub-pixel offset, in pixels, in [-0.5, 0.5) on each axis: the
// Halton sequence (base 2 for x, base 3 for y) from its second element on, so
// that no frame sits exactly on the pixel's corner, repeating every `period`
// frames (1 to max_taa_jitter_samples; 8 by default: eight well spread points
// are the most a pixel's history keeps in step with the 0.9 blend). Pure.
Vec2f taa_jitter(u32 frame, u32 period = 8);

// `projection` (or a view-projection) moved by `pixels` on a view `size`
// pixels across: every point lands that far off, whatever its depth (the
// shift is in clip space, scaled by w). +x is right and +y down, as the
// picture's rows run. Pure.
Mat4f jitter_projection(const Mat4f& projection, Vec2f pixels, Vec2i size);

} // namespace kin
