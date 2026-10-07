#include <kin/l10n/localization.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

namespace kin {
namespace {

// ---------------------------------------------------------------------------
// Plural-Forms: "nplurals=3; plural=(n==1 ? 0 : n%10>=2 && n%10<=4 ? 1 : 2);"

// Evaluates gettext's C-like plural expression for `n`; nullopt if malformed.
class PluralExpression {
public:
    PluralExpression(std::string_view text, i64 n) : _text(text), _n(n) {}

    std::optional<i64> evaluate() {
        const std::optional<i64> value = ternary();
        skip_spaces();
        if (!value || _pos != _text.size()) {
            return std::nullopt;
        }
        return value;
    }

private:
    void skip_spaces() {
        while (_pos < _text.size() && (_text[_pos] == ' ' || _text[_pos] == '\t')) ++_pos;
    }
    bool take(std::string_view token) {
        skip_spaces();
        if (_text.substr(_pos, token.size()) != token) {
            return false;
        }
        // "<" is not "<=", "!" not "!=", "=" alone is not an operator.
        if ((token == "<" || token == ">" || token == "!") && _pos + 1 < _text.size() && _text[_pos + 1] == '=') {
            return false;
        }
        _pos += token.size();
        return true;
    }
    std::optional<i64> ternary() {
        std::optional<i64> condition = logical_or();
        if (!condition || !take("?")) {
            return condition;
        }
        const std::optional<i64> yes = ternary();
        if (!yes || !take(":")) {
            return std::nullopt;
        }
        const std::optional<i64> no = ternary();
        if (!no) {
            return std::nullopt;
        }
        return *condition ? yes : no;
    }
    std::optional<i64> logical_or() {
        std::optional<i64> left = logical_and();
        while (left && take("||")) {
            const std::optional<i64> right = logical_and();
            if (!right) return std::nullopt;
            left = (*left || *right) ? 1 : 0;
        }
        return left;
    }
    std::optional<i64> logical_and() {
        std::optional<i64> left = equality();
        while (left && take("&&")) {
            const std::optional<i64> right = equality();
            if (!right) return std::nullopt;
            left = (*left && *right) ? 1 : 0;
        }
        return left;
    }
    std::optional<i64> equality() {
        std::optional<i64> left = relation();
        while (left) {
            if (take("==")) {
                const auto right = relation();
                if (!right) return std::nullopt;
                left = *left == *right;
            } else if (take("!=")) {
                const auto right = relation();
                if (!right) return std::nullopt;
                left = *left != *right;
            } else {
                break;
            }
        }
        return left;
    }
    std::optional<i64> relation() {
        std::optional<i64> left = sum();
        while (left) {
            std::optional<i64> right;
            if (take("<=")) {
                if (!(right = sum())) return std::nullopt;
                left = *left <= *right;
            } else if (take(">=")) {
                if (!(right = sum())) return std::nullopt;
                left = *left >= *right;
            } else if (take("<")) {
                if (!(right = sum())) return std::nullopt;
                left = *left < *right;
            } else if (take(">")) {
                if (!(right = sum())) return std::nullopt;
                left = *left > *right;
            } else {
                break;
            }
        }
        return left;
    }
    std::optional<i64> sum() {
        std::optional<i64> left = product();
        while (left) {
            if (take("+")) {
                const auto right = product();
                if (!right) return std::nullopt;
                left = *left + *right;
            } else if (take("-")) {
                const auto right = product();
                if (!right) return std::nullopt;
                left = *left - *right;
            } else {
                break;
            }
        }
        return left;
    }
    std::optional<i64> product() {
        std::optional<i64> left = unary();
        while (left) {
            const bool times = take("*");
            const bool divide = !times && take("/");
            const bool modulo = !times && !divide && take("%");
            if (!times && !divide && !modulo) {
                break;
            }
            const auto right = unary();
            if (!right || ((divide || modulo) && *right == 0)) return std::nullopt;
            left = times ? *left * *right : divide ? *left / *right : *left % *right;
        }
        return left;
    }
    std::optional<i64> unary() {
        if (take("!")) {
            const auto value = unary();
            return value ? std::optional<i64>{!*value} : std::nullopt;
        }
        if (take("-")) {
            const auto value = unary();
            return value ? std::optional<i64>{-*value} : std::nullopt;
        }
        return primary();
    }
    std::optional<i64> primary() {
        skip_spaces();
        if (take("(")) {
            const auto value = ternary();
            return value && take(")") ? value : std::nullopt;
        }
        if (_pos < _text.size() && _text[_pos] == 'n') {
            ++_pos;
            return _n;
        }
        i64 value = 0;
        const std::size_t start = _pos;
        while (_pos < _text.size() && _text[_pos] >= '0' && _text[_pos] <= '9') {
            value = value * 10 + (_text[_pos] - '0');
            ++_pos;
        }
        return _pos > start ? std::optional<i64>{value} : std::nullopt;
    }

