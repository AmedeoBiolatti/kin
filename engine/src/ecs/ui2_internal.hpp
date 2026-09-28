#pragma once

#include <kin/ui2/layout.hpp>

namespace kin::ecs_ui2_detail {

inline bool size_axis_equal(const ui2::SizeAxis& lhs, const ui2::SizeAxis& rhs) {
    return lhs.mode == rhs.mode && lhs.value == rhs.value && lhs.min == rhs.min && lhs.max == rhs.max;
}

inline bool padding_equal(const ui2::UiPadding& lhs, const ui2::UiPadding& rhs) {
    return lhs.left == rhs.left && lhs.top == rhs.top && lhs.right == rhs.right && lhs.bottom == rhs.bottom;
}

inline bool layout_style_equal(const ui2::LayoutStyle& lhs, const ui2::LayoutStyle& rhs) {
    return size_axis_equal(lhs.width, rhs.width) &&
           size_axis_equal(lhs.height, rhs.height) &&
           padding_equal(lhs.padding, rhs.padding) &&
           padding_equal(lhs.margin, rhs.margin) &&
           lhs.spacing == rhs.spacing &&
           lhs.line_spacing == rhs.line_spacing &&
           lhs.axis == rhs.axis &&
           lhs.main == rhs.main &&
           lhs.cross == rhs.cross &&
           lhs.line_cross == rhs.line_cross &&
           lhs.wrap == rhs.wrap &&
           lhs.grid_columns == rhs.grid_columns &&
           lhs.overlay == rhs.overlay &&
           lhs.anchor == rhs.anchor &&
           lhs.anchor_offset == rhs.anchor_offset &&
           lhs.draw_surface == rhs.draw_surface &&
           lhs.layer == rhs.layer;
}

} // namespace kin::ecs_ui2_detail
