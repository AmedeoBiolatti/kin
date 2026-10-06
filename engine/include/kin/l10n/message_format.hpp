#pragma once

#include <kin/core/types.hpp>

#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace kin {

// Translated text with values put in, in a subset of ICU MessageFormat:
//
//   "{gold} gold"                                       an argument
//   "{n, plural, =0 {no cards} one {# card} other {# cards}}"
//   "{who, select, female {her turn} male {his turn} other {their turn}}"
//   "{place, selectordinal, one {#st} two {#nd} few {#rd} other {#th}}"
//   "{n, number}"                                       the same as "{n}"
//
// In a plural, `#` is the number, formatted for the locale; `=N` matches one
// value exactly and wins over the categories (zero, one, two, few, many,
// other: see plural_category). A selectordinal is a plural of ordinal
// numbers ("1st", "2nd"; see ordinal_category). A plural or select must have `other`, which
// is also what an absent category falls back to. Branches nest.
//
// Numbers are grouped and use the locale's decimal sign ("12,500.5" in
// English, "12 500,5" in French, "12.500,5" in German), at most three
// decimals. A string argument is put in as it is.
//
// Quoting follows ICU: an apostrophe before `{`, `}` or (in a plural) `#`
// starts literal text that the next apostrophe ends, and two apostrophes are
// one. Any other apostrophe is just an apostrophe ("l'or", "don't").
//
// An argument the call does not give is left in the text as written
// ("{gold}"), so a missing value shows rather than vanishing.

// A named value for a message.
struct MessageArg {
    std::string_view name;
    std::variant<f64, std::string_view> value;

    MessageArg(std::string_view n, f64 v) : name(n), value(v) {}
    MessageArg(std::string_view n, i64 v) : name(n), value(static_cast<f64>(v)) {}
    MessageArg(std::string_view n, i32 v) : name(n), value(static_cast<f64>(v)) {}
    MessageArg(std::string_view n, u32 v) : name(n), value(static_cast<f64>(v)) {}
    MessageArg(std::string_view n, u64 v) : name(n), value(static_cast<f64>(v)) {}
    MessageArg(std::string_view n, std::string_view v) : name(n), value(v) {}
    MessageArg(std::string_view n, const char* v) : name(n), value(std::string_view{v}) {}
    MessageArg(std::string_view n, const std::string& v) : name(n), value(std::string_view{v}) {}
};

using MessageArgs = std::span<const MessageArg>;

// CLDR cardinal plural categories.
enum class PluralCategory : u8 { Zero, One, Two, Few, Many, Other };

std::string_view plural_category_name(PluralCategory category);
bool parse_plural_category(std::string_view name, PluralCategory& out);

// The category `n` falls in for `locale`'s language ("fr-CA" uses French's).
// Rules for: ar, be, bg, bs, ca, cs, da, de, el, en, es, et, fa, fi, fil, fr,
// he, hi, hr, hu, id, it, ja, ko, lt, lv, ms, nb, nl, nn, no, pl, pt (pt-PT
// apart), ro, ru, sk, sl, sr, sv, th, tr, uk, vi, zh. Other languages use
// one for 1 and other for the rest. A fractional n counts its visible
// decimals as formatted (at most three), as CLDR's operands do.
PluralCategory plural_category(std::string_view locale, f64 n);
// The categories `locale`'s language uses, `other` last.
std::span<const PluralCategory> plural_categories(std::string_view locale);

// The same for ordinals (CLDR): English one (1st, 21st), two (2nd), few (3rd),
// other (4th, 11th); French one (1er) and other; Italian many (l'8, l'11);
// rules for ca, en, fil, fr, hi, hu, it, ms, ro, sv, tl, uk, vi. Other
// languages have one form.
PluralCategory ordinal_category(std::string_view locale, f64 n);
std::span<const PluralCategory> ordinal_categories(std::string_view locale);

// `n` written for `locale`: grouped, its decimal sign, at most three decimals.
std::string format_number(std::string_view locale, f64 n);

struct MessageFormatOptions {
    // Called on each run of literal text, which it may rewrite (pseudo-
    // localization); arguments and numbers pass through untouched.
    void (*transform_literal)(std::string_view text, std::string& out) = nullptr;
};

// `pattern` with `args` put in. A malformed pattern comes back as it is.
std::string format_message(std::string_view pattern,
                           std::string_view locale,
                           MessageArgs args = {},
                           const MessageFormatOptions& options = {});
std::string format_message(std::string_view pattern,
                           std::string_view locale,
                           std::initializer_list<MessageArg> args,
                           const MessageFormatOptions& options = {});

// What a pattern asks for, for checking one translation against another.
struct MessageShape {
    struct Argument {
        std::string name;
        std::string kind; // "", "number", "plural", "selectordinal" or "select"
        std::vector<std::string> branches; // a plural's or select's keys ("one", "=0", ...)

        friend bool operator==(const Argument&, const Argument&) = default;
    };
    std::vector<Argument> arguments; // in order of first use, each name once
};

// Parses `pattern`; false, with a message in `error`, when it is malformed
// (an unclosed brace, a plural or select without `other`, an unknown kind).
bool inspect_message(std::string_view pattern, MessageShape& out, std::string& error);

} // namespace kin