    std::string_view _text;
    i64 _n;
    std::size_t _pos = 0;
};

struct PluralForms {
    i64 count = 2;
    std::string expression = "n != 1";
};

std::optional<PluralForms> parse_plural_forms(std::string_view header) {
    PluralForms forms;
    const std::size_t nplurals = header.find("nplurals");
    const std::size_t plural = header.find("plural=");
    if (nplurals == std::string_view::npos || plural == std::string_view::npos) {
        return std::nullopt;
    }
    std::size_t k = header.find('=', nplurals);
    if (k == std::string_view::npos) return std::nullopt;
    ++k;
    while (k < header.size() && header[k] == ' ') ++k;
    forms.count = 0;
    while (k < header.size() && header[k] >= '0' && header[k] <= '9') forms.count = forms.count * 10 + (header[k++] - '0');
    std::string_view expression = header.substr(plural + 7);
    expression = expression.substr(0, expression.find(';'));
    forms.expression = std::string{expression};
    if (forms.count < 1 || !PluralExpression(forms.expression, 1).evaluate()) {
        return std::nullopt;
    }
    return forms;
}

// "%d", "%i", "%u", "%1$d", "%ld" -> "#"; "%%" -> "%".
std::string printf_to_hash(std::string_view text) {
    std::string out;
    for (std::size_t k = 0; k < text.size(); ++k) {
        if (text[k] != '%') {
            out += text[k];
            continue;
        }
        if (k + 1 < text.size() && text[k + 1] == '%') {
            out += '%';
            ++k;
            continue;
        }
        std::size_t j = k + 1;
        while (j < text.size() && text[j] >= '0' && text[j] <= '9') ++j;
        if (j < text.size() && text[j] == '$') ++j;
        while (j < text.size() && (text[j] == 'l' || text[j] == 'h' || text[j] == 'z' || text[j] == 'j')) ++j;
        if (j < text.size() && (text[j] == 'd' || text[j] == 'i' || text[j] == 'u')) {
            out += '#';
            k = j;
        } else {
            out += '%';
        }
    }
    return out;
}

// An entry's plural forms as an ICU plural over `n`: each category the
// language uses takes the form Plural-Forms picks for an integer of that
// category; a category with no integer (fractions) takes the last form.
std::string plural_message(const std::vector<std::string>& forms, const PluralForms& rule, std::string_view locale) {
    std::string out = "{n, plural,";
    for (const PluralCategory category : plural_categories(locale)) {
        std::optional<i64> sample;
        for (i64 n = 0; n <= 1000 && !sample; ++n) {
            if (plural_category(locale, static_cast<f64>(n)) == category) sample = n;
        }
        if (!sample && plural_category(locale, 1000000.0) == category) sample = 1000000;
        std::size_t index = forms.size() - 1;
        if (sample) {
            if (const std::optional<i64> picked = PluralExpression(rule.expression, *sample).evaluate()) {
                index = static_cast<std::size_t>(std::clamp<i64>(*picked, 0, static_cast<i64>(forms.size()) - 1));
            }
        }
        out += ' ';
        out += plural_category_name(category);
        out += " {";
        out += printf_to_hash(forms[index]);
        out += '}';
    }
    out += '}';
    return out;
}

// ---------------------------------------------------------------------------
// The file

bool unescape(std::string_view quoted, std::string& out) {
    if (quoted.size() < 2 || quoted.front() != '"' || quoted.back() != '"') {
        return false;
    }
    quoted = quoted.substr(1, quoted.size() - 2);
    for (std::size_t k = 0; k < quoted.size(); ++k) {
        const char c = quoted[k];
        if (c != '\\' || k + 1 == quoted.size()) {
            out += c;
            continue;
        }
        const char e = quoted[++k];
        switch (e) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case 'a': out += '\a'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'v': out += '\v'; break;
        default: out += e; break; // \" \\ \'
        }
    }
    return true;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

struct Entry {
    std::optional<std::string> context;
    std::string id;
    std::optional<std::string> id_plural;
    std::vector<std::string> strs; // msgstr, or msgstr[0..]
    bool fuzzy = false;
    bool obsolete = false;
    i32 line = 0;
};

void escape(std::string& out, std::string_view text) {
    out += '"';
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\t': out += "\\t"; break;
        case '\r': out += "\\r"; break;
        case '\n': out += "\\n"; break;
        default: out += c; break;
        }
    }
    out += '"';
}

