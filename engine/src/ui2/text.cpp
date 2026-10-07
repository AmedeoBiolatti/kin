#include <kin/ui2/text.hpp>

#include "text_internal.hpp"

#include <kin/core/utf8.hpp>
#include <kin/renderer/renderer2d.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <limits>
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

// Accented Latin (U+00C0 to U+017F) as its base letter, for the 5x7 font.
constexpr std::string_view latin_folds =
    "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty"
    "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiIiJjKkkLlLlLlLlLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";
static_assert(latin_folds.size() == 0x180 - 0xC0);

// The character the 5x7 font draws for `c`: itself in ASCII, a base letter
// for accented Latin, else nothing (0).
char bitmap_char(u32 c) {
    if (c < 0x80) {
        return static_cast<char>(c);
    }
    if (c >= 0xC0 && c < 0xC0 + latin_folds.size()) {
        return latin_folds[c - 0xC0];
    }
    return 0;
}

std::size_t character_count(std::string_view text) {
    std::size_t count = 0;
    for (std::size_t k = 0; k < text.size(); k = utf8_next(text, k)) {
        ++count;
    }
    return count;
}

f32 bitmap_text_width(std::string_view text, f32 scale) {
    if (text.empty()) {
        return 0.0f;
    }
    return static_cast<f32>(character_count(text) * 6 - 1) * scale;
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
        for (std::size_t k = 0; k < text.size(); k = utf8_next(text, k)) {
            const Glyph rows = glyph(bitmap_char(utf8_decode(text, k)));
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
        for (std::size_t k = 0; k < text.size(); k = utf8_next(text, k)) {
            const auto value = static_cast<unsigned char>(bitmap_char(utf8_decode(text, k)));
            if (value <= glyph_first || value > glyph_last) {
                cursor.x += static_cast<f32>(glyph_advance) * scale;
                continue;
            }
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
            cursor.x += static_cast<f32>(glyph_advance) * scale;
        }
        return true;
    }

    mutable std::vector<GlyphAtlas> _glyph_atlases;
    mutable u64 _atlas_tick = 0;
};

} // namespace

Font::Font(std::shared_ptr<const IFontBackend> backend)
    : _backend(std::move(backend)) {
}

Font bitmap_font() {
    static const Font font{std::make_shared<BitmapFontBackend>()};
    return font;
}

Font load_ttf_font(const std::filesystem::path& path, f32 point_size, const TtfFontOptions& options) {
    return Font{text_detail::make_ttf_backend(path, point_size, options)};
}

