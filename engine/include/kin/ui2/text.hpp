#pragma once

#include <kin/core/bidi.hpp>
#include <kin/core/types.hpp>
#include <kin/renderer/color.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kin {
class Renderer2D;
}

namespace kin::ui2 {

using ::kin::Renderer2D;

// How a TTF font's glyphs are made.
//   Bitmap  rasterised, hinted, at each size drawn: the crispest small UI text,
//           but blurred when scaled by a transform or camera, and a new atlas
//           for every size (at most a few are kept).
//   Sdf     one atlas of signed distance fields: sharp at any size, scale, zoom
//           or turn, outlines nearly free; metrics scale linearly (unhinted).
//           Drawn as Bitmap on backends without distance fields
//           (capabilities().distance_fields).
enum class TextRendering : u8 { Bitmap, Sdf };

class IFontBackend {
public:
    virtual ~IFontBackend() = default;
    virtual Vec2f measure(std::string_view text, f32 scale) const = 0;
    virtual void draw(Renderer2D& renderer, std::string_view text, Vec2f pos, f32 scale, Color color) const = 0;
    // The text with an outline `outline` drawing units wide round it, drawn
    // in one go; false when the font cannot (draw_text_outlined then stamps it).
    virtual bool draw_outlined(Renderer2D& /*renderer*/, std::string_view /*text*/, Vec2f /*pos*/, f32 /*scale*/,
                               Color /*color*/, f32 /*outline*/, Color /*outline_color*/) const {
        return false;
    }
};

class Font {
public:
    Font() = default;
    explicit Font(std::shared_ptr<const IFontBackend> backend);

    bool valid() const { return _backend != nullptr; }
    explicit operator bool() const { return valid(); }

    // Stable identity for change detection (e.g. retained-UI re-measure when a
    // theme swaps fonts). Two Fonts measure identically if identities match.
    const void* identity() const { return _backend.get(); }

private:
    friend Vec2f measure_text(const Font& font, std::string_view text, f32 scale);
    friend void draw_text(Renderer2D& renderer, const Font& font, std::string_view text, Vec2f pos, f32 scale, Color color);
    friend void draw_text_outlined(Renderer2D& renderer, const Font& font, std::string_view text, Vec2f pos,
                                   f32 scale, Color color, f32 outline, Color outline_color);

    std::shared_ptr<const IFontBackend> _backend;
};

// The 5x7 pixel font: ASCII, with accented Latin letters drawn as their
// base letter and other characters left blank.
Font bitmap_font();

// A font file, and which face of it when it is a collection (.ttc).
struct FontSource {
    std::filesystem::path path;
    i32 face = 0;

