#pragma once

#include <kin/core/types.hpp>
#include <kin/renderer/sprite.hpp>
#include <kin/renderer/texture.hpp>

namespace kin {

struct SpriteSheetGrid {
    i32 tile_w = 0;
    i32 tile_h = 0;
    i32 spacing = 0;
    i32 margin = 0;
};

class SpriteSheet {
public:
    SpriteSheet() = default;
    SpriteSheet(Texture texture, SpriteSheetGrid grid);

    bool valid() const;
    i32 cols() const;
    i32 rows() const;
    Vec2i tile_size() const { return {_grid.tile_w, _grid.tile_h}; }

    Sprite sprite(i32 col, i32 row) const;
    Sprite sprite(i32 index) const;

private:
    Texture _texture;
    SpriteSheetGrid _grid{};
};

} // namespace kin
