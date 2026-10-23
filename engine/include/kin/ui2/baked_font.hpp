#pragma once

#include <kin/ui2/text.hpp>
#include <span>

namespace kin::ui2 {

struct BakedGlyphRect {
    i16 x, y, w, h;
};

struct BakedGlyph {
    u32 codepoint;
    BakedGlyphRect source;
    Vec2f offset;
    f32 advance;
};

struct BakedKerning {
    u32 left;
    u32 right;
    f32 adjustment;
};

// A pre-rasterized font: no files, image codecs, FreeType or shaping libraries.
// Alpha is encoded as pairs of (nonzero run length, coverage), row by row.
// Glyphs must be sorted by codepoint. Missing characters use '?' if provided.
// This is glyph-by-glyph layout for fixed alphabets; it does not shape scripts.
// The spans must outlive the returned Font (generated constexpr arrays do).
struct BakedFontData {
    Vec2i atlas_size;
    f32 line_height;
    std::span<const u8> alpha_rle;
    std::span<const BakedGlyph> glyphs;
    std::span<const BakedKerning> kerning;
};

Font baked_font(BakedFontData data);

} // namespace kin::ui2
