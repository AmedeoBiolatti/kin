#pragma once

#include <kin/renderer/color.hpp>

#include <array>

namespace kin::ui2 {

// A 12-step semantic color scale. Index 0 maps to step 1, index 11 to step 12.
struct ColorScale {
    std::array<Color, 12> steps{};

    constexpr Color operator[](int i) const {
        return steps[static_cast<std::size_t>(i)];
    }
};

} // namespace kin::ui2
