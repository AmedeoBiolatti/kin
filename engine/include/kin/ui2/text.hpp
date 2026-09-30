#pragma once

#include <kin/core/types.hpp>
#include <kin/renderer/color.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace kin {
class Renderer2D;
}

namespace kin::ui2 {

using ::kin::Renderer2D;

class IFontBackend {
public:
    virtual ~IFontBackend() = default;
    virtual Vec2f measure(std::string_view text, f32 scale) const = 0;
    virtual void draw(Renderer2D& renderer, std::string_view text, Vec2f pos, f32 scale, Color color) const = 0;
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

    std::shared_ptr<const IFontBackend> _backend;
};

Font bitmap_font();
// `oversample` rasterizes glyphs at point_size * oversample and draws them
// back at point_size: pass the renderer's output/logical ratio (e.g. 2.46 for a
// 960x540 logical canvas in a 2359-wide window) and text stays sharp instead
// of being upscaled from logical-pixel glyphs. Measurements stay logical.
Font load_ttf_font(const std::filesystem::path& path, f32 point_size, f32 oversample = 1.0f);

// System UI font for professional-looking interfaces: tries the platform's
// standard sans (Segoe UI / Arial / DejaVu / Liberation), cached per size,
// falling back to bitmap_font() when no TTF is found. `_bold` returns the
// matching bold cut (for title/emphasis tiers); falls back to the regular cut,
// then to the bitmap font. Sizes are clamped to [8, 32]pt.
Font system_ui_font(f32 point_size);
Font system_ui_font_bold(f32 point_size);
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
// breaks between characters when break_long_words, else overflows. max_width <= 0
// breaks at '\n' only. An empty text is one empty line, and text ending in '\n'
// ends with an empty line. Widths are summed per character, ignoring kerning.
std::vector<TextRange> wrap_text_ranges(const Font& font, std::string_view text, TextWrapOptions options);
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
