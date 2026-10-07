#pragma once

// What ui2's text code shares between the public functions (text.cpp), the
// TTF backend (text_ttf.cpp) and the character classes both use
// (text_script.cpp).

#include <kin/core/types.hpp>
#include <kin/ui2/text.hpp>

#include <memory>
#include <optional>
#include <string_view>

namespace kin::ui2::text_detail {

// Characters a glyph-by-glyph layout cannot place: marks that combine with
// the letter before, letters that join or reorder (Arabic, the Indic and
// South-East Asian scripts), the right-to-left scripts, and the invisible
// characters that steer shaping or direction. Text with any of them is shaped
// by HarfBuzz, run by run.
bool needs_shaping(u32 c);

// Characters a line may break before or after without a space: CJK
// ideographs, kana, Hangul syllables and the full-width forms.
bool breaks_anywhere(u32 c);
// Closing punctuation, small kana and the like: never the first on a line.
bool no_break_before(u32 c);
// Opening brackets and quotes: never the last on a line.
bool no_break_after(u32 c);

// Pairs a TrueType font is worth asking to kern: below the CJK blocks.
inline bool kernable(u32 c) { return c < 0x2E80; }

// The face of a font collection whose family name contains `family`
// (" JP", " SC"), or nullopt; an empty `family` takes face 0 of a file that opens.
std::optional<i32> find_font_face(const std::filesystem::path& path, std::string_view family);

std::shared_ptr<const IFontBackend> make_ttf_backend(const std::filesystem::path& path, f32 point_size,
                                                     const TtfFontOptions& options);

} // namespace kin::ui2::text_detail
