#include "text_internal.hpp"

#include <algorithm>
#include <iterator>

namespace kin::ui2::text_detail {
namespace {

bool in(u32 c, u32 lo, u32 hi) { return c >= lo && c <= hi; }

} // namespace

bool needs_shaping(u32 c) {
    if (c < 0x0300) {
        return false;
    }
    return in(c, 0x0300, 0x036F)      // combining diacritical marks
        || in(c, 0x0483, 0x0489)      // Cyrillic combining marks
        || in(c, 0x0590, 0x08FF)      // Hebrew, Arabic, Syriac, Thaana, NKo, Samaritan, Mandaic
        || in(c, 0x0900, 0x0DFF)      // Devanagari to Sinhala
        || in(c, 0x0E00, 0x0FFF)      // Thai, Lao, Tibetan
        || in(c, 0x1000, 0x109F)      // Myanmar
        || in(c, 0x1100, 0x11FF)      // conjoining Hangul jamo
        || in(c, 0x1700, 0x18AF)      // Philippine scripts, Khmer, Mongolian
        || in(c, 0x1900, 0x1AFF)      // Limbu to Tai Tham, combining marks extended
        || in(c, 0x1B00, 0x1C4F)      // Balinese to Lepcha
        || in(c, 0x1CD0, 0x1CFF)      // Vedic extensions
        || in(c, 0x1DC0, 0x1DFF)      // combining marks supplement
        || in(c, 0x200B, 0x200F)      // zero-width space and joiners, direction marks
        || in(c, 0x202A, 0x202E)      // embeddings and overrides
        || in(c, 0x2066, 0x2069)      // isolates
        || in(c, 0x20D0, 0x20FF)      // combining marks for symbols
        || in(c, 0x302A, 0x302F)      // ideographic tone marks
        || in(c, 0x3099, 0x309A)      // combining kana voicing
        || in(c, 0xA800, 0xA8FF)      // Syloti Nagri to Devanagari extended
        || in(c, 0xA900, 0xAAFF)      // Kayah Li to Myanmar extended
        || in(c, 0xABC0, 0xABFF)      // Meetei Mayek
        || in(c, 0xFB1D, 0xFDFF)      // Hebrew and Arabic presentation forms
        || in(c, 0xFE00, 0xFE0F)      // variation selectors
        || in(c, 0xFE20, 0xFE2F)      // combining half marks
        || in(c, 0xFE70, 0xFEFF)      // Arabic presentation forms B
        || in(c, 0x10800, 0x10FFF)    // historic right-to-left scripts
        || in(c, 0x11000, 0x11FFF)    // Brahmi and the other historic Indic scripts
        || in(c, 0x1E800, 0x1EFFF)    // Mende Kikakui, Adlam, Arabic mathematical letters
        || in(c, 0x1F3FB, 0x1F3FF)    // skin tone modifiers
        || in(c, 0xE0000, 0xE01EF);   // tags, variation selectors supplement
}

bool breaks_anywhere(u32 c) {
    return in(c, 0x2E80, 0x2FDF)      // CJK radicals, Kangxi
        || in(c, 0x3000, 0x303F)      // CJK symbols and punctuation
        || in(c, 0x3040, 0x30FF)      // hiragana, katakana
        || in(c, 0x3100, 0x31FF)      // bopomofo, Hangul compatibility jamo, katakana extensions
        || in(c, 0x3200, 0x33FF)      // enclosed CJK, compatibility
        || in(c, 0x3400, 0x4DBF)      // CJK extension A
        || in(c, 0x4E00, 0x9FFF)      // CJK unified ideographs
        || in(c, 0xA000, 0xA4CF)      // Yi
        || in(c, 0xAC00, 0xD7AF)      // Hangul syllables
        || in(c, 0xF900, 0xFAFF)      // CJK compatibility ideographs
        || in(c, 0xFE30, 0xFE4F)      // CJK compatibility forms
        || in(c, 0xFF00, 0xFFEF)      // full-width and half-width forms
        || in(c, 0x20000, 0x3FFFF);   // CJK extensions B and beyond
}

bool no_break_before(u32 c) {
    static constexpr u32 chars[] = {
        // ASCII closing punctuation (when it follows CJK text)
        ')', ']', '}', ',', '.', ':', ';', '?', '!', '%',
        0x00BB, 0x2019, 0x201D, 0x2026, 0x2025, 0x2010, 0x2013, 0x203A, 0x2030,
        // CJK punctuation and closing brackets
        0x3001, 0x3002, 0x3005, 0x3009, 0x300B, 0x300D, 0x300F, 0x3011, 0x3015, 0x3017, 0x3019, 0x301B, 0x301E,
        0x301F, 0x303B, 0x30A0, 0x30FB, 0x30FC, 0x30FD, 0x30FE, 0x309D, 0x309E,
        // full-width punctuation
        0xFF01, 0xFF09, 0xFF0C, 0xFF0E, 0xFF1A, 0xFF1B, 0xFF1F, 0xFF3D, 0xFF5D, 0xFF60, 0xFF61, 0xFF63, 0xFF64,
        0xFF65, 0xFF70, 0xFF5E, 0x301C,
        // small kana
        0x3041, 0x3043, 0x3045, 0x3047, 0x3049, 0x3063, 0x3083, 0x3085, 0x3087,
    };
    if (std::ranges::find(chars, c) != std::end(chars)) {
        return true;
    }
    // The rest of the small kana: ゎゕゖ, ァィゥェォッャュョヮヵヶ, ㇰ-ㇿ, ｧ-ｯ.
    return c == 0x308E || in(c, 0x3095, 0x3096) || c == 0x30A1 || c == 0x30A3 || c == 0x30A5 || c == 0x30A7 ||
           c == 0x30A9 || c == 0x30C3 || c == 0x30E3 || c == 0x30E5 || c == 0x30E7 || c == 0x30EE ||
           in(c, 0x30F5, 0x30F6) || in(c, 0x31F0, 0x31FF) || in(c, 0xFF67, 0xFF6F);
}

bool no_break_after(u32 c) {
    static constexpr u32 chars[] = {
        '(', '[', '{', 0x00AB, 0x2018, 0x201C, 0x2039,
        0x3008, 0x300A, 0x300C, 0x300E, 0x3010, 0x3014, 0x3016, 0x3018, 0x301A, 0x301D,
        0xFF08, 0xFF3B, 0xFF5B, 0xFF5F, 0xFF62, 0xFF04, 0xFFE1, 0xFFE5, 0xFFE6, 0x20AC, 0x00A3,
    };
    return std::ranges::find(chars, c) != std::end(chars);
}

} // namespace kin::ui2::text_detail
