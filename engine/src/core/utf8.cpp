#include <kin/core/utf8.hpp>

#include <algorithm>

namespace kin {

namespace {

bool continuation(char byte) {
    return (static_cast<unsigned char>(byte) & 0xC0u) == 0x80u;
}

// Length of the character starting with `lead`, as its lead byte declares.
std::size_t declared_length(unsigned char lead) {
    if (lead < 0x80u) {
        return 1;
    }
    if ((lead & 0xE0u) == 0xC0u) {
        return 2;
    }
    if ((lead & 0xF0u) == 0xE0u) {
        return 3;
    }
    if ((lead & 0xF8u) == 0xF0u) {
        return 4;
    }
    return 1; // a stray continuation or invalid lead byte stands alone
}

enum class CharKind { Space, Word, Punct };

CharKind kind_of(u32 cp) {
    if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == '\v' || cp == '\f') {
        return CharKind::Space;
    }
    if (cp >= 0x80u || cp == '_' || (cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z')) {
        return CharKind::Word;
    }
    return CharKind::Punct;
}

CharKind kind_at(std::string_view text, std::size_t pos) {
    return kind_of(utf8_decode(text, pos));
}

} // namespace

std::size_t utf8_next(std::string_view text, std::size_t pos) {
    if (pos >= text.size()) {
        return text.size();
    }
    const std::size_t length = declared_length(static_cast<unsigned char>(text[pos]));
    std::size_t next = pos + 1;
    // Take the declared continuation bytes that are actually there.
    while (next < text.size() && next < pos + length && continuation(text[next])) {
        ++next;
    }
    return next;
}

std::size_t utf8_prev(std::string_view text, std::size_t pos) {
    pos = std::min(pos, text.size());
    if (pos == 0) {
        return 0;
    }
    std::size_t prev = pos - 1;
    // Back over up to three continuation bytes to a lead byte whose character
    // really ends at `pos`; otherwise the last byte stands alone.
    std::size_t lead = prev;
    while (lead > 0 && pos - lead < 4 && continuation(text[lead])) {
        --lead;
    }
    if (lead != prev && utf8_next(text, lead) == pos) {
        return lead;
    }
    return prev;
}

std::size_t utf8_floor(std::string_view text, std::size_t pos) {
    if (pos >= text.size()) {
        return text.size();
    }
    std::size_t start = pos;
    while (start > 0 && pos - start < 3 && continuation(text[start])) {
        --start;
    }
    return utf8_next(text, start) > pos ? start : pos;
}

u32 utf8_decode(std::string_view text, std::size_t pos) {
    if (pos >= text.size()) {
        return 0;
    }
    const auto lead = static_cast<unsigned char>(text[pos]);
    const std::size_t length = utf8_next(text, pos) - pos;
    if (length != declared_length(lead) || (length == 1 && lead >= 0x80u)) {
        return 0xFFFDu;
    }
    if (length == 1) {
        return lead;
    }
    u32 cp = lead & (0x7Fu >> length);
    for (std::size_t i = 1; i < length; ++i) {
        cp = (cp << 6) | (static_cast<unsigned char>(text[pos + i]) & 0x3Fu);
    }
    return cp;
}

void utf8_append(std::string& out, u32 cp) {
    if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) {
        cp = 0xFFFDu;
    }
    if (cp < 0x80u) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800u) {
        out += static_cast<char>(0xC0u | (cp >> 6));
        out += static_cast<char>(0x80u | (cp & 0x3Fu));
    } else if (cp < 0x10000u) {
        out += static_cast<char>(0xE0u | (cp >> 12));
        out += static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
        out += static_cast<char>(0x80u | (cp & 0x3Fu));
    } else {
        out += static_cast<char>(0xF0u | (cp >> 18));
        out += static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu));
        out += static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
        out += static_cast<char>(0x80u | (cp & 0x3Fu));
    }
}

std::size_t utf8_word_left(std::string_view text, std::size_t pos) {
    pos = utf8_floor(text, pos);
    while (pos > 0 && kind_at(text, utf8_prev(text, pos)) == CharKind::Space) {
        pos = utf8_prev(text, pos);
    }
    if (pos == 0) {
        return 0;
    }
    const CharKind kind = kind_at(text, utf8_prev(text, pos));
    while (pos > 0 && kind_at(text, utf8_prev(text, pos)) == kind) {
        pos = utf8_prev(text, pos);
    }
    return pos;
}

