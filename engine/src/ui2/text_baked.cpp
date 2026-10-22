#include <kin/ui2/baked_font.hpp>
#include <kin/core/utf8.hpp>
#include <kin/renderer/renderer2d.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace kin::ui2 {
namespace {
class BakedFontBackend final : public IFontBackend {
public:
    explicit BakedFontBackend(BakedFontData data) : _data(data) {
        if (data.atlas_size.x <= 0 || data.atlas_size.y <= 0 ||
            data.atlas_size.x > 4096 || data.atlas_size.y > 4096 ||
            !std::isfinite(data.line_height) || data.line_height <= 0 ||
            data.alpha_rle.size() % 2 != 0) {
            throw std::invalid_argument("Invalid baked font dimensions or coverage");
        }
        std::size_t decoded = 0;
        for (std::size_t i = 0; i < data.alpha_rle.size(); i += 2) {
            if (data.alpha_rle[i] == 0) throw std::invalid_argument("Empty baked font run");
            decoded += data.alpha_rle[i];
        }
        if (decoded != static_cast<std::size_t>(data.atlas_size.x) * data.atlas_size.y) {
            throw std::invalid_argument("Baked font coverage size mismatch");
        }
        u32 previous = 0;
        for (const auto& glyph : data.glyphs) {
            const auto r = glyph.source;
            if (glyph.codepoint <= previous || r.x < 0 || r.y < 0 || r.w < 0 || r.h < 0 ||
                r.x > data.atlas_size.x || r.y > data.atlas_size.y ||
                r.w > data.atlas_size.x - r.x || r.h > data.atlas_size.y - r.y ||
                !std::isfinite(glyph.advance) || !std::isfinite(glyph.offset.x) || !std::isfinite(glyph.offset.y)) {
                throw std::invalid_argument("Invalid baked font glyph");
            }
            previous = glyph.codepoint;
        }
    }

    Vec2f measure(std::string_view text, f32 scale) const override {
        return walk(text, scale, [](const BakedGlyph&, Vec2f) {});
    }

    void draw(Renderer2D& renderer, std::string_view text, Vec2f pos, f32 scale, Color color) const override {
        if (!_atlas || _renderer_id != renderer.id()) {
            std::vector<u8> rgba(static_cast<std::size_t>(_data.atlas_size.x) * _data.atlas_size.y * 4, 255);
            std::size_t pixel = 0;
            for (std::size_t i = 0; i < _data.alpha_rle.size(); i += 2) {
                for (int run = 0; run < _data.alpha_rle[i]; ++run) {
                    rgba[pixel++ * 4 + 3] = _data.alpha_rle[i + 1];
                }
            }
            _atlas = renderer.create_texture_from_rgba(rgba.data(), _data.atlas_size);
            if (_atlas) renderer.set_scale_mode(_atlas, ScaleMode::Linear);
            _renderer_id = renderer.id();
        }
        if (!_atlas) return;
        walk(text, scale, [&](const BakedGlyph& glyph, Vec2f pen) {
            const auto r = glyph.source;
            if (!r.w || !r.h) return;
            renderer.draw_texture(_atlas,
                {float(r.x), float(r.y), float(r.w), float(r.h)},
                {pos.x + pen.x + glyph.offset.x * scale, pos.y + pen.y + glyph.offset.y * scale,
                 r.w * scale, r.h * scale}, color);
        });
    }

private:
    const BakedGlyph* find(u32 codepoint) const {
        auto it = std::ranges::lower_bound(_data.glyphs, codepoint, {}, &BakedGlyph::codepoint);
        return it != _data.glyphs.end() && it->codepoint == codepoint ? &*it : nullptr;
    }

    template<class Visit> Vec2f walk(std::string_view text, f32 scale, Visit visit) const {
        if (text.empty()) return {};
        Vec2f pen{};
        f32 width = 0;
        u32 previous = 0;
        for (std::size_t at = 0; at < text.size(); at = utf8_next(text, at)) {
            const u32 codepoint = utf8_decode(text, at);
            if (codepoint == '\n') {
                width = std::max(width, pen.x);
                pen.x = 0;
                pen.y += _data.line_height * scale;
                previous = 0;
                continue;
            }
            const auto* glyph = find(codepoint);
            if (!glyph) glyph = find('?');
            if (!glyph) { previous = 0; continue; }
            for (const auto& kern : _data.kerning) {
                if (kern.left == previous && kern.right == glyph->codepoint) {
                    pen.x += kern.adjustment * scale;
                    break;
                }
            }
            visit(*glyph, pen);
            pen.x += glyph->advance * scale;
            previous = glyph->codepoint;
        }
        return {std::max(width, pen.x), pen.y + _data.line_height * scale};
    }

    BakedFontData _data;
    mutable Texture _atlas;
    mutable u64 _renderer_id = 0;
};
}

Font baked_font(BakedFontData data) {
    return Font{std::make_shared<BakedFontBackend>(data)};
}
} // namespace kin::ui2
