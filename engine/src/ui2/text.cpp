#include <kin/ui2/text.hpp>

#include <kin/core/jobs.hpp>
#include <kin/core/utf8.hpp>
#include <kin/platform/log.hpp>
#include <kin/renderer/renderer2d.hpp>

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kin::ui2 {
namespace {

// ---- Signed distance fields for Sdf fonts ----

// One line of Felzenszwalb and Huttenlocher's exact squared Euclidean distance
// transform: d[q] = min over p of (q - p)^2 + f[p].
void distance_line(const f32* f, f32* d, i32* v, f32* z, i32 n) {
    constexpr f32 inf = 1e20f;
    i32 k = 0;
    v[0] = 0;
    z[0] = -inf;
    z[1] = inf;
    const auto meet = [&](i32 q, i32 p) {
        return ((f[q] + static_cast<f32>(q * q)) - (f[p] + static_cast<f32>(p * p))) / static_cast<f32>(2 * q - 2 * p);
    };
    for (i32 q = 1; q < n; ++q) {
        f32 s = meet(q, v[k]);
        while (s <= z[k]) {
            --k;
            s = meet(q, v[k]);
        }
        ++k;
        v[k] = q;
        z[k] = s;
        z[k + 1] = inf;
    }
    k = 0;
    for (i32 q = 0; q < n; ++q) {
        while (z[k + 1] < static_cast<f32>(q)) {
            ++k;
        }
        d[q] = static_cast<f32>((q - v[k]) * (q - v[k])) + f[v[k]];
    }
}

// The squared distance from every cell to the nearest one marked, down the
// columns then along the rows.
std::vector<f32> squared_distance(const std::vector<u8>& marked, i32 w, i32 h) {
    std::vector<f32> grid(marked.size());
    for (std::size_t i = 0; i < marked.size(); ++i) {
        grid[i] = marked[i] ? 0.0f : 1e20f;
    }
    const i32 n = std::max(w, h);
    std::vector<f32> f(static_cast<std::size_t>(n)), d(static_cast<std::size_t>(n)), z(static_cast<std::size_t>(n) + 1);
    std::vector<i32> v(static_cast<std::size_t>(n));
    for (i32 x = 0; x < w; ++x) {
        for (i32 y = 0; y < h; ++y) f[static_cast<std::size_t>(y)] = grid[static_cast<std::size_t>(y * w + x)];
        distance_line(f.data(), d.data(), v.data(), z.data(), h);
        for (i32 y = 0; y < h; ++y) grid[static_cast<std::size_t>(y * w + x)] = d[static_cast<std::size_t>(y)];
    }
    for (i32 y = 0; y < h; ++y) {
        std::copy_n(grid.begin() + y * w, w, f.begin());
        distance_line(f.data(), d.data(), v.data(), z.data(), w);
        std::copy_n(d.begin(), w, grid.begin() + y * w);
    }
    return grid;
}

// A glyph's coverage (`w` x `h` RGBA, drawn `supersample` times larger than
// the field) as a signed distance field: `spread` field pixels of margin all
// round, each texel's alpha 0.5 on the outline and 0.5 more or less `spread`
// pixels inside or out.
struct DistanceField {
    i32 w = 0;
    i32 h = 0;
    std::vector<u8> alpha;
};

DistanceField distance_field(const u8* rgba, i32 pitch, i32 w, i32 h, i32 supersample, f32 spread) {
    const i32 k = supersample;
    const i32 pad = static_cast<i32>(spread) * k;
    const i32 cw = (w + 2 * pad + k - 1) / k, ch = (h + 2 * pad + k - 1) / k; // the field's size
    const i32 hw = cw * k, hh = ch * k;
    std::vector<u8> inside(static_cast<std::size_t>(hw * hh), 0), outside(inside.size(), 1);
    for (i32 y = 0; y < h; ++y) {
        for (i32 x = 0; x < w; ++x) {
            if (rgba[y * pitch + x * 4 + 3] >= 128) {
                const std::size_t i = static_cast<std::size_t>((y + pad) * hw + x + pad);
                inside[i] = 1;
                outside[i] = 0;
            }
        }
    }
    const std::vector<f32> to_inside = squared_distance(inside, hw, hh);
    const std::vector<f32> to_outside = squared_distance(outside, hw, hh);
    DistanceField field{cw, ch, std::vector<u8>(static_cast<std::size_t>(cw * ch))};
    for (i32 fy = 0; fy < ch; ++fy) {
        for (i32 fx = 0; fx < cw; ++fx) {
            // The block's mean signed distance (pixel centres half a pixel off
            // the boundary), in field pixels.
            f32 sum = 0.0f;
            for (i32 y = fy * k; y < fy * k + k; ++y) {
                for (i32 x = fx * k; x < fx * k + k; ++x) {
                    const std::size_t i = static_cast<std::size_t>(y * hw + x);
                    sum += inside[i] ? std::sqrt(to_outside[i]) - 0.5f : 0.5f - std::sqrt(to_inside[i]);
                }
            }
            const f32 d = sum / static_cast<f32>(k * k * k);
            const f32 a = std::clamp(0.5f + d / (2.0f * spread), 0.0f, 1.0f);
            field.alpha[static_cast<std::size_t>(fy * cw + fx)] = static_cast<u8>(a * 255.0f + 0.5f);
        }
    }
    return field;
}

using Glyph = std::array<std::string_view, 7>;

Glyph glyph(char c) {
    if (c >= 'a' && c <= 'z') {
        c = static_cast<char>(c - 'a' + 'A');
    }
    switch (c) {
    case 'A': return {" ### ", "#   #", "#   #", "#####", "#   #", "#   #", "#   #"};
    case 'B': return {"#### ", "#   #", "#   #", "#### ", "#   #", "#   #", "#### "};
    case 'C': return {" ####", "#    ", "#    ", "#    ", "#    ", "#    ", " ####"};
    case 'D': return {"#### ", "#   #", "#   #", "#   #", "#   #", "#   #", "#### "};
    case 'E': return {"#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#####"};
    case 'F': return {"#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#    "};
    case 'G': return {" ####", "#    ", "#    ", "#  ##", "#   #", "#   #", " ####"};
    case 'H': return {"#   #", "#   #", "#   #", "#####", "#   #", "#   #", "#   #"};
    case 'I': return {"#####", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "#####"};
    case 'J': return {"#####", "   # ", "   # ", "   # ", "   # ", "#  # ", " ##  "};
    case 'K': return {"#   #", "#  # ", "# #  ", "##   ", "# #  ", "#  # ", "#   #"};
    case 'L': return {"#    ", "#    ", "#    ", "#    ", "#    ", "#    ", "#####"};
    case 'M': return {"#   #", "## ##", "# # #", "# # #", "#   #", "#   #", "#   #"};
    case 'N': return {"#   #", "##  #", "# # #", "#  ##", "#   #", "#   #", "#   #"};
    case 'O': return {" ### ", "#   #", "#   #", "#   #", "#   #", "#   #", " ### "};
    case 'P': return {"#### ", "#   #", "#   #", "#### ", "#    ", "#    ", "#    "};
    case 'Q': return {" ### ", "#   #", "#   #", "#   #", "# # #", "#  # ", " ## #"};
    case 'R': return {"#### ", "#   #", "#   #", "#### ", "# #  ", "#  # ", "#   #"};
    case 'S': return {" ####", "#    ", "#    ", " ### ", "    #", "    #", "#### "};
    case 'T': return {"#####", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  "};
    case 'U': return {"#   #", "#   #", "#   #", "#   #", "#   #", "#   #", " ### "};
    case 'V': return {"#   #", "#   #", "#   #", "#   #", "#   #", " # # ", "  #  "};
    case 'W': return {"#   #", "#   #", "#   #", "# # #", "# # #", "## ##", "#   #"};
    case 'X': return {"#   #", "#   #", " # # ", "  #  ", " # # ", "#   #", "#   #"};
    case 'Y': return {"#   #", "#   #", " # # ", "  #  ", "  #  ", "  #  ", "  #  "};
    case 'Z': return {"#####", "    #", "   # ", "  #  ", " #   ", "#    ", "#####"};
    case '0': return {" ### ", "#   #", "#  ##", "# # #", "##  #", "#   #", " ### "};
    case '1': return {"  #  ", " ##  ", "# #  ", "  #  ", "  #  ", "  #  ", "#####"};
    case '2': return {" ### ", "#   #", "    #", "   # ", "  #  ", " #   ", "#####"};
    case '3': return {"#### ", "    #", "    #", " ### ", "    #", "    #", "#### "};
    case '4': return {"#   #", "#   #", "#   #", "#####", "    #", "    #", "    #"};
    case '5': return {"#####", "#    ", "#    ", "#### ", "    #", "    #", "#### "};
    case '6': return {" ####", "#    ", "#    ", "#### ", "#   #", "#   #", " ### "};
    case '7': return {"#####", "    #", "   # ", "  #  ", " #   ", " #   ", " #   "};
    case '8': return {" ### ", "#   #", "#   #", " ### ", "#   #", "#   #", " ### "};
    case '9': return {" ### ", "#   #", "#   #", " ####", "    #", "    #", "#### "};
    case '/': return {"    #", "    #", "   # ", "  #  ", " #   ", "#    ", "#    "};
    case '>': return {"#    ", " #   ", "  #  ", "   # ", "  #  ", " #   ", "#    "};
    case '<': return {"    #", "   # ", "  #  ", " #   ", "  #  ", "   # ", "    #"};
    case '-': return {"     ", "     ", "     ", "#####", "     ", "     ", "     "};
    case '+': return {"     ", "  #  ", "  #  ", "#####", "  #  ", "  #  ", "     "};
    case ':': return {"     ", "  #  ", "  #  ", "     ", "  #  ", "  #  ", "     "};
    default: return {"     ", "     ", "     ", "     ", "     ", "     ", "     "};
    }
}

f32 bitmap_text_width(std::string_view text, f32 scale) {
    if (text.empty()) {
        return 0.0f;
    }
    return static_cast<f32>(text.size() * 6 - 1) * scale;
}

class BitmapFontBackend final : public IFontBackend {
public:
    Vec2f measure(std::string_view text, f32 scale) const override {
        return {bitmap_text_width(text, scale), 7.0f * scale};
    }

    void draw(Renderer2D& renderer, std::string_view text, Vec2f pos, f32 scale, Color color) const override {
        if (draw_from_glyph_atlas(renderer, text, pos, scale, color)) {
            return;
        }

        f32 cursor = pos.x;
        for (const char c : text) {
            const Glyph rows = glyph(c);
            for (std::size_t row = 0; row < rows.size(); ++row) {
                for (std::size_t col = 0; col < rows[row].size(); ++col) {
                    if (rows[row][col] != ' ') {
                        renderer.fill_rect({
                            cursor + static_cast<f32>(col) * scale,
                            pos.y + static_cast<f32>(row) * scale,
                            scale,
                            scale,
                        }, color);
                    }
                }
            }
            cursor += 6.0f * scale;
        }
    }

private:
    struct GlyphAtlas {
        u64 renderer_id = 0; // Renderer2D::id(), never a reused address
        Texture texture;
        u64 last_used = 0;
    };

    static constexpr i32 glyph_w = 5;
    static constexpr i32 glyph_h = 7;
    static constexpr i32 glyph_advance = 6;
    static constexpr i32 glyph_first = 32;
    static constexpr i32 glyph_last = 126;
    static constexpr i32 atlas_columns = 16;
    static constexpr i32 atlas_padding = 1;

    GlyphAtlas* find_glyph_atlas(Renderer2D& renderer) const {
        ++_atlas_tick;
        auto found = std::ranges::find_if(_glyph_atlases, [&](const GlyphAtlas& atlas) {
            return atlas.renderer_id == renderer.id();
        });
        if (found != _glyph_atlases.end()) {
            found->last_used = _atlas_tick;
            return &*found;
        }

        GlyphAtlas atlas = build_glyph_atlas(renderer);
        if (!atlas.texture) {
            return nullptr;
        }
        atlas.last_used = _atlas_tick;
        _glyph_atlases.push_back(std::move(atlas));
        return &_glyph_atlases.back();
    }

    GlyphAtlas build_glyph_atlas(Renderer2D& renderer) const {
        GlyphAtlas atlas;
        atlas.renderer_id = renderer.id();

        constexpr i32 cell_w = glyph_w + atlas_padding * 2;
        constexpr i32 cell_h = glyph_h + atlas_padding * 2;
        constexpr i32 glyph_count = glyph_last - glyph_first + 1;
        constexpr i32 rows = (glyph_count + atlas_columns - 1) / atlas_columns;
        const Vec2i atlas_size{atlas_columns * cell_w, rows * cell_h};
        std::vector<u8> pixels(static_cast<std::size_t>(atlas_size.x * atlas_size.y * 4), 0);

        for (i32 code = glyph_first; code <= glyph_last; ++code) {
            const char atlas_char = static_cast<char>(code);
            const char source_char = atlas_char >= 'a' && atlas_char <= 'z'
                ? static_cast<char>(atlas_char - 'a' + 'A')
                : atlas_char;
            const i32 index = code - glyph_first;
            const i32 col = index % atlas_columns;
            const i32 row = index / atlas_columns;
            const i32 dst_x = col * cell_w + atlas_padding;
            const i32 dst_y = row * cell_h + atlas_padding;
            const Glyph rows_pixels = glyph(source_char);
            for (i32 y = 0; y < glyph_h; ++y) {
                for (i32 x = 0; x < glyph_w; ++x) {
                    if (rows_pixels[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)] == ' ') {
                        continue;
                    }
                    const std::size_t offset = static_cast<std::size_t>(((dst_y + y) * atlas_size.x + dst_x + x) * 4);
                    pixels[offset + 0] = 255;
                    pixels[offset + 1] = 255;
                    pixels[offset + 2] = 255;
                    pixels[offset + 3] = 255;
                }
            }
        }

        atlas.texture = renderer.create_texture_from_rgba(pixels.data(), atlas_size);
        return atlas;
    }

    bool draw_from_glyph_atlas(Renderer2D& renderer, std::string_view text, Vec2f pos, f32 scale, Color color) const {
        if (text.empty()) {
            return true;
        }

        GlyphAtlas* atlas = find_glyph_atlas(renderer);
        if (!atlas) {
            return false;
        }

        constexpr i32 cell_w = glyph_w + atlas_padding * 2;
        constexpr i32 cell_h = glyph_h + atlas_padding * 2;
        Vec2f cursor = pos;
        for (char c : text) {
            const auto value = static_cast<unsigned char>(c);
            if (value < glyph_first || value > glyph_last) {
                cursor.x += static_cast<f32>(glyph_advance) * scale;
                continue;
            }

            if (c != ' ') {
                const i32 index = static_cast<i32>(value) - glyph_first;
                const i32 col = index % atlas_columns;
                const i32 row = index / atlas_columns;
                renderer.draw_texture(atlas->texture,
                                      {static_cast<f32>(col * cell_w + atlas_padding),
                                       static_cast<f32>(row * cell_h + atlas_padding),
                                       static_cast<f32>(glyph_w),
                                       static_cast<f32>(glyph_h)},
                                      {cursor.x, cursor.y, static_cast<f32>(glyph_w) * scale, static_cast<f32>(glyph_h) * scale},
                                      color);
            }
            cursor.x += static_cast<f32>(glyph_advance) * scale;
        }
        return true;
    }

    mutable std::vector<GlyphAtlas> _glyph_atlases;
    mutable u64 _atlas_tick = 0;
};

void ensure_ttf() {
    static const bool initialized = [] {
        if (!TTF_Init()) {
            KIN_LOG_ERROR_F("ui",
                            "ttf init failed",
                            (LogFields{{.name = "error", .value = SDL_GetError()}}));
            throw std::runtime_error(std::string{"TTF_Init failed: "} + SDL_GetError());
        }
        KIN_LOG_INFO("ui", "ttf initialized");
        return true;
    }();
    (void)initialized;
}

class TtfFontBackend final : public IFontBackend {
public:
    TtfFontBackend(const std::filesystem::path& path, f32 point_size, f32 oversample = 1.0f,
                   TextRendering rendering = TextRendering::Bitmap)
        : _path(path), _point_size(point_size), _oversample(std::max(1.0f, oversample)), _rendering(rendering) {
        ensure_ttf();
        if (!font_for_scale(1.0f)) {
            throw std::runtime_error(std::string{"TTF_OpenFont failed: "} + SDL_GetError());
        }
        KIN_LOG_INFO_F("ui",
                       "font loaded",
                       (LogFields{{.name = "path", .value = path.string()},
                                  {.name = "size", .value = std::to_string(point_size)}}));
    }

    ~TtfFontBackend() override {
        for (FontFace& face : _faces) {
            if (face.font) {
                TTF_CloseFont(face.font);
            }
        }
        if (_sdf_face) {
            TTF_CloseFont(_sdf_face);
        }
    }

    Vec2f measure(std::string_view text, f32 scale) const override {
        if (_rendering == TextRendering::Sdf) {
            // Linear: the large face's size, scaled down.
            TTF_Font* face = sdf_face();
            int w = 0, h = 0;
            const std::string copy{text};
            if (!face || !TTF_GetStringSize(face, copy.c_str(), copy.size(), &w, &h)) {
                return {};
            }
            const f32 s = sdf_scale(scale) / static_cast<f32>(sdf_supersample);
            return {static_cast<f32>(w) * s, static_cast<f32>(h) * s};
        }
        TTF_Font* font = font_for_scale(scale);
        if (!font) {
            return {};
        }
        int w = 0;
        int h = 0;
        const std::string copy{text};
        if (!TTF_GetStringSize(font, copy.c_str(), copy.size(), &w, &h)) {
            return {};
        }
        return {static_cast<f32>(w) / _oversample, static_cast<f32>(h) / _oversample};
    }

    bool draw_outlined(Renderer2D& renderer, std::string_view text, Vec2f pos, f32 scale, Color color, f32 outline,
                       Color outline_color) const override {
        return _rendering == TextRendering::Sdf && renderer.capabilities().distance_fields &&
               draw_distance_fields(renderer, text, pos, scale, color, outline, outline_color);
    }

    void draw(Renderer2D& renderer, std::string_view text, Vec2f pos, f32 scale, Color color) const override {
        if (text.empty()) {
            return;
        }
        if (_rendering == TextRendering::Sdf && renderer.capabilities().distance_fields &&
            draw_distance_fields(renderer, text, pos, scale, color, 0.0f, colors::transparent)) {
            return;
        }

        TTF_Font* font = font_for_scale(scale);
        if (!font) {
            return;
        }

        if (draw_from_glyph_atlas(renderer, font, text, pos, scale, color)) {
            return;
        }

        const CachedTextTexture* cached = find_cached_texture(renderer, text, color, scale);
        if (!cached) {
            cached = cache_text_texture(renderer, font, text, color, scale);
        }
        if (!cached) {
            return;
        }

        renderer.draw_texture(cached->texture,
                              {snap(pos.x),
                               snap(pos.y),
                               static_cast<f32>(cached->size.x) / _oversample,
                               static_cast<f32>(cached->size.y) / _oversample});
    }

private:
    struct FontFace {
        f32 point_size = 0.0f;
        TTF_Font* font = nullptr;
        u64 last_used = 0;
    };

    struct CachedTextTexture {
        u64 renderer_id = 0;
        std::string text;
        Color color{};
        f32 scale = 1.0f;
        Texture texture;
        Vec2i size{};
        u64 last_used = 0;
    };

    struct CachedGlyph {
        Rectf source{};
        Vec2i size{};
        f32 advance = 0.0f;
        f32 left = 0.0f;  // where the bitmap starts, from the pen: below 0 for a glyph that overhangs it
        bool drawable = false;
    };

    struct GlyphAtlas {
        u64 renderer_id = 0;
        f32 scale = 1.0f;
        Texture texture;
        std::array<CachedGlyph, 128> glyphs{};
        f32 line_height = 0.0f;
        u64 last_used = 0;
    };

    struct GlyphSurface {
        char ch = 0;
        Vec2i size{};
        f32 advance = 0.0f;
        f32 left = 0.0f;
        std::vector<u8> pixels;
    };

    // Sdf: one atlas of distance fields at the base size, made from glyphs
    // drawn `sdf_supersample` times larger; laid out by the large face's
    // metrics, scaled.
    static constexpr f32 sdf_base = 48.0f;    // the field's em, in its pixels
    static constexpr f32 sdf_spread = 8.0f;   // field pixels of distance either side of the outline
    static constexpr i32 sdf_supersample = 2;

    struct SdfGlyph {
        Rectf source{};    // in the atlas, margin included
        f32 advance = 0.0f; // base pixels
        f32 left = 0.0f;    // where the glyph's box starts, from the pen (base pixels)
        bool drawable = false;
    };

    struct SdfAtlas {
        u64 renderer_id = 0;
        Texture texture;
        std::array<SdfGlyph, 128> glyphs{};
        f32 line_height = 0.0f; // base pixels
    };

    // Base pixels to drawing units at `scale`.
    f32 sdf_scale(f32 scale) const { return _point_size * std::max(0.0f, scale) / sdf_base; }

    TTF_Font* sdf_face() const {
        if (!_sdf_face) {
            _sdf_face = TTF_OpenFont(_path.string().c_str(), sdf_base * static_cast<f32>(sdf_supersample));
        }
        return _sdf_face;
    }

    SdfAtlas* find_sdf_atlas(Renderer2D& renderer) const {
        for (SdfAtlas& atlas : _sdf_atlases) {
            if (atlas.renderer_id == renderer.id()) {
                return &atlas;
            }
        }
        if (SdfAtlas atlas = build_sdf_atlas(renderer); atlas.texture) {
            _sdf_atlases.push_back(std::move(atlas));
            return &_sdf_atlases.back();
        }
        return nullptr;
    }

    SdfAtlas build_sdf_atlas(Renderer2D& renderer) const {
        TTF_Font* face = sdf_face();
        if (!face) {
            return {};
        }
        constexpr f32 k = static_cast<f32>(sdf_supersample);
        SdfAtlas atlas;
        atlas.renderer_id = renderer.id();
        atlas.line_height = static_cast<f32>(TTF_GetFontHeight(face)) / k;
        // Each glyph drawn large here (SDL_ttf wants one thread), its distance
        // field made on the job system's workers.
        struct Made {
            char ch = 0;
            std::vector<u8> rgba;
            i32 w = 0, h = 0;
            DistanceField field;
        };
        std::vector<Made> made;
        for (char ch = 32; ch < 127; ++ch) {
            // As the Bitmap atlas: the pen moves by the advance, and the
            // bitmap starts at the left bearing when that is left of the pen.
            int min_x = 0, max_x = 0, min_y = 0, max_y = 0, advance = 0;
            if (!TTF_GetGlyphMetrics(face, static_cast<Uint32>(ch), &min_x, &max_x, &min_y, &max_y, &advance)) {
                continue;
            }
            SdfGlyph& glyph = atlas.glyphs[static_cast<std::size_t>(ch)];
            glyph.advance = static_cast<f32>(std::max(1, advance)) / k;
            glyph.left = static_cast<f32>(std::min(0, min_x)) / k;
            if (ch == ' ') {
                continue;
            }
            const std::string text{ch};
            SDL_Surface* surface = TTF_RenderText_Blended(face, text.c_str(), text.size(), SDL_Color{255, 255, 255, 255});
            if (!surface) {
                continue;
            }
            SDL_Surface* converted = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
            SDL_DestroySurface(surface);
            if (!converted) {
                continue;
            }
            Made m{.ch = ch, .rgba = {}, .w = converted->w, .h = converted->h, .field = {}};
            m.rgba.resize(static_cast<std::size_t>(converted->w * converted->h * 4));
            for (int y = 0; y < converted->h; ++y) {
                std::copy_n(static_cast<const u8*>(converted->pixels) + y * converted->pitch, converted->w * 4,
                            m.rgba.begin() + static_cast<std::ptrdiff_t>(y * converted->w * 4));
            }
            SDL_DestroySurface(converted);
            made.push_back(std::move(m));
        }
        default_job_system().parallel_for(static_cast<i32>(made.size()), [&](i32 i) {
            Made& m = made[static_cast<std::size_t>(i)];
            m.field = distance_field(m.rgba.data(), m.w * 4, m.w, m.h, sdf_supersample, sdf_spread);
            m.rgba = {};
        });
        i32 cell_w = 1, cell_h = 1;
        for (const Made& m : made) {
            cell_w = std::max(cell_w, m.field.w);
            cell_h = std::max(cell_h, m.field.h);
        }
        if (made.empty()) {
            return {};
        }
        constexpr i32 columns = 16;
        const i32 rows = (static_cast<i32>(made.size()) + columns - 1) / columns;
        const Vec2i size{columns * cell_w, rows * cell_h};
        std::vector<u8> pixels(static_cast<std::size_t>(size.x * size.y * 4), 255);
        for (std::size_t i = 3; i < pixels.size(); i += 4) {
            pixels[i] = 0;
        }
        for (std::size_t n = 0; n < made.size(); ++n) {
            const DistanceField& field = made[n].field;
            const i32 x0 = static_cast<i32>(n % columns) * cell_w, y0 = static_cast<i32>(n / columns) * cell_h;
            for (i32 y = 0; y < field.h; ++y) {
                for (i32 x = 0; x < field.w; ++x) {
                    pixels[static_cast<std::size_t>(((y0 + y) * size.x + x0 + x) * 4 + 3)] =
                        field.alpha[static_cast<std::size_t>(y * field.w + x)];
                }
            }
            SdfGlyph& glyph = atlas.glyphs[static_cast<std::size_t>(made[n].ch)];
            glyph.source = {static_cast<f32>(x0), static_cast<f32>(y0), static_cast<f32>(field.w),
                            static_cast<f32>(field.h)};
            glyph.drawable = true;
        }
        atlas.texture = renderer.create_texture_from_rgba(pixels.data(), size);
        if (atlas.texture) {
            renderer.set_scale_mode(atlas.texture, ScaleMode::Linear); // fields are read between texels
        }
        return atlas;
    }

    bool draw_distance_fields(Renderer2D& renderer, std::string_view text, Vec2f pos, f32 scale, Color color,
                              f32 outline, Color outline_color) const {
        if (!can_use_glyph_atlas(text)) {
            return false;
        }
        SdfAtlas* atlas = find_sdf_atlas(renderer);
        TTF_Font* face = sdf_face();
        if (!atlas || !face) {
            return false;
        }
        const f32 s = sdf_scale(scale);
        if (s <= 0.0f) {
            return true;
        }
        constexpr f32 k = static_cast<f32>(sdf_supersample);
        const SdfGlyph& space = atlas->glyphs[static_cast<std::size_t>(' ')];
        const f32 space_advance = space.advance > 0.0f ? space.advance : atlas->line_height * 0.5f;
        thread_local std::vector<SpriteInstance> quads;
        quads.clear();
        Vec2f pen{0.0f, 0.0f}; // base pixels from `pos`
        Uint32 previous = 0;
        for (const char ch : text) {
            if (ch == '\r') {
                continue;
            }
            if (ch == '\n') {
                pen = {0.0f, pen.y + atlas->line_height};
                previous = 0;
                continue;
            }
            if (ch == '\t') {
                pen.x += space_advance * 4.0f;
                previous = 0;
                continue;
            }
            const auto value = static_cast<unsigned char>(ch);
            int kerning = 0;
            if (previous != 0 && TTF_GetGlyphKerning(face, previous, value, &kerning)) {
                pen.x += static_cast<f32>(kerning) / k;
            }
            previous = value;
            const SdfGlyph& glyph = atlas->glyphs[value];
            if (glyph.drawable) {
                quads.push_back({.dest = {pos.x + (pen.x + glyph.left - sdf_spread) * s, pos.y + (pen.y - sdf_spread) * s,
                                          glyph.source.w * s, glyph.source.h * s},
                                 .source = glyph.source,
                                 .tint = color});
            }
            pen.x += glyph.advance > 0.0f ? glyph.advance : space_advance;
        }
        // The outline, in drawing units, as field pixels (at most what the
        // field holds outside the glyph).
        const DistanceFieldStyle field{.spread = sdf_spread,
                                       .outline = outline_color.a > 0 ? std::min(outline / s, sdf_spread - 1.0f) : 0.0f,
                                       .outline_color = outline_color};
        return renderer.draw_distance_field(atlas->texture, quads, field);
    }

    static constexpr std::size_t max_cache_entries = 512;
    // Bitmap: faces and atlases, one a size, kept for the most recent few.
    static constexpr std::size_t max_faces = 12;
    static constexpr std::size_t max_glyph_atlases = 8;

    static f32 effective_point_size(f32 point_size, f32 scale) {
        return std::max(1.0f, point_size * std::max(0.0f, scale));
    }

    static bool same_scale(f32 a, f32 b) {
        return std::abs(a - b) < 0.001f;
    }

    // Rounds to the grid the glyphs were rasterized for: logical pixels
    // normally, output pixels when oversampled, so every atlas texel lands on
    // exactly one pixel either way.
    f32 snap(f32 v) const { return std::round(v * _oversample) / _oversample; }

    TTF_Font* font_for_scale(f32 scale) const {
        const f32 size = effective_point_size(_point_size * _oversample, scale);
        ++_face_tick;
        auto found = std::ranges::find_if(_faces, [&](const FontFace& face) {
            return same_scale(face.point_size, size);
        });
        if (found != _faces.end()) {
            found->last_used = _face_tick;
            return found->font;
        }
        if (_faces.size() >= max_faces) {
            // A scale that keeps changing (an animation) would open faces
            // without end: the least recently used goes.
            const auto oldest = std::ranges::min_element(_faces, {}, &FontFace::last_used);
            TTF_CloseFont(oldest->font);
            _faces.erase(oldest);
        }

        TTF_Font* font = TTF_OpenFont(_path.string().c_str(), size);
        if (!font) {
            KIN_LOG_ERROR_F("ui",
                            "font open failed",
                            (LogFields{{.name = "path", .value = _path.string()},
                                       {.name = "size", .value = std::to_string(size)},
                                       {.name = "error", .value = SDL_GetError()}}));
            return nullptr;
        }
        _faces.push_back({.point_size = size, .font = font, .last_used = _face_tick});
        return font;
    }

    static bool can_use_glyph_atlas(std::string_view text) {
        return std::ranges::all_of(text, [](char ch) {
            const auto value = static_cast<unsigned char>(ch);
            return value == '\n' || value == '\r' || value == '\t' || (value >= 32 && value < 127);
        });
    }

    GlyphAtlas* find_glyph_atlas(Renderer2D& renderer, TTF_Font* font, f32 scale) const {
        ++_cache_tick;
        auto found = std::ranges::find_if(_glyph_atlases, [&](const GlyphAtlas& atlas) {
            return atlas.renderer_id == renderer.id() && same_scale(atlas.scale, scale);
        });
        if (found != _glyph_atlases.end()) {
            found->last_used = _cache_tick;
            return &*found;
        }

        if (GlyphAtlas atlas = build_glyph_atlas(renderer, font, scale); atlas.texture) {
            atlas.last_used = _cache_tick;
            if (_glyph_atlases.size() >= max_glyph_atlases) {
                _glyph_atlases.erase(std::ranges::min_element(_glyph_atlases, {}, &GlyphAtlas::last_used));
            }
            _glyph_atlases.push_back(std::move(atlas));
            return &_glyph_atlases.back();
        }
        return nullptr;
    }

    GlyphAtlas build_glyph_atlas(Renderer2D& renderer, TTF_Font* font, f32 scale) const {
        GlyphAtlas atlas;
        atlas.renderer_id = renderer.id();
        atlas.scale = scale;

        std::vector<GlyphSurface> surfaces;
        surfaces.reserve(95);
        i32 max_w = 1;
        i32 max_h = 1;
        for (char ch = 32; ch < 127; ++ch) {
            const std::string text{ch};
            int extent_w = 0;
            int extent_h = 0;
            if (!TTF_GetStringSize(font, text.c_str(), text.size(), &extent_w, &extent_h)) {
                continue;
            }
            // The pen moves by the glyph's advance, not by the width of its
            // bitmap: a glyph that overhangs its advance (italics, an f or a
            // j) would otherwise push the next one away. Its bitmap starts at
            // its left bearing when that is left of the pen (SDL_ttf renders a
            // string from min(0, the first glyph's left)).
            int min_x = 0;
            int max_x = 0;
            int min_y = 0;
            int max_y = 0;
            int advance_w = extent_w;
            if (!TTF_GetGlyphMetrics(font, static_cast<Uint32>(ch), &min_x, &max_x, &min_y, &max_y, &advance_w)) {
                advance_w = extent_w;
                min_x = 0;
            }
            const f32 left = static_cast<f32>(std::min(0, min_x));
            atlas.glyphs[static_cast<std::size_t>(ch)].advance = static_cast<f32>(std::max(1, advance_w));
            atlas.line_height = std::max(atlas.line_height, static_cast<f32>(std::max(1, extent_h)));

            if (ch == ' ') {
                continue;
            }

            SDL_Surface* surface = TTF_RenderText_Blended(font, text.c_str(), text.size(), SDL_Color{255, 255, 255, 255});
            if (!surface) {
                continue;
            }
            SDL_Surface* converted = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
            SDL_DestroySurface(surface);
            if (!converted) {
                continue;
            }

            GlyphSurface glyph;
            glyph.ch = ch;
            glyph.size = {converted->w, converted->h};
            glyph.advance = static_cast<f32>(std::max(1, advance_w));
            glyph.left = left;
            glyph.pixels.resize(static_cast<std::size_t>(converted->w * converted->h * 4));
            const auto* src = static_cast<const u8*>(converted->pixels);
            for (int y = 0; y < converted->h; ++y) {
                std::ranges::copy_n(src + y * converted->pitch,
                                    converted->w * 4,
                                    glyph.pixels.begin() + static_cast<std::ptrdiff_t>(y * converted->w * 4));
            }
            SDL_DestroySurface(converted);

            max_w = std::max(max_w, glyph.size.x);
            max_h = std::max(max_h, glyph.size.y);
            surfaces.push_back(std::move(glyph));
        }

        if (surfaces.empty()) {
            return {};
        }

        constexpr i32 columns = 16;
        constexpr i32 padding = 1;
        const i32 cell_w = max_w + padding * 2;
        const i32 cell_h = max_h + padding * 2;
        const i32 rows = (95 + columns - 1) / columns;
        const Vec2i atlas_size{columns * cell_w, rows * cell_h};
        std::vector<u8> pixels(static_cast<std::size_t>(atlas_size.x * atlas_size.y * 4), 0);

        for (const GlyphSurface& glyph : surfaces) {
            const i32 index = glyph.ch - 32;
            const i32 col = index % columns;
            const i32 row = index / columns;
            const i32 dst_x = col * cell_w + padding;
            const i32 dst_y = row * cell_h + padding;
            atlas.glyphs[static_cast<std::size_t>(glyph.ch)] = {
                .source = {static_cast<f32>(dst_x), static_cast<f32>(dst_y), static_cast<f32>(glyph.size.x), static_cast<f32>(glyph.size.y)},
                .size = glyph.size,
                .advance = glyph.advance,
                .left = glyph.left,
                .drawable = true,
            };
            for (i32 y = 0; y < glyph.size.y; ++y) {
                const auto* src = glyph.pixels.data() + static_cast<std::ptrdiff_t>(y * glyph.size.x * 4);
                auto dst = pixels.begin() + static_cast<std::ptrdiff_t>(((dst_y + y) * atlas_size.x + dst_x) * 4);
                std::ranges::copy_n(src, glyph.size.x * 4, dst);
            }
        }

        atlas.texture = renderer.create_texture_from_rgba(pixels.data(), atlas_size);
        if (atlas.line_height <= 0.0f) {
            atlas.line_height = static_cast<f32>(max_h);
        }
        return atlas;
    }

    bool draw_from_glyph_atlas(Renderer2D& renderer, TTF_Font* font, std::string_view text, Vec2f pos, f32 scale, Color color) const {
        if (!can_use_glyph_atlas(text)) {
            return false;
        }

        GlyphAtlas* atlas = find_glyph_atlas(renderer, font, scale);
        if (!atlas) {
            return false;
        }

        Vec2f cursor = pos;
        const f32 inv = 1.0f / _oversample;
        const f32 line_height = atlas->line_height * inv;
        const f32 space_advance = (atlas->glyphs[static_cast<std::size_t>(' ')].advance > 0.0f
            ? atlas->glyphs[static_cast<std::size_t>(' ')].advance
            : atlas->line_height * 0.5f) * inv;
        // Advances and kerning follow the font, so text draws as wide as
        // measure() (TTF_GetStringSize) says and carets and selections line up.
        Uint32 previous = 0;
        for (char ch : text) {
            if (ch == '\r') {
                continue;
            }
            if (ch == '\n') {
                cursor.x = pos.x;
                cursor.y += line_height;
                previous = 0;
                continue;
            }
            if (ch == '\t') {
                cursor.x += space_advance * 4.0f;
                previous = 0;
                continue;
            }

            const auto value = static_cast<unsigned char>(ch);
            int kerning = 0;
            if (previous != 0 && TTF_GetGlyphKerning(font, previous, value, &kerning)) {
                cursor.x += static_cast<f32>(kerning) * inv;
            }
            previous = value;
            const CachedGlyph& glyph = atlas->glyphs[static_cast<std::size_t>(value)];
            if (glyph.drawable) {
                // Pixel-snap the destination: the glyph atlas is integer-sized, so an
                // integer dest is a 1:1 blit (crisp). Fractional positions (from
                // accumulated sub-pixel advances) make the GPU linear-sample off-grid,
                // which softens and unevens the text. The fractional pen is kept for
                // correct average advance; only the draw position rounds.
                renderer.draw_texture(atlas->texture,
                                      glyph.source,
                                      {snap(cursor.x + glyph.left * inv),
                                       snap(cursor.y),
                                       static_cast<f32>(glyph.size.x) * inv,
                                       static_cast<f32>(glyph.size.y) * inv},
                                      color);
            }
            cursor.x += glyph.advance > 0.0f ? glyph.advance * inv : space_advance;
        }
        return true;
    }

    const CachedTextTexture* find_cached_texture(Renderer2D& renderer, std::string_view text, Color color, f32 scale) const {
        ++_cache_tick;
        auto found = std::ranges::find_if(_cache, [&](const CachedTextTexture& entry) {
            return entry.renderer_id == renderer.id() && entry.text == text && entry.color == color && same_scale(entry.scale, scale);
        });
        if (found == _cache.end()) {
            return nullptr;
        }
        found->last_used = _cache_tick;
        return &*found;
    }

    const CachedTextTexture* cache_text_texture(Renderer2D& renderer, TTF_Font* font, std::string_view text, Color color, f32 scale) const {
        const std::string copy{text};
        SDL_Color sdl_color{color.r, color.g, color.b, color.a};
        SDL_Surface* surface = TTF_RenderText_Blended(font, copy.c_str(), copy.size(), sdl_color);
        if (!surface) {
            return nullptr;
        }

        SDL_Surface* converted = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(surface);
        if (!converted) {
            return nullptr;
        }

        std::vector<u8> pixels(static_cast<std::size_t>(converted->w * converted->h * 4));
        const auto* src = static_cast<const u8*>(converted->pixels);
        for (int y = 0; y < converted->h; ++y) {
            std::ranges::copy_n(src + y * converted->pitch,
                                converted->w * 4,
                                pixels.begin() + static_cast<std::ptrdiff_t>(y * converted->w * 4));
        }

        const Vec2i size{converted->w, converted->h};
        SDL_DestroySurface(converted);

        Texture texture = renderer.create_texture_from_rgba(pixels.data(), size);
        if (!texture) {
            return nullptr;
        }

        _cache.push_back(CachedTextTexture{
            .renderer_id = renderer.id(),
            .text = copy,
            .color = color,
            .scale = scale,
            .texture = std::move(texture),
            .size = size,
            .last_used = _cache_tick,
        });
        evict_old_cache_entries();
        return &_cache.back();
    }

    void evict_old_cache_entries() const {
        if (_cache.size() <= max_cache_entries) {
            return;
        }

        const auto oldest = std::ranges::min_element(_cache, {}, &CachedTextTexture::last_used);
        if (oldest != _cache.end()) {
            _cache.erase(oldest);
        }
    }

    std::filesystem::path _path;
    f32 _point_size = 0.0f;
    f32 _oversample = 1.0f;
    TextRendering _rendering = TextRendering::Bitmap;
    mutable TTF_Font* _sdf_face = nullptr;
    mutable std::vector<SdfAtlas> _sdf_atlases;
    mutable u64 _face_tick = 0;
    mutable std::vector<FontFace> _faces;
    mutable std::vector<GlyphAtlas> _glyph_atlases;
    mutable std::vector<CachedTextTexture> _cache;
    mutable u64 _cache_tick = 0;
};

} // namespace

Font::Font(std::shared_ptr<const IFontBackend> backend)
    : _backend(std::move(backend)) {
}

Font bitmap_font() {
    static const Font font{std::make_shared<BitmapFontBackend>()};
    return font;
}

Font load_ttf_font(const std::filesystem::path& path, f32 point_size, f32 oversample, TextRendering rendering) {
    return Font{std::make_shared<TtfFontBackend>(path, point_size, oversample, rendering)};
}

namespace {

const std::filesystem::path& system_ui_font_path(bool bold) {
    static const auto paths = [] {
        const auto first_existing = [](std::initializer_list<const char*> candidates) {
            for (const char* candidate : candidates) {
                if (std::filesystem::exists(candidate)) {
                    return std::filesystem::path{candidate};
                }
            }
            return std::filesystem::path{};
        };
        struct Paths {
            std::filesystem::path regular;
            std::filesystem::path bold;
        } result;
        result.regular = first_existing({
            "C:/Windows/Fonts/segoeui.ttf",
            "C:/Windows/Fonts/arial.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
            "/Library/Fonts/Arial.ttf",
        });
        result.bold = first_existing({
            "C:/Windows/Fonts/segoeuib.ttf",
            "C:/Windows/Fonts/arialbd.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
            "/usr/share/fonts/truetype/liberation2/LiberationSans-Bold.ttf",
            "/Library/Fonts/Arial Bold.ttf",
        });
        if (result.bold.empty()) {
            result.bold = result.regular;
        }
        return result;
    }();
    return bold ? paths.bold : paths.regular;
}

Font cached_system_font(f32 point_size, bool bold, TextRendering rendering) {
    struct Key {
        i32 size;
        bool bold;
        TextRendering rendering;
        auto operator<=>(const Key&) const = default;
    };
    static std::map<Key, Font> cache;
    const i32 clamped = static_cast<i32>(std::clamp(point_size, 8.0f, 32.0f));
    const Key key{clamped, bold, rendering};
    if (const auto it = cache.find(key); it != cache.end()) {
        return it->second;
    }
    const std::filesystem::path& path = system_ui_font_path(bold);
    Font font = path.empty() ? bitmap_font() : load_ttf_font(path, static_cast<f32>(clamped), 1.0f, rendering);
    cache.emplace(key, font);
    return font;
}

} // namespace

Font system_ui_font(f32 point_size, TextRendering rendering) {
    return cached_system_font(point_size, false, rendering);
}

Font system_ui_font_bold(f32 point_size, TextRendering rendering) {
    return cached_system_font(point_size, true, rendering);
}

bool system_ui_font_available() {
    return !system_ui_font_path(false).empty();
}

Vec2f measure_text(const Font& font, std::string_view text, f32 scale) {
    const Font actual = font ? font : bitmap_font();
    return actual._backend->measure(text, scale);
}

std::vector<std::string> wrap_text(const Font& font, std::string_view text, TextWrapOptions options) {
    if (text.empty() || options.max_lines == 0) {
        return {};
    }

    const Font actual = font ? font : bitmap_font();
    if (options.max_width <= 0.0f) {
        return {std::string{text}};
    }

    std::vector<std::string> lines;
    std::string line;
    std::string word;

    const auto reached_limit = [&] {
        return options.max_lines >= 0 && static_cast<i32>(lines.size()) >= options.max_lines;
    };

    const auto fits = [&](std::string_view value) {
        return measure_text(actual, value, options.scale).x <= options.max_width;
    };

    const auto push_line = [&](std::string value) {
        if (reached_limit()) {
            return false;
        }
        lines.push_back(std::move(value));
        return !reached_limit();
    };

    const auto split_long_word = [&](std::string_view value) {
        std::size_t begin = 0;
        while (begin < value.size()) {
            std::size_t end = begin + 1;
            std::size_t best = end;
            while (end <= value.size() && fits(value.substr(begin, end - begin))) {
                best = end;
                ++end;
            }
            if (!push_line(std::string{value.substr(begin, best - begin)})) {
                return false;
            }
            begin = best;
        }
        return true;
    };

    const auto flush_word = [&] {
        if (word.empty()) {
            return !reached_limit();
        }

        const std::string candidate = line.empty() ? word : line + " " + word;
        if (fits(candidate)) {
            line = candidate;
            word.clear();
            return true;
        }

        if (!line.empty()) {
            if (!push_line(std::move(line))) {
                word.clear();
                return false;
            }
            line.clear();
        }

        if (!fits(word) && options.break_long_words) {
            const bool ok = split_long_word(word);
            word.clear();
            return ok;
        }

        line = std::move(word);
        word.clear();
        return true;
    };

    for (const char ch : text) {
        if (ch == '\n') {
            if (!flush_word()) {
                return lines;
            }
            if (!push_line(std::move(line))) {
                return lines;
            }
            line.clear();
        } else if (ch == ' ' || ch == '\t' || ch == '\r') {
            if (!flush_word()) {
                return lines;
            }
        } else {
            word.push_back(ch);
        }
    }

    if (!flush_word()) {
        return lines;
    }
    if (!line.empty() || lines.empty()) {
        push_line(std::move(line));
    }

    return lines;
}

std::vector<TextRange> wrap_text_ranges(const Font& font, std::string_view text, TextWrapOptions options) {
    std::vector<TextRange> lines;
    if (options.max_lines == 0) {
        return lines;
    }
    const Font actual = font ? font : bitmap_font();
    const auto reached_limit = [&] {
        return options.max_lines >= 0 && static_cast<i32>(lines.size()) >= options.max_lines;
    };

    // Character widths, measured once per distinct character.
    std::unordered_map<u32, f32> widths;
    const auto width_of = [&](std::size_t begin, std::size_t end) {
        u32 key = 0;
        for (std::size_t i = begin; i < end; ++i) {
            key = (key << 8) | static_cast<unsigned char>(text[i]);
        }
        const auto [it, inserted] = widths.try_emplace(key, 0.0f);
        if (inserted) {
            it->second = measure_text(actual, text.substr(begin, end - begin), options.scale).x;
        }
        return it->second;
    };
    const auto is_space = [&](std::size_t i) {
        return text[i] == ' ' || text[i] == '\t' || text[i] == '\r';
    };

    std::size_t hard_begin = 0;
    while (!reached_limit()) {
        const std::size_t newline = text.find('\n', hard_begin);
        const std::size_t hard_end = newline == std::string_view::npos ? text.size() : newline;
        std::size_t pos = hard_begin;
        if (options.max_width <= 0.0f) {
            lines.push_back({hard_begin, hard_end});
        } else {
            while (!reached_limit()) {
                f32 width = 0.0f;
                std::size_t after_space = std::string_view::npos;
                std::size_t i = pos;
                while (i < hard_end) {
                    const std::size_t next = utf8_next(text, i);
                    const f32 w = width_of(i, next);
                    const bool space = is_space(i);
                    // Whitespace may hang past the edge; the first character always fits.
                    if (!space && i > pos && width + w > options.max_width) {
                        break;
                    }
                    width += w;
                    if (space) {
                        after_space = next;
                    }
                    i = next;
                }
                if (i >= hard_end) {
                    lines.push_back({pos, hard_end});
                    break;
                }
                std::size_t cut = i;
                if (after_space != std::string_view::npos && after_space > pos) {
                    cut = after_space;
                } else if (!options.break_long_words) {
                    // Keep the word whole: the line runs to its end and the spaces after it.
                    while (cut < hard_end && !is_space(cut)) {
                        cut = utf8_next(text, cut);
                    }
                    while (cut < hard_end && is_space(cut)) {
                        cut = utf8_next(text, cut);
                    }
                }
                lines.push_back({pos, cut});
                pos = cut;
                if (pos >= hard_end) {
                    break;
                }
            }
        }
        if (newline == std::string_view::npos) {
            break;
        }
        hard_begin = newline + 1;
    }
    return lines;
}

std::vector<std::string> wrap_text(const Font& font, std::string_view text, f32 max_width, f32 scale) {
    return wrap_text(font, text, TextWrapOptions{.max_width = max_width, .scale = scale});
}

std::vector<std::string> wrap_text(std::string_view text, f32 max_width, f32 scale) {
    return wrap_text(bitmap_font(), text, max_width, scale);
}

Vec2f measure_wrapped_text(const Font& font, std::string_view text, TextWrapOptions options) {
    const std::vector<std::string> lines = wrap_text(font, text, options);
    if (lines.empty()) {
        return {};
    }

    f32 width = 0.0f;
    f32 height = 0.0f;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const Vec2f measured = measure_text(font, lines[i], options.scale);
        width = std::max(width, measured.x);
        height += measured.y;
        if (i + 1 < lines.size()) {
            height += options.line_spacing;
        }
    }
    return {width, height};
}