    FontSource() = default;
    FontSource(std::filesystem::path p, i32 f = 0) : path(std::move(p)), face(f) {}
    FontSource(const char* p) : path(p) {}
    FontSource(const std::string& p) : path(p) {}
    friend bool operator==(const FontSource&, const FontSource&) = default;
};

// A fallback font for one language: Japanese, Chinese and Korean share code
// points but draw some differently, so each wants its own font.
struct LanguageFont {
    std::string language; // a tag or its start: "ja", "zh-Hans", "zh-Hant", "ko"
    FontSource font;
};

struct TtfFontOptions {
    // Rasterizes glyphs at point_size * oversample and draws them back at
    // point_size: pass the renderer's output/logical ratio (e.g. 2.46 for a
    // 960x540 logical canvas in a 2359-wide window) and text stays sharp
    // instead of being upscaled from logical-pixel glyphs. Measurements stay
    // logical. Matters to Bitmap only.
    f32 oversample = 1.0f;
    TextRendering rendering = TextRendering::Bitmap;
    // Fonts for the characters this one lacks, tried in order: a CJK, Arabic
    // or Hebrew font behind a Latin one (system_fallback_fonts() lists the
    // system's). Each opens only when a character needs it.
    std::vector<FontSource> fallbacks;
    // Tried before `fallbacks` while text_language() is (or starts with) their
    // language: the right font for Japanese, Chinese or Korean. The font
    // follows a change of language, as do themes made with it.
    std::vector<LanguageFont> language_fallbacks;
};

// A TrueType or OpenType font. Text is laid out glyph by glyph from an atlas
// that grows as new characters appear (any script without joining letters:
// Latin, Greek, Cyrillic, CJK, ...), kerned as the font says. Text that needs
// a shaper - right-to-left scripts, Arabic and Indic letters that join or
// reorder, combining marks - is ordered by the bidirectional algorithm and
// shaped by HarfBuzz run by run (each run drawn from a texture of its own).
Font load_ttf_font(const std::filesystem::path& path, f32 point_size, const TtfFontOptions& options);
Font load_ttf_font(const std::filesystem::path& path, f32 point_size, f32 oversample = 1.0f,
                   TextRendering rendering = TextRendering::Bitmap);

// Fonts installed with the system that cover the scripts a UI font often
// lacks (CJK, Arabic, Hebrew, Thai, Devanagari, ...), most useful first. Empty
// where none are found. system_ui_font falls back to them.
const std::vector<FontSource>& system_fallback_fonts();
// The system's fonts for Japanese, Simplified and Traditional Chinese and
// Korean, by language (the right face of a collection such as Noto Sans CJK),
// for TtfFontOptions::language_fallbacks. system_ui_font uses them.
const std::vector<LanguageFont>& system_language_fonts();

// The language text is shown in (a BCP 47 tag, "" for none): fonts choose
// their language_fallbacks by it, and HarfBuzz shapes with it (forms that
// differ by language). run_scene_app sets it from the localization.
void set_text_language(std::string_view language);
std::string text_language();

// The direction of a paragraph of text whose characters run both ways (see
// kin/core/bidi.hpp): unset, each line's first strong character decides, as
// the Unicode algorithm does; a right-to-left UI sets RightToLeft so a line
// starting with a Latin name still reads right to left. Applies to every
// TTF font's layout.
void set_text_base_direction(std::optional<TextDirection> direction);
std::optional<TextDirection> text_base_direction();
// The direction `text` lays out in: text_base_direction(), else that of its
// first strong character (left to right if it has none).
TextDirection paragraph_direction(std::string_view text);

// How text that is too wide for its room is made to fit.
//   Ellipsis  cut at a character and ended with "…" (its logical end: the
//             left end of right-to-left text)
//   Shrink    drawn smaller, down to min_scale of its scale, then cut as Ellipsis
enum class TextFit : u8 { Ellipsis, Shrink };

struct FittedText {
    std::string text; // what to draw
    f32 scale = 1.0f; // at what scale
    bool changed = false; // cut or shrunk
};

// `text` (one line) fitted into `max_width` drawing units.
FittedText fit_text(const Font& font, std::string_view text, f32 max_width, f32 scale, TextFit fit,
                    f32 min_scale = 0.7f);

// Carets and selections in one line of text, wherever its characters fall when
// it runs both ways. Offsets are bytes of `line` at character boundaries; x is
// in drawing units from where draw_text puts the line's start. Within a
// right-to-left run the caret moves leftwards as the offset grows.
f32 caret_x(const Font& font, std::string_view line, std::size_t offset, f32 scale = 1.0f);
// The caret offset whose x is nearest `x`.
std::size_t caret_at(const Font& font, std::string_view line, f32 x, f32 scale = 1.0f);
// The caret one step left (`step` < 0) or right of `offset` on screen, as the
// arrow keys move it through text that runs both ways; nullopt at that end
// of the line.
std::optional<std::size_t> caret_move(const Font& font, std::string_view line, std::size_t offset, i32 step,
                                      f32 scale = 1.0f);
// The spans [x0, x1) the selection [begin, end) of `line` covers, left to
// right: one where the line runs one way, several where directions mix.
std::vector<std::pair<f32, f32>> selection_spans(const Font& font, std::string_view line, std::size_t begin,
                                                 std::size_t end, f32 scale = 1.0f);

// System UI font for professional-looking interfaces: tries the platform's
// standard sans (Segoe UI / Arial / DejaVu / Liberation), cached per size,
// falling back to bitmap_font() when no TTF is found. `_bold` returns the
// matching bold cut (for title/emphasis tiers); falls back to the regular cut,
// then to the bitmap font. Sizes are clamped to [8, 32]pt.
Font system_ui_font(f32 point_size, TextRendering rendering = TextRendering::Bitmap);
Font system_ui_font_bold(f32 point_size, TextRendering rendering = TextRendering::Bitmap);
bool system_ui_font_available(); // true when a real TTF backs system_ui_font

struct TextWrapOptions {
    f32 max_width = 0.0f;
    f32 scale = 1.0f;
    f32 line_spacing = 0.0f;
    i32 max_lines = -1;
    bool break_long_words = true;
};

// A byte range [begin, end) of a text.
struct TextRange {
    std::size_t begin = 0;
    std::size_t end = 0;

