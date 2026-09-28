#include <kin/ui2/skin_loader.hpp>

#include <kin/assets/asset_manager.hpp>
#include <kin/assets/image.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/renderer/sprite.hpp>

#include <algorithm>
#include <cmath>
#include <memory>

namespace kin::ui2 {

Texture load_texture(AssetManager& assets, Renderer2D& renderer, std::string_view path,
                     ScaleMode scale) {
    const std::shared_ptr<const Image> image = assets.load<Image>(path);
    Texture texture = renderer.create_texture_from_rgba(image->rgba.data(), image->size);
    renderer.set_scale_mode(texture, scale);
    return texture;
}

UiNineSlice nine_slice(Texture texture, f32 margin, Rectf source) {
    const Vec2i size = texture.size();
    const Rectf src = (source.w > 0.0f && source.h > 0.0f)
                          ? source
                          : Rectf{0.0f, 0.0f, static_cast<f32>(size.x), static_cast<f32>(size.y)};
    return UiNineSlice{Sprite{std::move(texture), src}, margin, margin, margin, margin};
}

UiNineSlice load_nine_slice(AssetManager& assets, Renderer2D& renderer, std::string_view path,
                            f32 margin, ScaleMode scale) {
    return nine_slice(load_texture(assets, renderer, path, scale), margin);
}

std::vector<u8> make_frame_rgba(int px, Color border, Color fill, f32 radius, f32 border_w) {
    std::vector<u8> buf(static_cast<std::size_t>(px) * px * 4u, 0u);
    const f32 half = px * 0.5f;
    const auto shade_c = [](Color c, f32 f) {
        const auto s = [f](u8 v) {
            return static_cast<u8>(std::clamp(static_cast<f32>(v) * f, 0.0f, 255.0f));
        };
        return Color{s(c.r), s(c.g), s(c.b), c.a};
    };
    for (int y = 0; y < px; ++y) {
        const f32 v = px > 1 ? static_cast<f32>(y) / static_cast<f32>(px - 1) : 0.0f;
        for (int x = 0; x < px; ++x) {
            // Signed distance to a rounded box centered in the texture (<0 = inside).
            const f32 qx = std::abs((x + 0.5f) - half) - (half - radius);
            const f32 qy = std::abs((y + 0.5f) - half) - (half - radius);
            const f32 ax = std::max(qx, 0.0f), ay = std::max(qy, 0.0f);
            const f32 d = std::min(std::max(qx, qy), 0.0f)
                          + std::sqrt(ax * ax + ay * ay) - radius;
            Color c{0, 0, 0, 0};
            if (d < 0.0f) {
                if (-d <= border_w) {
                    // Beveled rim: brighter at the outer edge, darker deeper in.
                    const f32 k = std::clamp((-d) / border_w, 0.0f, 1.0f);
                    c = shade_c(border, 1.25f - 0.7f * k);
                } else {
                    // Embossed fill: vertical gradient (lighter top) reads as a real surface.
                    c = shade_c(fill, 1.18f - 0.4f * v);
                }
                // Soften the outer 1 px for smooth edges on any background.
                const f32 aa = std::clamp(-d, 0.0f, 1.0f);
                c.a = static_cast<u8>(static_cast<f32>(c.a) * aa);
            }
            const std::size_t o = (static_cast<std::size_t>(y) * px + x) * 4u;
            buf[o + 0] = c.r;
            buf[o + 1] = c.g;
            buf[o + 2] = c.b;
            buf[o + 3] = c.a;
        }
    }
    return buf;
}

UiNineSlice make_frame_skin(Renderer2D& renderer, int px,
                            Color border, Color fill, f32 radius, f32 border_w) {
    const std::vector<u8> buf = make_frame_rgba(px, border, fill, radius, border_w);
    Texture tex = renderer.create_texture_from_rgba(buf.data(), {px, px});
    renderer.set_scale_mode(tex, ScaleMode::Linear);
    const f32 m = radius + 3.0f;
    return uniform_nine_slice(full_sprite(std::move(tex)), m);
}

} // namespace kin::ui2