Vec2f measure_wrapped_text(std::string_view text, TextWrapOptions options) {
    return measure_wrapped_text(bitmap_font(), text, options);
}

void draw_text(Renderer2D& renderer,
               const Font& font,
               std::string_view text,
               Vec2f pos,
               f32 scale,
               Color color) {
    const Font actual = font ? font : bitmap_font();
    actual._backend->draw(renderer, text, pos, scale, color);
}

void draw_text(Renderer2D& renderer, std::string_view text, Vec2f pos, f32 scale, Color color) {
    draw_text(renderer, bitmap_font(), text, pos, scale, color);
}

void draw_text_outlined(Renderer2D& renderer, const Font& font, std::string_view text, Vec2f pos, f32 scale,
                        Color color, f32 outline, Color outline_color) {
    const Font actual = font ? font : bitmap_font();
    if (outline <= 0.0f || outline_color.a == 0) {
        actual._backend->draw(renderer, text, pos, scale, color);
        return;
    }
    if (actual._backend->draw_outlined(renderer, text, pos, scale, color, outline, outline_color)) {
        return;
    }
    const std::array<Vec2f, 4> offsets{{{-outline, 0.0f}, {outline, 0.0f}, {0.0f, -outline}, {0.0f, outline}}};
    for (const Vec2f off : offsets) {
        actual._backend->draw(renderer, text, {pos.x + off.x, pos.y + off.y}, scale, outline_color);
    }
    actual._backend->draw(renderer, text, pos, scale, color);
}