    friend constexpr bool operator==(TextRange, TextRange) = default;
};

Vec2f measure_text(const Font& font, std::string_view text, f32 scale = 1.0f);
std::vector<std::string> wrap_text(const Font& font, std::string_view text, TextWrapOptions options);
// wrap_text for editors: each displayed line as a byte range of `text` rather than
// a copy, and nothing dropped. Lines break at '\n' (which no range includes), then
// a line wider than max_width breaks after the last whitespace that fits; that
// whitespace stays at the end of its line. A word too wide for a line of its own
// breaks between characters when break_long_words, else overflows. CJK text
// also breaks between characters (not before closing punctuation or after
// opening brackets). max_width <= 0 breaks at '\n' only. An empty text is one
// empty line, and text ending in '\n' ends with an empty line. Widths are
// summed per character, ignoring kerning.
std::vector<TextRange> wrap_text_ranges(const Font& font, std::string_view text, TextWrapOptions options);
// Lines break at '\n', then at spaces, and between CJK characters as
// wrap_text_ranges does; the spaces at a break are dropped.
std::vector<std::string> wrap_text(const Font& font, std::string_view text, f32 max_width, f32 scale = 1.0f);
std::vector<std::string> wrap_text(std::string_view text, f32 max_width, f32 scale = 1.0f);
Vec2f measure_wrapped_text(const Font& font, std::string_view text, TextWrapOptions options);
Vec2f measure_wrapped_text(std::string_view text, TextWrapOptions options);
void draw_text(Renderer2D& renderer,
               const Font& font,
               std::string_view text,
               Vec2f pos,
               f32 scale,
               Color color);
void draw_text(Renderer2D& renderer, std::string_view text, Vec2f pos, f32 scale, Color color);
// The text with an outline `outline` drawing units wide round it: from the
// distance field in one pass with an Sdf font, else four copies offset
// left, right, up and down under the text.
void draw_text_outlined(Renderer2D& renderer,
                        const Font& font,
                        std::string_view text,
                        Vec2f pos,
                        f32 scale,
                        Color color,
                        f32 outline,
                        Color outline_color);
void draw_text_centered(Renderer2D& renderer,
                        const Font& font,
                        std::string_view text,
                        Vec2f center,
                        f32 scale,
                        Color color);
void draw_text_centered(Renderer2D& renderer, std::string_view text, Vec2f center, f32 scale, Color color);
void draw_wrapped_text(Renderer2D& renderer,
                       const Font& font,
                       std::string_view text,
                       Vec2f pos,
                       TextWrapOptions options,
                       Color color);
void draw_wrapped_text(Renderer2D& renderer,
                       std::string_view text,
                       Vec2f pos,
                       TextWrapOptions options,
                       Color color);

} // namespace kin::ui2
