#pragma once

#include <kin/core/types.hpp>
#include <kin/renderer/color.hpp>
#include <kin/renderer/flip.hpp>

namespace kin {

// One quad of a draw_sprites() call: what draw_texture(texture, source, dest,
// tint, rotation, pivot, flip) would draw.
struct SpriteInstance {
    Rectf dest{};
    Rectf source{}; // texture pixels; empty means the whole texture
    Color tint = colors::white;
    f32 rotation = 0.0f;     // degrees, clockwise, about the pivot
    Vec2f pivot{0.5f, 0.5f}; // normalized within dest
    Flip flip = Flip::None;
};

} // namespace kin
