#include <kin/l10n/message_format.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <optional>

namespace kin {
namespace {

// ---------------------------------------------------------------------------
// Locales

// The language of a tag, lower case: "pt" for "pt-PT", "pt_BR" or "PT".
std::string_view language_of(std::string_view locale, std::array<char, 8>& buffer) {
    std::size_t n = 0;
    while (n < locale.size() && n < buffer.size() && locale[n] != '-' && locale[n] != '_') {
        const char c = locale[n];
        buffer[n] = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        ++n;
    }
    return {buffer.data(), n};
}

bool region_is(std::string_view locale, std::string_view region) {
    const std::size_t dash = locale.find_first_of("-_");
    if (dash == std::string_view::npos || locale.size() - dash - 1 != region.size()) {
        return false;
    }
    for (std::size_t i = 0; i < region.size(); ++i) {
        const char c = locale[dash + 1 + i];
        if ((c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c) != region[i]) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Plural rules (CLDR 44 cardinals)

enum class Rule : u8 {
    OneIsOne,       // en de nl sv ...: one for 1 (no decimals)
    OneIsN1,        // el bg hu tr es ...: one when n = 1, decimals too
    Other,          // ja ko zh th vi id ms
    French,         // fr pt: one for 0 and 1 (and 0.x, 1.x); many for millions
    Portugal,       // pt-PT: one for 1; many for millions
    Spanish,        // es: one for n = 1; many for millions
    Italian,        // it ca: one for 1; many for millions
    Danish,
    HindiPersian,   // hi fa: one for 0 and 1
    Filipino,
    EastSlavic,     // ru uk be
    Polish,
    Czech,          // cs sk
    SerboCroatian,  // bs hr sr
    Slovenian,
    Lithuanian,
    Latvian,
    Romanian,
    Hebrew,
    Arabic,
};

struct LanguageRule {
    std::string_view language;
    Rule rule;
};

constexpr std::array language_rules{
    LanguageRule{"ar", Rule::Arabic},       LanguageRule{"be", Rule::EastSlavic},
    LanguageRule{"bg", Rule::OneIsN1},      LanguageRule{"bs", Rule::SerboCroatian},
    LanguageRule{"ca", Rule::Italian},      LanguageRule{"cs", Rule::Czech},
    LanguageRule{"da", Rule::Danish},       LanguageRule{"de", Rule::OneIsOne},
    LanguageRule{"el", Rule::OneIsN1},      LanguageRule{"en", Rule::OneIsOne},
    LanguageRule{"es", Rule::Spanish},      LanguageRule{"et", Rule::OneIsOne},
    LanguageRule{"fa", Rule::HindiPersian}, LanguageRule{"fi", Rule::OneIsOne},
    LanguageRule{"fil", Rule::Filipino},    LanguageRule{"fr", Rule::French},
    LanguageRule{"he", Rule::Hebrew},       LanguageRule{"hi", Rule::HindiPersian},
    LanguageRule{"hr", Rule::SerboCroatian}, LanguageRule{"hu", Rule::OneIsN1},
    LanguageRule{"id", Rule::Other},        LanguageRule{"it", Rule::Italian},
    LanguageRule{"iw", Rule::Hebrew},       LanguageRule{"ja", Rule::Other},
    LanguageRule{"ko", Rule::Other},        LanguageRule{"lt", Rule::Lithuanian},
    LanguageRule{"lv", Rule::Latvian},      LanguageRule{"ms", Rule::Other},
    LanguageRule{"nb", Rule::OneIsN1},      LanguageRule{"nl", Rule::OneIsOne},
    LanguageRule{"nn", Rule::OneIsN1},      LanguageRule{"no", Rule::OneIsN1},
    LanguageRule{"pl", Rule::Polish},       LanguageRule{"pt", Rule::French},
    LanguageRule{"ro", Rule::Romanian},     LanguageRule{"ru", Rule::EastSlavic},
    LanguageRule{"sk", Rule::Czech},        LanguageRule{"sl", Rule::Slovenian},
    LanguageRule{"sr", Rule::SerboCroatian}, LanguageRule{"sv", Rule::OneIsOne},
    LanguageRule{"th", Rule::Other},        LanguageRule{"tl", Rule::Filipino},
    LanguageRule{"tr", Rule::OneIsN1},      LanguageRule{"uk", Rule::EastSlavic},
    LanguageRule{"vi", Rule::Other},        LanguageRule{"zh", Rule::Other},
};

Rule rule_for(std::string_view locale) {
    std::array<char, 8> buffer{};
    const std::string_view language = language_of(locale, buffer);
    if (language == "pt" && region_is(locale, "PT")) {
        return Rule::Portugal;
    }
    for (const LanguageRule& entry : language_rules) {
        if (entry.language == language) {
            return entry.rule;
        }
    }
    return Rule::OneIsOne;
}

using P = PluralCategory;

std::span<const PluralCategory> categories_of(Rule rule) {
    static constexpr std::array one_other{P::One, P::Other};
    static constexpr std::array other{P::Other};
    static constexpr std::array one_many_other{P::One, P::Many, P::Other};
    static constexpr std::array one_few_many_other{P::One, P::Few, P::Many, P::Other};
    static constexpr std::array one_few_other{P::One, P::Few, P::Other};
    static constexpr std::array one_two_few_other{P::One, P::Two, P::Few, P::Other};
    static constexpr std::array zero_one_other{P::Zero, P::One, P::Other};
    static constexpr std::array one_two_other{P::One, P::Two, P::Other};
    static constexpr std::array all{P::Zero, P::One, P::Two, P::Few, P::Many, P::Other};
    switch (rule) {
    case Rule::Other: return other;
    case Rule::French:
    case Rule::Portugal:
    case Rule::Spanish:
    case Rule::Italian: return one_many_other;
    case Rule::EastSlavic:
    case Rule::Polish:
    case Rule::Czech:
    case Rule::Lithuanian: return one_few_many_other;
    case Rule::SerboCroatian:
    case Rule::Romanian: return one_few_other;
    case Rule::Slovenian: return one_two_few_other;
    case Rule::Latvian: return zero_one_other;
    case Rule::Hebrew: return one_two_other;
    case Rule::Arabic: return all;
    default: return one_other;
    }
}

// CLDR's operands: n the absolute value, i its integer digits, v the count
// of visible fraction digits, f those digits as an integer.
struct Operands {
    f64 n = 0.0;
    i64 i = 0;
    i32 v = 0;
    i64 f = 0;
};

// The digits `format_number` would show: at most three decimals, trailing
// zeros dropped.
Operands operands_of(f64 value) {
    Operands op;
    if (!std::isfinite(value)) {
        return op;
    }
    const f64 a = std::fabs(value);
    const f64 thousandths = std::round(a * 1000.0);
    if (thousandths >= 9.0e15) { // beyond exact integers: no decimals to see
        op.n = a;
        op.i = static_cast<i64>(std::min(a, 9.0e18));
        return op;
    }
    const auto whole = static_cast<i64>(thousandths);
    op.i = whole / 1000;
    op.f = whole % 1000;
    op.v = 3;
    while (op.v > 0 && op.f % 10 == 0) {
        op.f /= 10;
        --op.v;
    }
    op.n = static_cast<f64>(whole) / 1000.0;
    return op;
}

bool in(i64 x, i64 lo, i64 hi) { return x >= lo && x <= hi; }

PluralCategory categorize(Rule rule, const Operands& o) {
    const i64 i = o.i;
    const i32 v = o.v;
    const i64 f = o.f;
    const bool integer = v == 0;
    const bool millions = integer && i != 0 && i % 1000000 == 0;
    switch (rule) {
    case Rule::OneIsOne: return i == 1 && integer ? P::One : P::Other;
    case Rule::OneIsN1: return o.n == 1.0 ? P::One : P::Other;
    case Rule::Other: return P::Other;
    case Rule::French:
        if (i == 0 || i == 1) return P::One;
        return millions ? P::Many : P::Other;
    case Rule::Portugal:
        if (i == 1 && integer) return P::One;
        return millions ? P::Many : P::Other;
    case Rule::Spanish:
        if (o.n == 1.0) return P::One;
        return millions ? P::Many : P::Other;
    case Rule::Italian:
        if (i == 1 && integer) return P::One;
        return millions ? P::Many : P::Other;
    case Rule::Danish: return o.n == 1.0 || (f != 0 && (i == 0 || i == 1)) ? P::One : P::Other;
    case Rule::HindiPersian: return i == 0 || o.n == 1.0 ? P::One : P::Other;
    case Rule::Filipino: {
        const bool one = (integer && in(i, 1, 3)) || (integer && i % 10 != 4 && i % 10 != 6 && i % 10 != 9) ||
                         (!integer && f % 10 != 4 && f % 10 != 6 && f % 10 != 9);
        return one ? P::One : P::Other;
    }
    case Rule::EastSlavic:
        if (!integer) return P::Other;
        if (i % 10 == 1 && i % 100 != 11) return P::One;
        if (in(i % 10, 2, 4) && !in(i % 100, 12, 14)) return P::Few;
        return P::Many;
    case Rule::Polish:
        if (!integer) return P::Other;
        if (i == 1) return P::One;
        if (in(i % 10, 2, 4) && !in(i % 100, 12, 14)) return P::Few;
        return P::Many;
    case Rule::Czech:
        if (!integer) return P::Many;
        if (i == 1) return P::One;
        if (in(i, 2, 4)) return P::Few;
        return P::Other;
    case Rule::SerboCroatian:
        if ((integer && i % 10 == 1 && i % 100 != 11) || (f % 10 == 1 && f % 100 != 11)) return P::One;
        if ((integer && in(i % 10, 2, 4) && !in(i % 100, 12, 14)) || (in(f % 10, 2, 4) && !in(f % 100, 12, 14))) {
            return P::Few;
        }
        return P::Other;
    case Rule::Slovenian:
        if (!integer) return P::Few;
        if (i % 100 == 1) return P::One;
        if (i % 100 == 2) return P::Two;
        if (in(i % 100, 3, 4)) return P::Few;
        return P::Other;
    case Rule::Lithuanian:
        if (f != 0) return P::Many;
        if (i % 10 == 1 && !in(i % 100, 11, 19)) return P::One;
        if (in(i % 10, 2, 9) && !in(i % 100, 11, 19)) return P::Few;
        return P::Other;
    case Rule::Latvian:
        if ((integer && (i % 10 == 0 || in(i % 100, 11, 19))) || (v == 2 && in(f % 100, 11, 19))) return P::Zero;
        if ((integer && i % 10 == 1 && i % 100 != 11) || (v == 2 && f % 10 == 1 && f % 100 != 11) ||
            (v != 2 && v != 0 && f % 10 == 1)) {
            return P::One;
        }
        return P::Other;
    case Rule::Romanian:
        if (i == 1 && integer) return P::One;
        if (!integer || i == 0 || in(i % 100, 1, 19)) return P::Few;
        return P::Other;
    case Rule::Hebrew:
        if ((i == 1 && integer) || (i == 0 && !integer)) return P::One;
        if (i == 2 && integer) return P::Two;
        return P::Other;
    case Rule::Arabic:
        if (!integer) return P::Other;
        if (i == 0) return P::Zero;
        if (i == 1) return P::One;
        if (i == 2) return P::Two;
        if (in(i % 100, 3, 10)) return P::Few;
        if (in(i % 100, 11, 99)) return P::Many;
        return P::Other;
    }
    return P::Other;
}

// ---------------------------------------------------------------------------
// Ordinal rules (CLDR 44)

enum class OrdinalRule : u8 { Other, English, OneIsOne, Italian, Swedish, Catalan, Hungarian, Hindi, Ukrainian };

OrdinalRule ordinal_rule_for(std::string_view locale) {
    std::array<char, 8> buffer{};
    const std::string_view l = language_of(locale, buffer);
    if (l == "en") return OrdinalRule::English;
    if (l == "fr" || l == "ms" || l == "ro" || l == "vi" || l == "fil" || l == "tl") return OrdinalRule::OneIsOne;
    if (l == "it") return OrdinalRule::Italian;
    if (l == "sv") return OrdinalRule::Swedish;
    if (l == "ca") return OrdinalRule::Catalan;
    if (l == "hu") return OrdinalRule::Hungarian;
    if (l == "hi") return OrdinalRule::Hindi;
    if (l == "uk") return OrdinalRule::Ukrainian;
    return OrdinalRule::Other;
}

std::span<const PluralCategory> ordinal_categories_of(OrdinalRule rule) {
    static constexpr std::array other{P::Other};
    static constexpr std::array one_other{P::One, P::Other};
    static constexpr std::array one_two_few_other{P::One, P::Two, P::Few, P::Other};
    static constexpr std::array many_other{P::Many, P::Other};
    static constexpr std::array few_other{P::Few, P::Other};
    static constexpr std::array one_two_few_many_other{P::One, P::Two, P::Few, P::Many, P::Other};
    switch (rule) {
    case OrdinalRule::English:
    case OrdinalRule::Catalan: return one_two_few_other;
    case OrdinalRule::OneIsOne:
    case OrdinalRule::Swedish:
    case OrdinalRule::Hungarian: return one_other;
    case OrdinalRule::Italian: return many_other;
    case OrdinalRule::Hindi: return one_two_few_many_other;
    case OrdinalRule::Ukrainian: return few_other;
    case OrdinalRule::Other: return other;
    }
    return other;
}

PluralCategory ordinal(OrdinalRule rule, f64 value) {
    const i64 n = static_cast<i64>(std::llround(std::fabs(value)));
    const i64 n10 = n % 10;
    const i64 n100 = n % 100;
    switch (rule) {
    case OrdinalRule::English:
        if (n10 == 1 && n100 != 11) return P::One;
        if (n10 == 2 && n100 != 12) return P::Two;
        if (n10 == 3 && n100 != 13) return P::Few;
        return P::Other;
    case OrdinalRule::OneIsOne: return n == 1 ? P::One : P::Other;
    case OrdinalRule::Italian: return n == 11 || n == 8 || n == 80 || n == 800 ? P::Many : P::Other;
    case OrdinalRule::Swedish: return (n10 == 1 || n10 == 2) && n100 != 11 && n100 != 12 ? P::One : P::Other;
    case OrdinalRule::Catalan:
        if (n == 1 || n == 3) return P::One;
        if (n == 2) return P::Two;
        if (n == 4) return P::Few;
        return P::Other;
    case OrdinalRule::Hungarian: return n == 1 || n == 5 ? P::One : P::Other;
    case OrdinalRule::Hindi:
        if (n == 1) return P::One;
        if (n == 2 || n == 3) return P::Two;
        if (n == 4) return P::Few;
        if (n == 6) return P::Many;
        return P::Other;
    case OrdinalRule::Ukrainian: return n10 == 3 && n100 != 13 ? P::Few : P::Other;
    case OrdinalRule::Other: return P::Other;
    }
    return P::Other;
}

// ---------------------------------------------------------------------------
// Numbers

struct NumberSymbols {
    std::string_view decimal = ".";
    std::string_view group = ",";
    i32 min_grouping = 1; // digits needed above the first group before grouping (es, pl: 2)
};

constexpr std::string_view nbsp = "\xC2\xA0";

NumberSymbols symbols_for(std::string_view locale) {
    std::array<char, 8> buffer{};
    const std::string_view l = language_of(locale, buffer);
    const auto any = [&](std::initializer_list<std::string_view> list) {
        return std::ranges::find(list, l) != list.end();
    };
    if (l == "pt" && region_is(locale, "PT")) {
        return {",", nbsp, 2};
    }
    if (any({"es", "pl"})) {
        return {",", l == "es" ? "." : nbsp, 2};
    }
    if (any({"de", "nl", "it", "pt", "id", "tr", "da", "el", "ro", "hr", "sl", "sr", "bs", "ca", "vi"})) {
        return {",", ".", 1};
    }
    if (any({"fr", "ru", "uk", "be", "cs", "sk", "sv", "nb", "nn", "no", "fi", "bg", "hu", "lt", "lv", "et"})) {
        return {",", nbsp, 1};
    }
    return {};
}

void append_number(std::string& out, std::string_view locale, f64 value) {
    if (std::isnan(value)) {
        out += "NaN";
        return;
    }
    if (std::isinf(value)) {
        out += value < 0 ? "-\xE2\x88\x9E" : "\xE2\x88\x9E";
        return;
    }
    const Operands o = operands_of(value);
    std::array<char, 32> digits{};
    const auto [end, ec] = std::to_chars(digits.data(), digits.data() + digits.size(), o.i);
    const std::string_view whole{digits.data(), ec == std::errc{} ? static_cast<std::size_t>(end - digits.data()) : 0};

    if (value < 0 && (o.i != 0 || o.f != 0)) {
        out += '-';
    }
    const NumberSymbols symbols = symbols_for(locale);
    const bool grouped = static_cast<i32>(whole.size()) > 3 + symbols.min_grouping - 1;
    for (std::size_t k = 0; k < whole.size(); ++k) {
        if (grouped && k > 0 && (whole.size() - k) % 3 == 0) {
            out += symbols.group;
        }
        out += whole[k];
    }
    if (o.v > 0) {
        out += symbols.decimal;
        std::array<char, 4> fraction{};
        i64 f = o.f;
        for (i32 k = o.v - 1; k >= 0; --k) {
            fraction[static_cast<std::size_t>(k)] = static_cast<char>('0' + f % 10);
            f /= 10;
        }
        out.append(fraction.data(), static_cast<std::size_t>(o.v));
    }
}

// ---------------------------------------------------------------------------
// Patterns

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

std::string_view trim(std::string_view s) {
    while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
    return s;
}

// Where an apostrophe at `pos` starts quoted text, the position of the
// apostrophe closing it (the end of the text if none does); otherwise npos.
// `hash_special`: in a plural, where '#' is syntax.
std::size_t quote_end(std::string_view text, std::size_t pos, bool hash_special) {
    if (pos + 1 >= text.size()) {
        return std::string_view::npos;
    }
    const char next = text[pos + 1];
    if (next != '{' && next != '}' && !(hash_special && next == '#')) {
        return std::string_view::npos;
    }
    std::size_t k = pos + 1;
    while (k < text.size()) {
        if (text[k] == '\'') {
            if (k + 1 < text.size() && text[k + 1] == '\'') {
                k += 2;
                continue;
            }
            return k;
        }
        ++k;
    }
    return text.size();
}

// The '}' closing the '{' at `open`, or npos.
std::size_t matching_brace(std::string_view text, std::size_t open) {
    i32 depth = 0;
    std::size_t k = open;
    while (k < text.size()) {
        const char c = text[k];
        if (c == '\'') {
            if (k + 1 < text.size() && text[k + 1] == '\'') {
                k += 2;
                continue;
            }
            const std::size_t end = quote_end(text, k, false);
            if (end != std::string_view::npos) {
                k = end + 1;
                continue;
            }
        } else if (c == '{') {
            ++depth;
        } else if (c == '}') {
            if (--depth == 0) {
                return k;
            }
        }
        ++k;
    }
    return std::string_view::npos;
}

struct Argument {
    std::string_view name;
    std::string_view kind; // "", "number", "plural", "select"
    std::string_view rest; // after the kind's comma
};

std::optional<Argument> split_argument(std::string_view inner) {
    Argument arg;
    const std::size_t comma = inner.find(',');
    arg.name = trim(inner.substr(0, comma));
    if (arg.name.empty()) {
        return std::nullopt;
    }
    for (char c : arg.name) {
        if (is_space(c) || c == '{' || c == '}' || c == '\'' || c == '#') {
            return std::nullopt;
        }
    }
    if (comma == std::string_view::npos) {
        return arg;
    }
    const std::string_view after = inner.substr(comma + 1);
    const std::size_t comma2 = after.find(',');
    arg.kind = trim(after.substr(0, comma2));
    if (arg.kind != "number" && arg.kind != "plural" && arg.kind != "select" && arg.kind != "selectordinal") {
        return std::nullopt;
    }
    if (comma2 != std::string_view::npos) {
        arg.rest = after.substr(comma2 + 1);
    }
    if (arg.kind != "number" && trim(arg.rest).empty()) {
        return std::nullopt;
    }
    return arg;
}

struct Branch {
    std::string_view key;
    std::string_view body;
};

// "one {...} other {...}" as branches; false if malformed or without other.
bool split_branches(std::string_view rest, std::vector<Branch>& out) {
    out.clear();
    std::size_t k = 0;
    bool other = false;
    while (true) {
        while (k < rest.size() && is_space(rest[k])) ++k;
        if (k == rest.size()) {
            break;
        }
        const std::size_t key_begin = k;
        while (k < rest.size() && !is_space(rest[k]) && rest[k] != '{' && rest[k] != '}') ++k;
        const std::string_view key = rest.substr(key_begin, k - key_begin);
        while (k < rest.size() && is_space(rest[k])) ++k;
        if (key.empty() || k == rest.size() || rest[k] != '{') {
            return false;
        }
        const std::size_t close = matching_brace(rest, k);
        if (close == std::string_view::npos) {
            return false;
        }
        out.push_back({key, rest.substr(k + 1, close - k - 1)});
        other = other || key == "other";
        k = close + 1;
    }
    return other;
}

const MessageArg* find_arg(MessageArgs args, std::string_view name) {
    for (const MessageArg& arg : args) {
        if (arg.name == name) {
            return &arg;
        }
    }
    return nullptr;
}

struct Formatter {
    std::string_view locale;
    MessageArgs args;
    const MessageFormatOptions& options;
    Rule rule;
    std::string literal; // pending literal text

    void flush(std::string& out) {
        if (literal.empty()) {
            return;
        }
        if (options.transform_literal) {
            options.transform_literal(literal, out);
        } else {
            out += literal;
        }
        literal.clear();
    }

    // Formats `text` into `out`; `number` is the plural's value inside a plural branch.
    bool run(std::string_view text, std::string& out, std::optional<f64> number) {
        std::size_t k = 0;
        while (k < text.size()) {
            const char c = text[k];
            if (c == '\'') {
                if (k + 1 < text.size() && text[k + 1] == '\'') {
                    literal += '\'';
                    k += 2;
                    continue;
                }
                const std::size_t end = quote_end(text, k, number.has_value());
                if (end == std::string_view::npos) {
                    literal += '\'';
                    ++k;
                    continue;
                }
                // Quoted text, with '' inside it as one apostrophe.
                for (std::size_t q = k + 1; q < end; ++q) {
                    literal += text[q];
                    if (text[q] == '\'' && q + 1 < end && text[q + 1] == '\'') {
                        ++q;
                    }
                }
                k = end + 1;
                continue;
            }
            if (c == '#' && number) {
                flush(out);
                append_number(out, locale, *number);
                ++k;
                continue;
            }
            if (c == '}') {
                return false;
            }
            if (c != '{') {
                literal += c;
                ++k;
                continue;
            }
            const std::size_t close = matching_brace(text, k);
            if (close == std::string_view::npos) {
                return false;
            }
            const std::string_view whole = text.substr(k, close - k + 1);
            const std::optional<Argument> arg = split_argument(whole.substr(1, whole.size() - 2));
            if (!arg) {
                return false;
            }
            k = close + 1;
            flush(out);
            const MessageArg* value = find_arg(args, arg->name);
            if (arg->kind.empty() || arg->kind == "number") {
                if (!value) {
                    out += whole;
                } else if (const f64* n = std::get_if<f64>(&value->value)) {
                    append_number(out, locale, *n);
                } else {
                    out += std::get<std::string_view>(value->value);
                }
                continue;
            }
            std::vector<Branch> own;
            if (!split_branches(arg->rest, own)) {
                return false;
            }
            if (!value) {
                out += whole;
                continue;
            }
            const Branch* chosen = nullptr;
            if (arg->kind == "plural" || arg->kind == "selectordinal") {
                const f64* n = std::get_if<f64>(&value->value);
                f64 x = 0.0;
                if (n) {
                    x = *n;
                } else {
                    const std::string_view s = std::get<std::string_view>(value->value);
                    std::from_chars(s.data(), s.data() + s.size(), x);
                }
                for (const Branch& b : own) {
                    if (b.key.size() > 1 && b.key[0] == '=') {
                        f64 exact = 0.0;
                        const auto [p, ec] = std::from_chars(b.key.data() + 1, b.key.data() + b.key.size(), exact);
                        if (ec == std::errc{} && p == b.key.data() + b.key.size() && exact == x) {
                            chosen = &b;
                            break;
                        }
                    }
                }
                if (!chosen) {
                    const std::string_view category = plural_category_name(
                        arg->kind == "plural" ? categorize(rule, operands_of(x)) : ordinal(ordinal_rule_for(locale), x));
                    for (const Branch& b : own) {
                        if (b.key == category) {
                            chosen = &b;
                            break;
                        }
                    }
                }
                if (!chosen) {
                    chosen = &*std::ranges::find(own, std::string_view{"other"}, &Branch::key);
                }
                if (!run(chosen->body, out, x)) {
                    return false;
                }
            } else {
                const std::string_view* s = std::get_if<std::string_view>(&value->value);
                for (const Branch& b : own) {
                    if (s && b.key == *s) {
                        chosen = &b;
                        break;
                    }
                }
                if (!chosen) {
                    chosen = &*std::ranges::find(own, std::string_view{"other"}, &Branch::key);
                }
                if (!run(chosen->body, out, number)) {
                    return false;
                }
            }
        }
        flush(out);
        return true;
    }
};

bool inspect(std::string_view text, MessageShape& shape, std::string& error, bool in_plural) {
    std::size_t k = 0;
    while (k < text.size()) {
        const char c = text[k];
        if (c == '\'') {
            if (k + 1 < text.size() && text[k + 1] == '\'') {
                k += 2;
                continue;
            }
            const std::size_t end = quote_end(text, k, in_plural);
            k = end == std::string_view::npos ? k + 1 : end + 1;
            continue;
        }
        if (c == '}') {
            error = "unmatched '}'";
            return false;
        }
        if (c != '{') {
            ++k;
            continue;
        }
        const std::size_t close = matching_brace(text, k);
        if (close == std::string_view::npos) {
            error = "unclosed '{'";
            return false;
        }
        const std::string_view inner = text.substr(k + 1, close - k - 1);
        const std::optional<Argument> arg = split_argument(inner);
        if (!arg) {
            error = "bad argument '{" + std::string{inner} + "}'";
            return false;
        }
        k = close + 1;
        auto found = std::ranges::find(shape.arguments, arg->name, &MessageShape::Argument::name);
        if (found == shape.arguments.end()) {
            shape.arguments.push_back({std::string{arg->name}, std::string{arg->kind}, {}});
            found = shape.arguments.end() - 1;
        } else if (found->kind.empty() && !arg->kind.empty()) {
            found->kind = std::string{arg->kind};
        }
        const bool plural = arg->kind == "plural" || arg->kind == "selectordinal";
        if (!plural && arg->kind != "select") {
            continue;
        }
        std::vector<Branch> branches;
        if (!split_branches(arg->rest, branches)) {
            error = "'" + std::string{arg->name} + "' " + std::string{arg->kind} + " needs branches ending with 'other'";
            return false;
        }
        const std::size_t index = static_cast<std::size_t>(found - shape.arguments.begin());
        for (const Branch& b : branches) {
            if (plural && b.key[0] != '=') {
                PluralCategory unused{};
                if (!parse_plural_category(b.key, unused)) {
                    error = "unknown plural category '" + std::string{b.key} + "'";
                    return false;
                }
            }
            auto& keys = shape.arguments[index].branches;
            if (std::ranges::find(keys, b.key) == keys.end()) {
                keys.emplace_back(b.key);
            }
            if (!inspect(b.body, shape, error, in_plural || plural)) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

std::string_view plural_category_name(PluralCategory category) {
    switch (category) {
    case P::Zero: return "zero";
    case P::One: return "one";
    case P::Two: return "two";
    case P::Few: return "few";
    case P::Many: return "many";
    case P::Other: return "other";
    }
    return "other";
}

bool parse_plural_category(std::string_view name, PluralCategory& out) {
    for (PluralCategory c : {P::Zero, P::One, P::Two, P::Few, P::Many, P::Other}) {
        if (plural_category_name(c) == name) {
            out = c;
            return true;
        }
    }
    return false;
}

PluralCategory plural_category(std::string_view locale, f64 n) {
    return categorize(rule_for(locale), operands_of(n));
}

std::span<const PluralCategory> plural_categories(std::string_view locale) {
    return categories_of(rule_for(locale));
}

PluralCategory ordinal_category(std::string_view locale, f64 n) {
    return ordinal(ordinal_rule_for(locale), n);
}

std::span<const PluralCategory> ordinal_categories(std::string_view locale) {
    return ordinal_categories_of(ordinal_rule_for(locale));
}

std::string format_number(std::string_view locale, f64 n) {
    std::string out;
    append_number(out, locale, n);
    return out;
}

std::string format_message(std::string_view pattern,
                           std::string_view locale,
                           MessageArgs args,
                           const MessageFormatOptions& options) {
    // The common case, a pattern with nothing to put in, without a copy loop.
    if (pattern.find_first_of("{}'") == std::string_view::npos) {
        std::string out;
        if (options.transform_literal) {
            options.transform_literal(pattern, out);
        } else {
            out.assign(pattern);
        }
        return out;
    }
    Formatter formatter{locale, args, options, rule_for(locale), {}};
    std::string out;
    out.reserve(pattern.size() + 16);
    if (!formatter.run(pattern, out, std::nullopt)) {
        return std::string{pattern};
    }
    return out;
}

std::string format_message(std::string_view pattern,
                           std::string_view locale,
                           std::initializer_list<MessageArg> args,
                           const MessageFormatOptions& options) {
    return format_message(pattern, locale, MessageArgs{args.begin(), args.size()}, options);
}

bool inspect_message(std::string_view pattern, MessageShape& out, std::string& error) {
    MessageShape shape;
    if (!inspect(pattern, shape, error, false)) {
        return false;
    }
    out = std::move(shape);
    return true;
}

} // namespace kin
