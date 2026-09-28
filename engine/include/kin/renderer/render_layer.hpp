#pragma once

#include <kin/core/types.hpp>

namespace kin {

enum class RenderLayer : i32 {
    Background = -1000,
    TileBack = -500,
    World = 0,
    Actors = 100,
    TileFront = 200,
    Effects = 300,
    UI = 1000,
    Debug = 2000,
};

constexpr i32 layer_value(RenderLayer layer, i32 offset = 0) {
    return static_cast<i32>(layer) + offset;
}

} // namespace kin
