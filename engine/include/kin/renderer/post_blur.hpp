#pragma once

#include <kin/renderer/render_target.hpp>

namespace kin {

class Renderer2D;

struct BlurParams {
    i32 passes = 2;    // down/up-sample iterations; higher = wider, softer
    f32 radius = 1.0f; // sample-offset scale per pass
};

// Blurs `src` (dual-filter / Kawase) and returns the result as a pool-backed
// target. The returned PooledTarget keeps the result checked out of the renderer's
// render-target pool until the caller drops it, so a recycled target can't be
// drawn after reuse.
//
// Preconditions / conventions:
//   - `src` must be Linear-sampled (ScaleMode::Linear); the chain relies on
//     bilinear averaging. If `src` was created Nearest, call
//     r.set_scale_mode(src.texture(), ScaleMode::Linear) first.
//   - Runs in PREMULTIPLIED alpha space (see render_target.hpp).
//   - Requires r.capabilities().render_targets; on a backend without render
//     targets the returned PooledTarget is empty (callers must degrade on
//     !valid()).
//
// NOTE: this phase declares the signature only; the implementation
// (post_blur.cpp, the Kawase chain) is deferred to a later phase.
class PooledTarget; // defined in renderer2d.hpp

PooledTarget blur(Renderer2D& r, const RenderTarget& src, BlurParams params);

} // namespace kin
