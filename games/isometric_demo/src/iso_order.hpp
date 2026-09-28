#pragma once

#include <kin/core/types.hpp>

namespace isometric_demo {

constexpr kin::i32 BaseContentOffset = 0;
constexpr kin::i32 MovableObjectOffset = 5;

inline kin::i32 cell_order_key(kin::Vec2i cell, kin::i32 map_cols) {
    return (cell.x + cell.y) * map_cols + cell.x;
}

inline kin::i32 render_depth(kin::Vec2i cell, kin::i32 type_offset, kin::i32 map_cols) {
    return cell_order_key(cell, map_cols) * 10 + type_offset;
}

} // namespace isometric_demo
