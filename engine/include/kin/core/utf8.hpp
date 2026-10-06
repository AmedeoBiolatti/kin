#pragma once

#include <kin/core/types.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace kin {

// Character-safe stepping through UTF-8 text by byte offset. A boundary is an
// offset at the start of a character (or the end of the text); these never land
// inside a multi-byte character, so editing at the offsets they return keeps the
// text valid. Malformed bytes count as one character each.

// The next boundary after `pos` (the end of the text at most).
std::size_t utf8_next(std::string_view text, std::size_t pos);
// The previous boundary before `pos` (0 at least).
std::size_t utf8_prev(std::string_view text, std::size_t pos);
// `pos` moved back to a boundary if it is inside a character; clamped to the text.
std::size_t utf8_floor(std::string_view text, std::size_t pos);
// The code point starting at `pos` (U+FFFD for malformed bytes, 0 at the end).
u32 utf8_decode(std::string_view text, std::size_t pos);
// `cp` written to the end of `out` (U+FFFD for a surrogate or beyond U+10FFFF).
void utf8_append(std::string& out, u32 cp);

// Word stepping for editors. Characters are whitespace, word characters (letters,
// digits, '_' and anything outside ASCII) or punctuation; a word is a run of one
// kind other than whitespace.
// Start of the word before `pos`: skips whitespace back, then the word.
std::size_t utf8_word_left(std::string_view text, std::size_t pos);
// Start of the next word after `pos`: skips the word at `pos`, then whitespace.
std::size_t utf8_word_right(std::string_view text, std::size_t pos);
// The run of one kind around `pos` (a word, punctuation or whitespace), as
// [begin, end): what a double-click selects.
std::pair<std::size_t, std::size_t> utf8_word_at(std::string_view text, std::size_t pos);

// Windows (\r\n) and old Mac (\r) line endings turned into \n.
std::string normalize_newlines(std::string_view text);

} // namespace kin