Font load_ttf_font(const std::filesystem::path& path, f32 point_size, f32 oversample, TextRendering rendering) {
    return load_ttf_font(path, point_size, TtfFontOptions{.oversample = oversample, .rendering = rendering});
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
    Font font = path.empty() ? bitmap_font()
                             : load_ttf_font(path, static_cast<f32>(clamped),
                                             TtfFontOptions{.rendering = rendering,
                                                            .fallbacks = system_fallback_fonts(),
                                                            .language_fallbacks = system_language_fonts()});
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

const std::vector<FontSource>& system_fallback_fonts() {
    static const std::vector<FontSource> fonts = [] {
        // Per script, the first that exists; scripts a UI font most often
        // lacks first. Fonts that cover several scripts come early.
        const std::vector<std::vector<const char*>> scripts{
            // Hebrew, Arabic, Greek, Cyrillic
            {"C:/Windows/Fonts/segoeui.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
             "/System/Library/Fonts/Supplemental/Arial Unicode.ttf"},
            {"C:/Windows/Fonts/tahoma.ttf", "/usr/share/fonts/truetype/noto/NotoSansArabic-Regular.ttf",
             "/usr/share/fonts/truetype/noto/NotoNaskhArabic-Regular.ttf", "/System/Library/Fonts/GeezaPro.ttc"},
            {"/usr/share/fonts/truetype/noto/NotoSansHebrew-Regular.ttf", "/System/Library/Fonts/SFHebrew.ttf"},
            // Chinese, Japanese, Korean
            {"C:/Windows/Fonts/msyh.ttc", "C:/Windows/Fonts/YuGothM.ttc", "C:/Windows/Fonts/meiryo.ttc",
             "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
             "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
             "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
             "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf", "/System/Library/Fonts/PingFang.ttc",
             "/System/Library/Fonts/Hiragino Sans GB.ttc"},
            {"C:/Windows/Fonts/malgun.ttf", "/usr/share/fonts/truetype/unfonts-core/UnDotum.ttf",
             "/System/Library/Fonts/AppleSDGothicNeo.ttc"},
            // Thai, Devanagari and the other Indic scripts
            {"C:/Windows/Fonts/leelawui.ttf", "/usr/share/fonts/truetype/noto/NotoSansThai-Regular.ttf",
             "/usr/share/fonts/truetype/tlwg/Garuda.ttf", "/System/Library/Fonts/Thonburi.ttc"},
            {"C:/Windows/Fonts/Nirmala.ttf", "/usr/share/fonts/truetype/noto/NotoSansDevanagari-Regular.ttf",
             "/usr/share/fonts/truetype/lohit-devanagari/Lohit-Devanagari.ttf",
             "/System/Library/Fonts/Kohinoor.ttc"},
        };
        std::vector<FontSource> found;
        for (const auto& candidates : scripts) {
            for (const char* candidate : candidates) {
                std::error_code error;
                if (std::filesystem::exists(candidate, error) &&
                    std::ranges::find(found, FontSource{candidate}) == found.end()) {
                    found.emplace_back(candidate);
                    break;
                }
            }
        }
        return found;
    }();
    return fonts;
}

const std::vector<LanguageFont>& system_language_fonts() {
    static const std::vector<LanguageFont> fonts = [] {
        struct Candidate {
            const char* path;
            const char* family; // in a collection, the face whose family has this
        };
        // The more particular tags first: a font chain takes every match in order.
        const std::vector<std::pair<const char*, std::vector<Candidate>>> languages{
            {"ja", {{"C:/Windows/Fonts/YuGothM.ttc", ""}, {"C:/Windows/Fonts/meiryo.ttc", ""},
                    {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", " JP"},
                    {"/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc", " JP"},
                    {"/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc", " JP"},
                    {"/usr/share/fonts/opentype/ipafont-gothic/ipag.ttf", ""},
                    {"/System/Library/Fonts/\xE3\x83\x92\xE3\x83\xA9\xE3\x82\xAE\xE3\x83\x8E\xE8\xA7\x92\xE3\x82\xB4\xE3\x82\xB7\xE3\x83\x83\xE3\x82\xAF W3.ttc", ""}}},
            {"ko", {{"C:/Windows/Fonts/malgun.ttf", ""},
                    {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", " KR"},
                    {"/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc", " KR"},
                    {"/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc", " KR"},
                    {"/usr/share/fonts/truetype/unfonts-core/UnDotum.ttf", ""},
                    {"/System/Library/Fonts/AppleSDGothicNeo.ttc", ""}}},
            {"zh-HK", {{"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", " HK"},
                       {"/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc", " HK"},
                       {"/System/Library/Fonts/PingFang.ttc", " HK"}}},
            {"zh-Hant", {{"C:/Windows/Fonts/msjh.ttc", ""},
                         {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", " TC"},
                         {"/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc", " TC"},
                         {"/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc", " TC"},
                         {"/System/Library/Fonts/PingFang.ttc", " TC"}}},
            {"zh", {{"C:/Windows/Fonts/msyh.ttc", ""},
                    {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", " SC"},
                    {"/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc", " SC"},
                    {"/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc", " SC"},
                    {"/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf", ""},
                    {"/System/Library/Fonts/PingFang.ttc", " SC"}}},
        };
        std::vector<LanguageFont> found;
        const auto add = [&](const char* language, const std::vector<Candidate>& candidates) {
            for (const Candidate& candidate : candidates) {
                std::error_code error;
                if (!std::filesystem::exists(candidate.path, error)) {
                    continue;
                }
                if (const std::optional<i32> face = text_detail::find_font_face(candidate.path, candidate.family)) {
                    found.push_back({language, FontSource{candidate.path, *face}});
                    return true;
                }
            }
            return false;
        };
        for (const auto& [language, candidates] : languages) {
            add(language, candidates);
        }
        // Traditional Chinese by region too (zh-TW, zh-MO), with the zh-Hant font.
        for (const LanguageFont& font : std::vector<LanguageFont>{found}) {
            if (font.language == "zh-Hant") {
                found.push_back({"zh-TW", font.font});
                found.push_back({"zh-MO", font.font});
            }
        }
        // More particular first.
        std::ranges::stable_sort(found, [](const LanguageFont& a, const LanguageFont& b) {
            return a.language.size() > b.language.size();
        });
        return found;
    }();
    return fonts;
}

Vec2f measure_text(const Font& font, std::string_view text, f32 scale) {
    const Font actual = font ? font : bitmap_font();
    return actual._backend->measure(text, scale);
}

// Whether a line may break between `before` and `after` with no space between
// them: next to CJK text, unless punctuation forbids it.
bool cjk_break(u32 before, u32 after) {
    return (text_detail::breaks_anywhere(before) || text_detail::breaks_anywhere(after)) &&
           !text_detail::no_break_before(after) && !text_detail::no_break_after(before);
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
    std::string_view glue;    // what joins `word` to the line: a space, or nothing between CJK characters
    bool spaced = false;      // whitespace since the last word
    u32 last = 0;             // the word's last character

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
            std::size_t best = utf8_next(value, begin); // a character at least
            while (best < value.size()) {
                const std::size_t end = utf8_next(value, best);
                if (!fits(value.substr(begin, end - begin))) {
                    break;
                }
                best = end;
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

        std::string candidate = line;
        if (!line.empty()) {
            candidate += glue;
        }
        candidate += word;
        if (fits(candidate)) {
            line = std::move(candidate);
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

    for (std::size_t k = 0; k < text.size();) {
        const std::size_t next = utf8_next(text, k);
        const u32 ch = utf8_decode(text, k);
        if (ch == '\n') {
            if (!flush_word()) {
                return lines;
            }
            if (!push_line(std::move(line))) {
                return lines;
            }
            line.clear();
            spaced = false;
        } else if (ch == ' ' || ch == '\t' || ch == '\r') {
            if (!flush_word()) {
                return lines;
            }
            spaced = true;
        } else {
            if (!word.empty() && cjk_break(last, ch) && !flush_word()) {
                return lines;
            }
            if (word.empty()) {
                glue = spaced ? std::string_view{" "} : std::string_view{};
                spaced = false;
            }
            word.append(text.substr(k, next - k));
            last = ch;
        }
        k = next;
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
                    // CJK text breaks between characters, as at whitespace.
                    if (i > pos && cjk_break(utf8_decode(text, utf8_prev(text, i)), utf8_decode(text, i))) {
                        after_space = i;
                    }
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

FittedText fit_text(const Font& font, std::string_view text, f32 max_width, f32 scale, TextFit fit, f32 min_scale) {
    FittedText out{std::string{text}, scale, false};
    if (text.empty() || measure_text(font, text, scale).x <= max_width) {
        return out;
    }
    if (fit == TextFit::Shrink) {
        const f32 width = measure_text(font, text, scale).x;
        const f32 smallest = scale * std::clamp(min_scale, 0.05f, 1.0f);
        out.scale = std::max(smallest, scale * max_width / std::max(width, 1.0f));
        out.changed = true;
        // Rounding may leave it a hair wide; step down until it fits.
        for (int i = 0; i < 4 && out.scale > smallest && measure_text(font, text, out.scale).x > max_width; ++i) {
            out.scale = std::max(smallest, out.scale * 0.98f);
        }
        if (measure_text(font, text, out.scale).x <= max_width) {
            return out;
        }
    }
    // The 5x7 font has no "…".
    const std::string_view ellipsis = font && font.identity() != bitmap_font().identity() ? "\xE2\x80\xA6" : "...";
    // The most characters that fit with the ellipsis: a binary search over the boundaries.
    std::vector<std::size_t> boundaries;
    for (std::size_t k = 0; k < text.size(); k = utf8_next(text, k)) {
        boundaries.push_back(k);
    }
    const auto fits = [&](std::size_t count) {
        std::string candidate{text.substr(0, boundaries[count])};
        while (!candidate.empty() && candidate.back() == ' ') {
            candidate.pop_back();
        }
        candidate += ellipsis;
        return measure_text(font, candidate, out.scale).x <= max_width;
    };
    std::size_t lo = 0;
    std::size_t hi = boundaries.size() - 1;
    while (lo < hi) {
        const std::size_t mid = (lo + hi + 1) / 2;
        if (fits(mid)) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    out.text.assign(text.substr(0, boundaries[lo]));
    while (!out.text.empty() && out.text.back() == ' ') {
        out.text.pop_back();
    }
    out.text += ellipsis;
    out.changed = true;
    return out;
}

namespace {

// A line's runs in display order, with where each starts and how wide it is.
struct VisualRun {
    std::size_t begin = 0;
    std::size_t end = 0;
    bool rtl = false;
    f32 x = 0.0f;
    f32 width = 0.0f;
};

std::vector<VisualRun> visual_runs(const Font& font, std::string_view line, f32 scale) {
    std::vector<VisualRun> runs;
    if (!has_right_to_left(line)) {
        runs.push_back({0, line.size(), false, 0.0f, measure_text(font, line, scale).x});
        return runs;
    }
    f32 x = 0.0f;
    for (const BidiRun& run : bidi_runs(line, text_base_direction())) {
        const f32 width = measure_text(font, line.substr(run.begin, run.end - run.begin), scale).x;
        runs.push_back({run.begin, run.end, run.right_to_left(), x, width});
        x += width;
    }
    return runs;
}

// The caret before `offset` (within [run.begin, run.end]) in `run`.
f32 caret_in_run(const Font& font, std::string_view line, const VisualRun& run, std::size_t offset, f32 scale) {
    const f32 prefix = offset <= run.begin ? 0.0f
        : offset >= run.end                ? run.width
                                           : measure_text(font, line.substr(run.begin, offset - run.begin), scale).x;
    return run.rtl ? run.x + run.width - prefix : run.x + prefix;
}

} // namespace

TextDirection paragraph_direction(std::string_view text) {
    if (const std::optional<TextDirection> base = text_base_direction()) {
        return *base;
    }
    return first_strong_direction(text);
}

f32 caret_x(const Font& font, std::string_view line, std::size_t offset, f32 scale) {
    offset = std::min(offset, line.size());
    const std::vector<VisualRun> runs = visual_runs(font, line, scale);
    if (runs.empty()) {
        return 0.0f;
    }
    // The run holding the character after the caret, else the one ending at it.
    const VisualRun* at = nullptr;
    for (const VisualRun& run : runs) {
        if (run.begin <= offset && offset < run.end) {
            at = &run;
            break;
        }
        if (run.end == offset) {
            at = &run;
        }
    }
    return at ? caret_in_run(font, line, *at, offset, scale) : 0.0f;
}

std::size_t caret_at(const Font& font, std::string_view line, f32 x, f32 scale) {
    std::size_t best = 0;
    f32 best_distance = std::numeric_limits<f32>::max();
    for (const VisualRun& run : visual_runs(font, line, scale)) {
        // The stops caret_x gives: a run's end only where the line ends (elsewhere
        // that offset belongs to the run after it).
        for (std::size_t k = run.begin;; k = utf8_next(line, k)) {
            if (k >= run.end && k < line.size()) {
                break;
            }
            const f32 distance = std::abs(caret_in_run(font, line, run, k, scale) - x);
            if (distance < best_distance) {
                best_distance = distance;
                best = k;
            }
            if (k >= run.end) {
                break;
            }
        }
    }
    return best;
}

std::optional<std::size_t> caret_move(const Font& font, std::string_view line, std::size_t offset, i32 step,
                                      f32 scale) {
    offset = std::min(offset, line.size());
    const f32 from = caret_x(font, line, offset, scale);
    std::optional<std::size_t> best;
    f32 best_x = 0.0f;
    for (const VisualRun& run : visual_runs(font, line, scale)) {
        for (std::size_t k = run.begin;; k = utf8_next(line, k)) {
            if (k >= run.end && k < line.size()) {
                break; // that offset belongs to the next run (as in caret_x)
            }
            const f32 x = caret_in_run(font, line, run, k, scale);
            const bool ahead = step < 0 ? x < from - 0.01f : x > from + 0.01f;
            if (k != offset && ahead && (!best || (step < 0 ? x > best_x : x < best_x))) {
                best = k;
                best_x = x;
            }
            if (k >= run.end) {
                break;
            }
        }
    }
    return best;
}

std::vector<std::pair<f32, f32>> selection_spans(const Font& font, std::string_view line, std::size_t begin,
                                                 std::size_t end, f32 scale) {
    std::vector<std::pair<f32, f32>> spans;
    end = std::min(end, line.size());
    if (begin >= end) {
        return spans;
    }
    for (const VisualRun& run : visual_runs(font, line, scale)) {
        const std::size_t a = std::max(begin, run.begin);
        const std::size_t b = std::min(end, run.end);
        if (a >= b) {
            continue;
        }
        const f32 xa = caret_in_run(font, line, run, a, scale);
        const f32 xb = caret_in_run(font, line, run, b, scale);
        const std::pair<f32, f32> span{std::min(xa, xb), std::max(xa, xb)};
        if (!spans.empty() && std::abs(spans.back().second - span.first) < 0.01f) {
            spans.back().second = span.second;
        } else {
            spans.push_back(span);
        }
    }
    return spans;
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
