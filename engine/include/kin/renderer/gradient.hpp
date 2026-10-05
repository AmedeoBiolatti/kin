#pragma once

#include <kin/renderer/color.hpp>

#include <optional>

namespace kin {

enum class GradientDirection {
    Vertical,   // start at top edge, end at bottom edge
    Horizontal, // start at left edge, end at right edge
};

// Two-color linear gradient. N-stop gradients are a later, source-compatible
// extension; two colors cover the common vertical/horizontal case.
struct Gradient {
    Color start;
    Color end;
    GradientDirection direction = GradientDirection::Vertical;
    // Where the colours mix (ColorMix): unset, as the pipeline blends (sRGB
    // values in a Gamma one, linear light in a Linear one). OKLab keeps the
    // middle as bright and saturated as the ends look.
    std::optional<ColorMix> mix;
};

} // namespace kin
