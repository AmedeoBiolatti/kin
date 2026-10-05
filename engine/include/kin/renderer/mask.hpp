#pragma once

// Masks (Renderer2D::push_mask): what is drawn while one is open shows only
// where the mask is. A mask is anything drawn (shapes, sprites, text, a render
// target), read per pixel as a coverage from 0 to 1.

#include <kin/core/types.hpp>

namespace kin {

// How the coverage cuts what is drawn.
enum class MaskMode : u8 {
    Alpha,   // times the coverage: soft and anti-aliased edges, fades
    Stencil, // all or nothing: where the coverage reaches `threshold`
};

// Which of the mask's channels is its coverage.
enum class MaskSource : u8 {
    Alpha,     // its alpha: a shape, a sprite's silhouette
    Luminance, // its brightness (times alpha): white shows, black hides (SVG's masks)
};

struct MaskOptions {
    MaskMode mode = MaskMode::Alpha;
    MaskSource source = MaskSource::Alpha;
    f32 threshold = 0.5f; // Stencil: the least coverage that shows
    bool invert = false;  // show what is outside the mask instead
    // Of the current target's pixels, each way, for the mask and what it
    // masks: below 1 for soft masks over soft content (fog, light), cheaper.
    f32 resolution = 1.0f;
};

// The coverage `options` read from a mask's pixel (premultiplied, 0 to 1).
constexpr f32 mask_coverage(const MaskOptions& options, f32 r, f32 g, f32 b, f32 a) {
    f32 k = options.source == MaskSource::Luminance ? 0.2126f * r + 0.7152f * g + 0.0722f * b : a;
    if (options.mode == MaskMode::Stencil) {
        k = k >= options.threshold ? 1.0f : 0.0f;
    }
    return options.invert ? 1.0f - k : k;
}

} // namespace kin
