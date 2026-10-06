#include <kin/core/bidi.hpp>

#include <kin/core/utf8.hpp>

#include <algorithm>

namespace kin {
namespace {

// Bidi classes (UAX #9, table 4).
enum class Bc : u8 {
    L, R, AL, EN, ES, ET, AN, CS, NSM, BN, B, S, WS, ON,
    LRE, LRO, RLE, RLO, PDF, LRI, RLI, FSI, PDI,
};

bool in(u32 c, u32 lo, u32 hi) { return c >= lo && c <= hi; }

// The bidi class of a code point, from the ranges UI text meets: Latin,
// Greek, Cyrillic, CJK and the other left-to-right scripts, the right-to-left
// scripts with their marks and digits, numbers, punctuation and the explicit
// formatting characters. Rare symbols outside these count as L or ON.
Bc bidi_class(u32 c) {
    if (c < 0x80) {
        if (c == 0x09 || c == 0x0B || c == 0x1F) return Bc::S;
        if (c == 0x0A || c == 0x0D || in(c, 0x1C, 0x1E)) return Bc::B;
        if (c == 0x0C || c == 0x20) return Bc::WS;
        if (c < 0x20 || c == 0x7F) return Bc::BN;
        if (in(c, '0', '9')) return Bc::EN;
        if (c == '+' || c == '-') return Bc::ES;
        if (c == '#' || c == '$' || c == '%') return Bc::ET;
        if (c == ',' || c == '.' || c == '/' || c == ':') return Bc::CS;
        if (in(c, 'A', 'Z') || in(c, 'a', 'z')) return Bc::L;
        return Bc::ON;
    }
    if (c <= 0x00FF) {
        if (c == 0x85) return Bc::B;
        if (c < 0xA0) return Bc::BN;
        if (c == 0xA0) return Bc::CS;
        if (c == 0xAD) return Bc::BN;
        if (in(c, 0xA2, 0xA5) || in(c, 0xB0, 0xB1)) return Bc::ET;
        if (in(c, 0xB2, 0xB3) || c == 0xB9) return Bc::EN;
        if (c == 0xAA || c == 0xB5 || c == 0xBA) return Bc::L;
        if (c < 0xC0 || c == 0xD7 || c == 0xF7) return Bc::ON;
        return Bc::L;
    }
    if (in(c, 0x0300, 0x036F) || in(c, 0x0483, 0x0489)) return Bc::NSM;
    if (in(c, 0x0590, 0x05FF)) {
        if (in(c, 0x0591, 0x05BD) || c == 0x05BF || in(c, 0x05C1, 0x05C2) || in(c, 0x05C4, 0x05C5) || c == 0x05C7) {
            return Bc::NSM;
        }
        return Bc::R;
    }
    if (in(c, 0x0600, 0x06FF)) {
        if (in(c, 0x0600, 0x0605) || in(c, 0x0660, 0x0669) || in(c, 0x066B, 0x066C) || c == 0x06DD) return Bc::AN;
        if (c == 0x060C) return Bc::CS;
        if (c == 0x066A) return Bc::ET;
        if (in(c, 0x06F0, 0x06F9)) return Bc::EN;
        if (in(c, 0x0610, 0x061A) || in(c, 0x064B, 0x065F) || c == 0x0670 || in(c, 0x06D6, 0x06DC) ||
            in(c, 0x06DF, 0x06E4) || in(c, 0x06E7, 0x06E8) || in(c, 0x06EA, 0x06ED)) {
            return Bc::NSM;
        }
        return Bc::AL;
    }
    if (in(c, 0x0700, 0x08FF)) {
        if (c == 0x0711 || in(c, 0x0730, 0x074A) || in(c, 0x07A6, 0x07B0) || in(c, 0x07EB, 0x07F3) ||
            in(c, 0x0816, 0x082D) || in(c, 0x0859, 0x085B) || in(c, 0x0898, 0x089F) || in(c, 0x08CA, 0x08E1) ||
            in(c, 0x08E3, 0x08FF)) {
            return Bc::NSM;
        }
        if (c == 0x08E2) return Bc::AN;
        if (in(c, 0x07C0, 0x085F)) return Bc::R; // NKo, Samaritan, Mandaic
        return Bc::AL;                           // Syriac, Thaana, Arabic supplements
    }
    if (c == 0x061C) return Bc::AL;
    if (in(c, 0x2000, 0x200A) || c == 0x2028 || c == 0x205F || c == 0x3000) return Bc::WS;
    if (c == 0x2029) return Bc::B;
    if (in(c, 0x200B, 0x200D) || in(c, 0x2060, 0x2064) || c == 0xFEFF) return Bc::BN;
    if (c == 0x200E) return Bc::L;
    if (c == 0x200F) return Bc::R;
    switch (c) {
    case 0x202A: return Bc::LRE;
    case 0x202B: return Bc::RLE;
    case 0x202C: return Bc::PDF;
    case 0x202D: return Bc::LRO;
    case 0x202E: return Bc::RLO;
    case 0x2066: return Bc::LRI;
    case 0x2067: return Bc::RLI;
    case 0x2068: return Bc::FSI;
    case 0x2069: return Bc::PDI;
    case 0x202F: return Bc::CS;
    case 0x2044: return Bc::CS;
    case 0x2212: return Bc::ES;
    case 0x2213: return Bc::ET;
    default: break;
    }
    if (in(c, 0x2030, 0x2034)) return Bc::ET;
    if (in(c, 0x2010, 0x205E)) return Bc::ON;
    if (c == 0x2070 || in(c, 0x2074, 0x2079) || in(c, 0x2080, 0x2089)) return Bc::EN;
    if (in(c, 0x207A, 0x207B) || in(c, 0x208A, 0x208B)) return Bc::ES;
    if (in(c, 0x20A0, 0x20CF)) return Bc::ET;
    if (in(c, 0x20D0, 0x20FF)) return Bc::NSM;
    if (in(c, 0x2100, 0x2BFF)) {
        // Letterlike symbols, number forms, arrows, maths, technical,
        // pictographs, boxes: neutral, bar a few letters.
        if (c == 0x2102 || c == 0x2107 || in(c, 0x210A, 0x2113) || c == 0x2115 || in(c, 0x2119, 0x211D) ||
            c == 0x2124 || c == 0x2126 || c == 0x2128 || in(c, 0x212A, 0x212D) || in(c, 0x212F, 0x2139) ||
            in(c, 0x2160, 0x2188)) {
            return Bc::L;
        }
        return Bc::ON;
    }
    if (in(c, 0x2E00, 0x2E7F) || in(c, 0x3001, 0x3004) || in(c, 0x3008, 0x3020) || c == 0x3030 || c == 0x303D ||
        in(c, 0x303E, 0x303F) || c == 0x30A0 || c == 0x30FB) {
        return Bc::ON;
    }
    if (in(c, 0xFB1D, 0xFB4F)) {
        if (c == 0xFB1E) return Bc::NSM;
        if (c == 0xFB29) return Bc::ES;
        return Bc::R;
    }
    if (in(c, 0xFB50, 0xFDFF)) return in(c, 0xFD3E, 0xFD3F) ? Bc::ON : Bc::AL;
    if (in(c, 0xFE00, 0xFE0F) || in(c, 0xFE20, 0xFE2F)) return Bc::NSM;
    if (in(c, 0xFE50, 0xFE6F)) return Bc::ON;
    if (in(c, 0xFE70, 0xFEFE)) return Bc::AL;
    if (in(c, 0xFF01, 0xFF20)) {
        if (in(c, 0xFF10, 0xFF19)) return Bc::EN;
        if (in(c, 0xFF03, 0xFF05)) return Bc::ET;
        if (c == 0xFF0B || c == 0xFF0D) return Bc::ES;
        if (c == 0xFF0C || in(c, 0xFF0E, 0xFF0F) || c == 0xFF1A) return Bc::CS;
        return Bc::ON;
    }
    if (in(c, 0x10800, 0x10FFF)) {
        if (in(c, 0x10D00, 0x10D3F) || in(c, 0x10EC0, 0x10EFF) || in(c, 0x10F30, 0x10F6F)) return Bc::AL;
        if (in(c, 0x10E60, 0x10E7E) || in(c, 0x10D30, 0x10D39)) return Bc::AN;
        return Bc::R;
    }
    if (in(c, 0x1E800, 0x1EFFF)) return in(c, 0x1EC70, 0x1ECBF) || in(c, 0x1ED00, 0x1EDFF) || in(c, 0x1EE00, 0x1EEFF)
                                     ? Bc::AL
                                     : Bc::R;
    if (in(c, 0x1F000, 0x1FAFF)) return Bc::ON; // pictographs, emoji
    if (in(c, 0xE0001, 0xE007F)) return Bc::BN;  // tags
    return Bc::L;
}

bool strong_rtl(Bc t) { return t == Bc::R || t == Bc::AL; }
bool isolate_initiator(Bc t) { return t == Bc::LRI || t == Bc::RLI || t == Bc::FSI; }
bool removed_by_x9(Bc t) {
    return t == Bc::LRE || t == Bc::RLE || t == Bc::LRO || t == Bc::RLO || t == Bc::PDF || t == Bc::BN;
}
bool neutral(Bc t) {
    return t == Bc::B || t == Bc::S || t == Bc::WS || t == Bc::ON || isolate_initiator(t) || t == Bc::PDI;
}

struct Char {
    std::size_t begin = 0;
    Bc original = Bc::L;
    Bc type = Bc::L;
    u8 level = 0;
};

// P2-P3 over chars [from, to): the first strong type, skipping isolates.
std::optional<TextDirection> first_strong(const std::vector<Char>& chars, std::size_t from, std::size_t to,
                                          bool stop_at_pdi) {
    i32 isolates = 0;
    for (std::size_t i = from; i < to; ++i) {
        const Bc t = chars[i].original;
        if (isolate_initiator(t)) {
            ++isolates;
        } else if (t == Bc::PDI) {
            if (isolates == 0 && stop_at_pdi) {
                break;
            }
            isolates = std::max(0, isolates - 1);
        } else if (isolates == 0 && t == Bc::L) {
            return TextDirection::LeftToRight;
        } else if (isolates == 0 && strong_rtl(t)) {
            return TextDirection::RightToLeft;
        }
    }
    return std::nullopt;
}

std::vector<Char> decode(std::string_view text) {
    std::vector<Char> chars;
    chars.reserve(text.size());
    for (std::size_t k = 0; k < text.size(); k = utf8_next(text, k)) {
        const Bc type = bidi_class(utf8_decode(text, k));
        chars.push_back({k, type, type, 0});
    }
    return chars;
}

// Resolves weak and neutral types and implicit levels over one level run
// (W1-W7, N1-N2, I1-I2). `ids` index the run's characters in order, with
// those X9 removes left out.
void resolve_run(std::vector<Char>& chars, const std::vector<std::size_t>& ids, Bc sos, Bc eos) {
    const u8 level = chars[ids.front()].level;
    const auto type = [&](std::size_t n) -> Bc& { return chars[ids[n]].type; };
    const std::size_t count = ids.size();

    // W1
    for (std::size_t n = 0; n < count; ++n) {
        if (type(n) == Bc::NSM) {
            const Bc before = n == 0 ? sos : type(n - 1);
            type(n) = isolate_initiator(before) || before == Bc::PDI ? Bc::ON : before;
        }
    }
    // W2, W3
    Bc last_strong = sos;
    for (std::size_t n = 0; n < count; ++n) {
        const Bc t = type(n);
        if (t == Bc::L || t == Bc::R || t == Bc::AL) {
            last_strong = t;
        } else if (t == Bc::EN && last_strong == Bc::AL) {
            type(n) = Bc::AN;
        }
    }
    for (std::size_t n = 0; n < count; ++n) {
        if (type(n) == Bc::AL) {
            type(n) = Bc::R;
        }
    }
    // W4
    for (std::size_t n = 1; n + 1 < count; ++n) {
        const Bc a = type(n - 1), t = type(n), b = type(n + 1);
        if (t == Bc::ES && a == Bc::EN && b == Bc::EN) {
            type(n) = Bc::EN;
        } else if (t == Bc::CS && a == b && (a == Bc::EN || a == Bc::AN)) {
            type(n) = a;
        }
    }
    // W5
    for (std::size_t n = 0; n < count; ++n) {
        if (type(n) != Bc::ET) {
            continue;
        }
        std::size_t end = n;
        while (end < count && type(end) == Bc::ET) ++end;
        const bool touches_en = (n > 0 && type(n - 1) == Bc::EN) || (end < count && type(end) == Bc::EN);
        if (touches_en) {
            for (std::size_t m = n; m < end; ++m) type(m) = Bc::EN;
        }
        n = end;
    }
    // W6
    for (std::size_t n = 0; n < count; ++n) {
        const Bc t = type(n);
        if (t == Bc::ES || t == Bc::ET || t == Bc::CS) {
            type(n) = Bc::ON;
        }
    }
    // W7
    last_strong = sos;
    for (std::size_t n = 0; n < count; ++n) {
        const Bc t = type(n);
        if (t == Bc::L || t == Bc::R) {
            last_strong = t;
        } else if (t == Bc::EN && last_strong == Bc::L) {
            type(n) = Bc::L;
        }
    }
    // N1, N2
    const auto as_strong = [](Bc t) { return t == Bc::L ? Bc::L : Bc::R; }; // R, EN and AN count as R
    const Bc embedding = (level & 1) ? Bc::R : Bc::L;
    for (std::size_t n = 0; n < count; ++n) {
        if (!neutral(type(n))) {
            continue;
        }
        std::size_t end = n;
        while (end < count && neutral(type(end))) ++end;
        const Bc before = n == 0 ? sos : as_strong(type(n - 1));
        const Bc after = end == count ? eos : as_strong(type(end));
        const Bc resolved = before == after ? before : embedding;
        for (std::size_t m = n; m < end; ++m) type(m) = resolved;
        n = end;
    }
    // I1, I2
    for (std::size_t n = 0; n < count; ++n) {
        Char& ch = chars[ids[n]];
        if ((ch.level & 1) == 0) {
            if (ch.type == Bc::R) {
                ch.level += 1;
            } else if (ch.type == Bc::AN || ch.type == Bc::EN) {
                ch.level += 2;
            }
        } else if (ch.type == Bc::L || ch.type == Bc::EN || ch.type == Bc::AN) {
            ch.level += 1;
        }
    }
}

} // namespace

bool has_right_to_left(std::string_view text) {
    for (std::size_t k = 0; k < text.size(); k = utf8_next(text, k)) {
        const auto byte = static_cast<unsigned char>(text[k]);
        if (byte < 0xD6) { // below U+0590 (and ASCII)
            continue;
        }
        const Bc t = bidi_class(utf8_decode(text, k));
        if (t == Bc::R || t == Bc::AL || t == Bc::AN || t == Bc::RLE || t == Bc::RLO || t == Bc::RLI ||
            t == Bc::FSI) {
            return true;
        }
    }
    return false;
}

TextDirection first_strong_direction(std::string_view text, TextDirection fallback) {
    const std::vector<Char> chars = decode(text);
    return first_strong(chars, 0, chars.size(), false).value_or(fallback);
}

std::vector<BidiRun> bidi_runs(std::string_view line, std::optional<TextDirection> base) {
    std::vector<BidiRun> runs;
    if (line.empty()) {
        return runs;
    }
    std::vector<Char> chars = decode(line);
    const TextDirection direction = base ? *base : first_strong(chars, 0, chars.size(), false).value_or(TextDirection::LeftToRight);
    const u8 paragraph = direction == TextDirection::RightToLeft ? 1 : 0;

    // X1-X8: explicit levels.
    struct Entry {
        u8 level;
        Bc override; // L, R, or ON for none
    };
    constexpr u8 max_depth = 125;
    std::vector<Entry> stack{{paragraph, Bc::ON}};
    for (std::size_t i = 0; i < chars.size(); ++i) {
        Char& ch = chars[i];
        const Entry top = stack.back();
        Bc t = ch.original;
        if (t == Bc::FSI) {
            t = first_strong(chars, i + 1, chars.size(), true) == TextDirection::RightToLeft ? Bc::RLI : Bc::LRI;
        }
        const bool rtl_push = t == Bc::RLE || t == Bc::RLO || t == Bc::RLI;
        const bool ltr_push = t == Bc::LRE || t == Bc::LRO || t == Bc::LRI;
        if (rtl_push || ltr_push) {
            const u8 next = rtl_push ? static_cast<u8>((top.level + 1) | 1) : static_cast<u8>((top.level + 2) & ~1);
            ch.level = top.level;
            if (isolate_initiator(t) && top.override != Bc::ON) {
                ch.type = top.override;
            }
            if (next <= max_depth) {
                stack.push_back({next, t == Bc::RLO ? Bc::R : (t == Bc::LRO ? Bc::L : Bc::ON)});
            }
            continue;
        }
        if (t == Bc::PDF || t == Bc::PDI) {
            if (stack.size() > 1) {
                stack.pop_back();
            }
            ch.level = stack.back().level;
            if (t == Bc::PDI && stack.back().override != Bc::ON) {
                ch.type = stack.back().override;
            }
            continue;
        }
        if (t == Bc::B) {
            ch.level = paragraph;
            continue;
        }
        ch.level = top.level;
        if (top.override != Bc::ON && !removed_by_x9(t)) {
            ch.type = top.override;
        }
    }

    // Level runs over the characters X9 keeps, each resolved with its sos and eos.
    std::vector<std::size_t> kept;
    kept.reserve(chars.size());
    for (std::size_t i = 0; i < chars.size(); ++i) {
        if (!removed_by_x9(chars[i].original)) {
            kept.push_back(i);
        }
    }
    const auto direction_of = [](u8 level) { return (level & 1) ? Bc::R : Bc::L; };
    std::vector<std::size_t> ids;
    for (std::size_t k = 0; k < kept.size();) {
        const u8 level = chars[kept[k]].level;
        ids.clear();
        std::size_t end = k;
        while (end < kept.size() && chars[kept[end]].level == level) {
            ids.push_back(kept[end]);
            ++end;
        }
        const u8 before = k == 0 ? paragraph : chars[kept[k - 1]].level;
        const u8 after = end == kept.size() ? paragraph : chars[kept[end]].level;
        resolve_run(chars, ids, direction_of(std::max(level, before)), direction_of(std::max(level, after)));
        k = end;
    }

    // L1: separators, and whitespace before them or the line's end, at the paragraph level.
    bool trailing = true;
    for (std::size_t i = chars.size(); i-- > 0;) {
        const Bc t = chars[i].original;
        if (t == Bc::S || t == Bc::B) {
            chars[i].level = paragraph;
            trailing = true;
        } else if (t == Bc::WS || isolate_initiator(t) || t == Bc::PDI || removed_by_x9(t)) {
            if (trailing) {
                chars[i].level = paragraph;
            }
        } else {
            trailing = false;
        }
    }
    // What X9 removed takes the level before it (or after, at the start).
    for (std::size_t i = 0; i < chars.size(); ++i) {
        if (removed_by_x9(chars[i].original)) {
            if (i > 0) {
                chars[i].level = chars[i - 1].level;
            } else {
                const auto next = std::ranges::find_if(chars, [](const Char& c) { return !removed_by_x9(c.original); });
                chars[i].level = next != chars.end() ? next->level : paragraph;
            }
        }
    }

    // Runs in logical order, then reordered (L2).
    for (std::size_t i = 0; i < chars.size();) {
        std::size_t end = i;
        while (end < chars.size() && chars[end].level == chars[i].level) ++end;
        runs.push_back({chars[i].begin, end < chars.size() ? chars[end].begin : line.size(), chars[i].level});
        i = end;
    }
    u8 highest = 0;
    u8 lowest_odd = 255;
    for (const BidiRun& run : runs) {
        highest = std::max(highest, run.level);
        if (run.level & 1) {
            lowest_odd = std::min(lowest_odd, run.level);
        }
    }
    for (u8 level = highest; level >= lowest_odd && level > 0; --level) {
        for (std::size_t i = 0; i < runs.size();) {
            if (runs[i].level < level) {
                ++i;
                continue;
            }
            std::size_t end = i;
            while (end < runs.size() && runs[end].level >= level) ++end;
            std::reverse(runs.begin() + static_cast<std::ptrdiff_t>(i), runs.begin() + static_cast<std::ptrdiff_t>(end));
            i = end;
        }
    }
    return runs;
}

} // namespace kin