void draw_text_centered(Renderer2D& renderer,
                        const Font& font,
                        std::string_view text,
                        Vec2f center,
                        f32 scale,
                        Color color) {
    const Vec2f size = measure_text(font, text, scale);
    draw_text(renderer, font, text, {center.x - size.x * 0.5f, center.y - size.y * 0.5f}, scale, color);
}

void draw_text_centered(Renderer2D& renderer, std::string_view text, Vec2f center, f32 scale, Color color) {
    draw_text_centered(renderer, bitmap_font(), text, center, scale, color);
}

void draw_wrapped_text(Renderer2D& renderer,
                       const Font& font,
                       std::string_view text,
                       Vec2f pos,
                       TextWrapOptions options,
                       Color color) {
    const std::vector<std::string> lines = wrap_text(font, text, options);
    f32 y = pos.y;
    for (const std::string& line : lines) {
        draw_text(renderer, font, line, {pos.x, y}, options.scale, color);
        y += measure_text(font, line, options.scale).y + options.line_spacing;
    }
}

void draw_wrapped_text(Renderer2D& renderer,
                       std::string_view text,
                       Vec2f pos,
                       TextWrapOptions options,
                       Color color) {
    draw_wrapped_text(renderer, bitmap_font(), text, pos, options, color);
}

} // namespace kin