std::size_t utf8_word_right(std::string_view text, std::size_t pos) {
    pos = utf8_floor(text, pos);
    if (pos < text.size()) {
        const CharKind kind = kind_at(text, pos);
        if (kind != CharKind::Space) {
            while (pos < text.size() && kind_at(text, pos) == kind) {
                pos = utf8_next(text, pos);
            }
        }
    }
    while (pos < text.size() && kind_at(text, pos) == CharKind::Space) {
        pos = utf8_next(text, pos);
    }
    return pos;
}

std::pair<std::size_t, std::size_t> utf8_word_at(std::string_view text, std::size_t pos) {
    pos = utf8_floor(text, pos);
    if (text.empty()) {
        return {0, 0};
    }
    // At the very end, select the run that ends there.
    if (pos == text.size()) {
        pos = utf8_prev(text, pos);
    }
    const CharKind kind = kind_at(text, pos);
    std::size_t begin = pos;
    while (begin > 0 && kind_at(text, utf8_prev(text, begin)) == kind) {
        begin = utf8_prev(text, begin);
    }
    std::size_t end = pos;
    while (end < text.size() && kind_at(text, end) == kind) {
        end = utf8_next(text, end);
    }
    return {begin, end};
}

std::string normalize_newlines(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            out.push_back('\n');
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                ++i;
            }
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

namespace {

// A code point that goes on the one before it: a combining mark (the common
// blocks, and the marks of the scripts UI text meets most), a variation
// selector, a skin tone, a tag, a keycap's frame.
bool extends(u32 cp) {
    struct Range {
        u32 first;
        u32 last;
    };
    static constexpr Range ranges[] = {
        {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x05BF, 0x05BF}, {0x05C1, 0x05C2}, {0x05C4, 0x05C5},
        {0x05C7, 0x05C7}, {0x0610, 0x061A}, {0x064B, 0x065F}, {0x0670, 0x0670}, {0x06D6, 0x06DC}, {0x06DF, 0x06E4},
        {0x06E7, 0x06E8}, {0x06EA, 0x06ED}, {0x0900, 0x0903}, {0x093A, 0x094F}, {0x0951, 0x0957}, {0x0962, 0x0963},
        {0x0981, 0x0983}, {0x09BC, 0x09D7}, {0x0E31, 0x0E31}, {0x0E34, 0x0E3A}, {0x0E47, 0x0E4E}, {0x1AB0, 0x1AFF},
        {0x1DC0, 0x1DFF}, {0x20D0, 0x20FF}, {0xFE00, 0xFE0F}, {0xFE20, 0xFE2F}, {0x1F3FB, 0x1F3FF}, {0xE0020, 0xE007F},
        {0xE0100, 0xE01EF},
    };
    return std::ranges::any_of(ranges, [cp](const Range& r) { return cp >= r.first && cp <= r.last; });
}

bool regional_indicator(u32 cp) {
    return cp >= 0x1F1E6 && cp <= 0x1F1FF;
}

constexpr u32 zwj = 0x200D;

} // namespace

std::size_t utf8_next_grapheme(std::string_view text, std::size_t pos) {
    if (pos >= text.size()) {
        return text.size();
    }
    if (text[pos] == '\r' && pos + 1 < text.size() && text[pos + 1] == '\n') {
        return pos + 2;
    }
    const u32 first = utf8_decode(text, pos);
    std::size_t at = utf8_next(text, pos);
    if (first == '\n' || first == '\r') {
        return at;
    }
    if (regional_indicator(first) && at < text.size() && regional_indicator(utf8_decode(text, at))) {
        return utf8_next(text, at); // a flag
    }
    while (at < text.size()) {
        const u32 cp = utf8_decode(text, at);
        if (cp == zwj) {
            at = utf8_next(text, at);
            if (at < text.size()) {
                at = utf8_next(text, at); // what it joins on
            }
            continue;
        }
        if (!extends(cp)) {
            break;
        }
        at = utf8_next(text, at);
    }
    return at;
}

std::size_t utf8_prev_grapheme(std::string_view text, std::size_t pos) {
    pos = std::min(pos, text.size());
    if (pos == 0) {
        return 0;
    }
    // From a boundary surely before it (an ASCII character not joined on to the
    // one before it, or the text's start), forward to the last one before `pos`.
    std::size_t start = utf8_prev(text, pos);
    for (int steps = 0; start > 0 && steps < 64; ++steps) {
        const auto byte = static_cast<unsigned char>(text[start]);
        const std::size_t before = utf8_prev(text, start);
        if (byte < 0x80 && byte != '\n' && utf8_decode(text, before) != zwj) {
            break;
        }
        start = before;
    }
    std::size_t at = start;
    for (;;) {
        const std::size_t next = utf8_next_grapheme(text, at);
        if (next >= pos || next == at) {
            return at;
        }
        at = next;
    }
}

} // namespace kin
