#pragma once

#include <kin/renderer/color.hpp>

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
};

} // namespace kin
