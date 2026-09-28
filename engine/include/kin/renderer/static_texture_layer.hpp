#pragma once

#include <kin/core/types.hpp>
#include <kin/renderer/color.hpp>
#include <kin/renderer/texture.hpp>

#include <string_view>
#include <vector>

namespace kin {

class Renderer2D;
class SpriteCatalog;

class RgbaCanvas {
public:
    RgbaCanvas() = default;
    explicit RgbaCanvas(Vec2i size, Color clear = colors::transparent);

    Vec2i size() const { return _size; }
    bool empty() const { return _pixels.empty(); }
    const std::vector<u8>& pixels() const { return _pixels; }
    std::vector<u8>& pixels() { return _pixels; }
    const u8* data() const { return _pixels.data(); }
    u8* data() { return _pixels.data(); }

    void clear(Color color = colors::transparent);
    bool has_visible_pixels() const;
    void blend_pixel(Vec2i pos, Color color);
    bool blit_sprite(const SpriteCatalog& sprites,
                     std::string_view sprite_id,
                     const std::vector<u8>& atlas_pixels,
                     Vec2i atlas_size,
                     Vec2i top_left);
    Texture upload(Renderer2D& renderer) const;

private:
    Vec2i _size{};
    std::vector<u8> _pixels;
};

} // namespace kin