// A keyword and its string, a line a time where it holds line breaks.
void write_field(std::string& out, std::string_view keyword, std::string_view text) {
    out += keyword;
    out += ' ';
    if (text.find('\n') == std::string_view::npos || text.find('\n') == text.size() - 1) {
        escape(out, text);
        out += '\n';
        return;
    }
    out += "\"\"\n";
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = std::min(text.find('\n', start), text.size() - 1) + 1;
        escape(out, text.substr(start, end - start));
        out += '\n';
        start = end;
    }
}

} // namespace

bool parse_gettext(std::string_view text, LanguageFile& out, std::vector<std::string>& errors, std::string_view locale) {
    if (text.starts_with("\xEF\xBB\xBF")) {
        text.remove_prefix(3);
    }
    const std::size_t errors_before = errors.size();
    std::vector<Entry> entries;
    Entry entry;
    bool started = false;  // a keyword seen in this entry
    bool has_str = false;  // and a msgstr
    std::string* target = nullptr; // what continuation lines add to
    bool next_fuzzy = false;
    const auto finish = [&] {
        if (started) {
            entries.push_back(std::move(entry));
        }
        entry = {};
        started = has_str = false;
        target = nullptr;
    };
    i32 line_number = 0;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = std::min(text.find('\n', start), text.size());
        std::string_view line = trim(text.substr(start, end - start));
        ++line_number;
        start = end + 1;
        const std::string where = "line " + std::to_string(line_number) + ": ";
        if (line.empty()) {
            if (has_str) finish();
            if (end == text.size()) break;
            continue;
        }
        bool obsolete = false;
        if (line.starts_with("#~")) {
            obsolete = true;
            line = trim(line.substr(2));
        } else if (line.starts_with('#')) {
            if (has_str) finish();
            if (line.starts_with("#,") && line.find("fuzzy") != std::string_view::npos) next_fuzzy = true;
            if (end == text.size()) break;
            continue;
        }
        if (line.starts_with('"')) {
            if (!target || !unescape(line, *target)) {
                errors.push_back(where + "a string out of place");
            }
            if (end == text.size()) break;
            continue;
        }
        const std::size_t space = line.find_first_of(" \t");
        const std::string_view keyword = line.substr(0, space);
        const std::string_view value = space == std::string_view::npos ? std::string_view{} : trim(line.substr(space));
        if ((keyword == "msgctxt" || keyword == "msgid") && has_str) {
            finish();
        }
        if (!started) {
            entry.fuzzy = next_fuzzy;
            entry.obsolete = obsolete;
            entry.line = line_number;
            next_fuzzy = false;
        }
        started = true;
        std::string parsed;
        if (!unescape(value, parsed)) {
            errors.push_back(where + "'" + std::string{keyword} + "' needs a quoted string");
            if (end == text.size()) break;
            continue;
        }
        if (keyword == "msgctxt") {
            entry.context = std::move(parsed);
            target = &*entry.context;
        } else if (keyword == "msgid") {
            entry.id = std::move(parsed);
            target = &entry.id;
        } else if (keyword == "msgid_plural") {
            entry.id_plural = std::move(parsed);
            target = &*entry.id_plural;
        } else if (keyword == "msgstr" || keyword.starts_with("msgstr[")) {
            std::size_t index = 0;
            if (keyword != "msgstr") {
                index = static_cast<std::size_t>(std::atoi(std::string{keyword.substr(7)}.c_str()));
            }
            if (entry.strs.size() <= index) entry.strs.resize(index + 1);
            entry.strs[index] = std::move(parsed);
            target = &entry.strs[index];
            has_str = true;
        } else {
            errors.push_back(where + "unknown keyword '" + std::string{keyword} + "'");
        }
        if (end == text.size()) break;
    }
    finish();

    LanguageFile file;
    PluralForms forms;
    std::string header_language;
    for (const Entry& e : entries) {
        if (e.id.empty() && !e.context && !e.strs.empty()) {
            // The header: "Language: fr\nPlural-Forms: ...\n".
            std::string_view header = e.strs[0];
            for (std::size_t k = 0; k < header.size();) {
                const std::size_t line_end = std::min(header.find('\n', k), header.size());
                const std::string_view field = header.substr(k, line_end - k);
                if (field.starts_with("Language:")) {
                    header_language = std::string{trim(field.substr(9))};
                } else if (field.starts_with("Plural-Forms:")) {
                    if (const std::optional<PluralForms> parsed = parse_plural_forms(field.substr(13))) {
                        forms = *parsed;
                    } else {
                        errors.push_back("line " + std::to_string(e.line) + ": Plural-Forms does not parse");
                    }
                }
                k = line_end + 1;
            }
        }
    }
    file.locale = normalize_locale(header_language.empty() ? locale : std::string_view{header_language});
    if (file.locale.empty()) {
        errors.emplace_back("no language: the header has no 'Language:' and none was given");
    }
    file.name = file.locale;
    for (const Entry& e : entries) {
        if (e.obsolete || e.fuzzy || (e.id.empty() && !e.context)) {
            continue; // obsolete, unchecked, or the header
        }
        const std::string& key = e.context ? *e.context : e.id;
        std::string message;
        if (e.id_plural) {
            if (e.strs.empty() || std::ranges::any_of(e.strs, [](const std::string& s) { return s.empty(); })) {
                continue; // untranslated
            }
            if (static_cast<i64>(e.strs.size()) != forms.count) {
                errors.push_back("line " + std::to_string(e.line) + ": '" + key + "' has " + std::to_string(e.strs.size()) +
                                 " plural forms, Plural-Forms says " + std::to_string(forms.count));
                continue;
            }
            message = plural_message(e.strs, forms, file.locale);
        } else {
            if (e.strs.empty() || e.strs[0].empty()) {
                continue; // untranslated
            }
            message = e.strs[0];
        }
        if (!file.strings.emplace(key, std::move(message)).second) {
            errors.push_back("line " + std::to_string(e.line) + ": '" + key + "' is given twice");
        }
    }
    if (errors.size() != errors_before) {
        return false;
    }
    out = std::move(file);
    return true;
}

std::string write_gettext(const LanguageFile& base, const LanguageFile* translation) {
    std::string out;
    out += "# Generated by kin from the ";
    out += base.locale;
    out += " text: msgctxt is the key, msgid the ";
    out += base.locale;
    out += " text, msgstr a kin message (see docs/localization.md).\n";
    out += "msgid \"\"\nmsgstr \"\"\n";
    out += "\"Content-Type: text/plain; charset=UTF-8\\n\"\n";
    if (translation) {
        out += "\"Language: " + translation->locale + "\\n\"\n";
    }
    out += "\"X-Generator: kin\\n\"\n";
    std::vector<const std::string*> keys;
    for (const auto& [key, text] : base.strings) {
        keys.push_back(&key);
    }
    std::ranges::sort(keys, {}, [](const std::string* key) { return std::string_view{*key}; });
    for (const std::string* key : keys) {
        out += '\n';
        write_field(out, "msgctxt", *key);
        write_field(out, "msgid", base.strings.at(*key));
        std::string_view translated;
        if (translation) {
            if (const auto found = translation->strings.find(*key); found != translation->strings.end()) {
                translated = found->second;
            }
        }
        write_field(out, "msgstr", translated);
    }
    return out;
}

} // namespace kin
