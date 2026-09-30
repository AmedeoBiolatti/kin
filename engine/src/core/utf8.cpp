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

} // namespace kin
