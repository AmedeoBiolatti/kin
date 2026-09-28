#include <kin/renderer/static_texture_layer.hpp>

#include <kin/renderer/renderer2d.hpp>
#include <kin/renderer/sprite_catalog.hpp>

#include <algorithm>

namespace kin {

namespace {

std::size_t pixel_index(Vec2i size, Vec2i pos) {
    return static_cast<std::size_t>((pos.y * size.x + pos.x) * 4);
}

} // namespace

RgbaCanvas::RgbaCanvas(Vec2i size, Color clear_color)
    : _size(size),
      _pixels(size.x > 0 && size.y > 0 ? static_cast<std::size_t>(size.x * size.y * 4) : 0) {
    clear(clear_color);
}

void RgbaCanvas::clear(Color color) {
    if (_pixels.empty()) {
        return;
    }

    for (std::size_t i = 0; i < _pixels.size(); i += 4) {
        _pixels[i + 0] = color.r;
        _pixels[i + 1] = color.g;
        _pixels[i + 2] = color.b;
        _pixels[i + 3] = color.a;
    }
}

bool RgbaCanvas::has_visible_pixels() const {
    for (std::size_t i = 3; i < _pixels.size(); i += 4) {
        if (_pixels[i] != 0) {
            return true;
        }
    }
    return false;
}

void RgbaCanvas::blend_pixel(Vec2i pos, Color src) {
    if (pos.x < 0 || pos.y < 0 || pos.x >= _size.x || pos.y >= _size.y || src.a == 0) {
        return;
    }

    const std::size_t i = pixel_index(_size, pos);
    if (src.a == 255) {
        _pixels[i + 0] = src.r;
        _pixels[i + 1] = src.g;
        _pixels[i + 2] = src.b;
        _pixels[i + 3] = src.a;
        return;
    }

    const u32 inv_a = 255 - src.a;
    _pixels[i + 0] = static_cast<u8>((static_cast<u32>(src.r) * src.a + static_cast<u32>(_pixels[i + 0]) * inv_a) / 255);
    _pixels[i + 1] = static_cast<u8>((static_cast<u32>(src.g) * src.a + static_cast<u32>(_pixels[i + 1]) * inv_a) / 255);
    _pixels[i + 2] = static_cast<u8>((static_cast<u32>(src.b) * src.a + static_cast<u32>(_pixels[i + 2]) * inv_a) / 255);
    _pixels[i + 3] = static_cast<u8>(std::min<u32>(255, src.a + (static_cast<u32>(_pixels[i + 3]) * inv_a) / 255));
}

bool RgbaCanvas::blit_sprite(const SpriteCatalog& sprites,
                             std::string_view sprite_id,
                             const std::vector<u8>& atlas_pixels,
                             Vec2i atlas_size,
                             Vec2i top_left) {
    ResolvedSprite resolved;
    if (_pixels.empty() || atlas_pixels.empty() || !sprites.resolve(sprite_id, resolved)) {
        return false;
    }

    const Rectf source = resolved.sprite.source;
    const i32 sx = static_cast<i32>(source.x);
    const i32 sy = static_cast<i32>(source.y);
    const i32 sw = static_cast<i32>(source.w);
    const i32 sh = static_cast<i32>(source.h);
    bool wrote = false;
    for (i32 y = 0; y < sh; ++y) {
        for (i32 x = 0; x < sw; ++x) {
            const Vec2i src_pos{sx + x, sy + y};
            if (src_pos.x < 0 || src_pos.y < 0 || src_pos.x >= atlas_size.x || src_pos.y >= atlas_size.y) {
                continue;
            }
            const std::size_t src_i = pixel_index(atlas_size, src_pos);
            const Color src{
                atlas_pixels[src_i + 0],
                atlas_pixels[src_i + 1],
                atlas_pixels[src_i + 2],
                atlas_pixels[src_i + 3],
            };
            if (src.a != 0) {
                wrote = true;
            }
            blend_pixel({top_left.x + x, top_left.y + y}, src);
        }
    }
    return wrote;
}

Texture RgbaCanvas::upload(Renderer2D& renderer) const {
    return renderer.create_texture_from_rgba(_pixels.data(), _size);
}

} // namespace kin
