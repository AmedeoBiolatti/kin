#include <kin/renderer/sprite_sheet.hpp>

#include <algorithm>

namespace kin {

SpriteSheet::SpriteSheet(Texture texture, SpriteSheetGrid grid)
    : _texture(std::move(texture)),
      _grid(grid) {
}

bool SpriteSheet::valid() const {
    return _texture.valid() && _grid.tile_w > 0 && _grid.tile_h > 0;
}

i32 SpriteSheet::cols() const {
    if (!valid()) {
        return 0;
    }

    const i32 available = _texture.size().x - _grid.margin * 2 + _grid.spacing;
    const i32 stride = _grid.tile_w + _grid.spacing;
    return std::max(0, available / stride);
}

i32 SpriteSheet::rows() const {
    if (!valid()) {
        return 0;
    }

    const i32 available = _texture.size().y - _grid.margin * 2 + _grid.spacing;
    const i32 stride = _grid.tile_h + _grid.spacing;
    return std::max(0, available / stride);
}

Sprite SpriteSheet::sprite(i32 col, i32 row) const {
    if (!valid() || col < 0 || row < 0 || col >= cols() || row >= rows()) {
        return {};
    }

    const i32 x = _grid.margin + col * (_grid.tile_w + _grid.spacing);
    const i32 y = _grid.margin + row * (_grid.tile_h + _grid.spacing);
    return Sprite{
        .texture = _texture,
        .source = {
            static_cast<f32>(x),
            static_cast<f32>(y),
            static_cast<f32>(_grid.tile_w),
            static_cast<f32>(_grid.tile_h),
        },
    };
}

Sprite SpriteSheet::sprite(i32 index) const {
    const i32 column_count = cols();
    if (column_count <= 0 || index < 0) {
        return {};
    }

    return sprite(index % column_count, index / column_count);
}

} // namespace kin
