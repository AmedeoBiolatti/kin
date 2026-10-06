#include <kin/l10n/localization.hpp>

#include <kin/assets/file_watcher.hpp>
#include <kin/core/json.hpp>
#include <kin/core/json_value.hpp>

#include <SDL3/SDL_locale.h>
#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <sstream>

namespace kin {
namespace {

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
char upper(char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; }

bool same_text_ignoring_case(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::ranges::equal(a, b, [](char x, char y) { return lower(x) == lower(y); });
}

std::string to_lower(std::string_view s) {
    std::string out{s};
    std::ranges::transform(out, out.begin(), lower);
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
        s.remove_suffix(1);
    }
    return s;
}

bool parse_direction(std::string_view text, TextDirection& out) {
    if (text == "ltr") {
        out = TextDirection::LeftToRight;
        return true;
    }
    if (text == "rtl") {
        out = TextDirection::RightToLeft;
        return true;
    }
    return false;
}

void flatten(const JsonValue& node, const std::string& prefix, TranslationTable& out, std::vector<std::string>& errors) {
    for (const auto& [name, value] : node.members()) {
        const std::string key = prefix.empty() ? name : prefix + "." + name;
        if (name.empty()) {
            errors.push_back("strings: an empty key under '" + prefix + "'");
        } else if (value.is_object()) {
            flatten(value, key, out, errors);
        } else if (!value.is_string()) {
            errors.push_back("strings: '" + key + "' is not a string");
        } else if (!out.emplace(key, value.as_string()).second) {
            errors.push_back("strings: '" + key + "' is given twice");
        }
    }
}

// RFC 4180 records: fields per row. False on an unclosed quote.
bool split_csv(std::string_view text, std::vector<std::vector<std::string>>& rows, std::vector<i32>& lines,
               std::vector<std::string>& errors) {
    std::vector<std::string> row;
    std::string field;
    bool quoted = false;
    bool field_started = false;
    i32 line = 1;
    i32 row_line = 1;
    const auto end_row = [&] {
        row.push_back(std::move(field));
        field.clear();
        if (!(row.size() == 1 && row[0].empty())) {
            rows.push_back(std::move(row));
            lines.push_back(row_line);
        }
        row.clear();
        field_started = false;
    };
    for (std::size_t k = 0; k < text.size(); ++k) {
        const char c = text[k];
        if (quoted) {
            if (c == '"') {
                if (k + 1 < text.size() && text[k + 1] == '"') {
                    field += '"';
                    ++k;
                } else {
                    quoted = false;
                }
            } else {
                if (c == '\n') {
                    ++line;
                }
                if (c != '\r' || k + 1 >= text.size() || text[k + 1] != '\n') {
                    field += c;
                }
            }
            continue;
        }
        if (c == '"' && !field_started) {
            quoted = true;
            field_started = true;
        } else if (c == ',') {
            row.push_back(std::move(field));
            field.clear();
            field_started = false;
        } else if (c == '\n' || c == '\r') {
            if (c == '\r' && k + 1 < text.size() && text[k + 1] == '\n') {
                ++k;
            }
            end_row();
            ++line;
            row_line = line;
        } else {
            field += c;
            field_started = true;
        }
    }
    if (quoted) {
        errors.push_back("line " + std::to_string(row_line) + ": a quoted field is not closed");
        return false;
    }
    if (field_started || !row.empty()) {
        end_row();
    }
    return true;
}

bool ignored_column(std::string_view name) {
    const std::string n = to_lower(trim(name));
    return n.empty() || n.starts_with('#') || n == "comment" || n == "comments" || n == "context" || n == "notes" ||
           n == "note" || n == "description";
}

bool valid_tag(std::string_view tag) {
    if (tag.empty()) {
        return false;
    }
    for (char c : tag) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) {
            return false;
        }
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Locale tags

std::string normalize_locale(std::string_view tag) {
    tag = trim(tag);
    // POSIX forms: "fr_CA.UTF-8@euro".
    tag = tag.substr(0, std::min(tag.find('.'), tag.find('@')));
    std::string out;
    std::size_t part = 0;
    std::size_t k = 0;
    while (k <= tag.size()) {
        const std::size_t end = std::min(tag.find_first_of("-_", k), tag.size());
        const std::string_view subtag = tag.substr(k, end - k);
        if (!subtag.empty()) {
            if (!out.empty()) {
                out += '-';
            }
            if (part == 0) {
                out += to_lower(subtag);
            } else if (subtag.size() == 4 && std::isalpha(static_cast<unsigned char>(subtag[0]))) {
                out += upper(subtag[0]); // script: "Hans"
                out += to_lower(subtag.substr(1));
            } else if (subtag.size() == 2) {
                out += upper(subtag[0]); // region: "CA"
                out += upper(subtag[1]);
            } else {
                out += to_lower(subtag); // "419", variants
            }
            ++part;
        }
        k = end + 1;
    }
    return out;
}

std::string_view locale_language(std::string_view tag) {
    return tag.substr(0, std::min(tag.find_first_of("-_"), tag.size()));
}

TextDirection locale_direction(std::string_view tag) {
    static constexpr std::array rtl{std::string_view{"ar"}, std::string_view{"arc"}, std::string_view{"ckb"},
                                    std::string_view{"dv"}, std::string_view{"fa"},  std::string_view{"he"},
                                    std::string_view{"iw"}, std::string_view{"ks"},  std::string_view{"ps"},
                                    std::string_view{"sd"}, std::string_view{"syr"}, std::string_view{"ug"},
                                    std::string_view{"ur"}, std::string_view{"yi"}};
    const std::string normal = normalize_locale(tag);
    const std::string_view language = locale_language(normal);
    // A script subtag decides where it is given: "pa-Arab" is right to left.
    for (const std::string& part : locale_fallbacks(normal)) {
        const std::string_view last = std::string_view{part}.substr(part.rfind('-') + 1);
        if (last == "Arab" || last == "Hebr" || last == "Thaa" || last == "Syrc" || last == "Nkoo" || last == "Adlm") {
            return TextDirection::RightToLeft;
        }
        if (last == "Latn" || last == "Cyrl") {
            return TextDirection::LeftToRight;
        }
    }
    return std::ranges::find(rtl, language) != rtl.end() ? TextDirection::RightToLeft : TextDirection::LeftToRight;
}

std::vector<std::string> locale_fallbacks(std::string_view tag) {
    std::vector<std::string> out;
    std::string current = normalize_locale(tag);
    while (!current.empty()) {
        out.push_back(current);
        const std::size_t dash = current.rfind('-');
        if (dash == std::string::npos) {
            break;
        }
        current.resize(dash);
    }
    return out;
}

std::string match_locale(std::span<const std::string> preferred, std::span<const std::string> available) {
    std::vector<std::string> normal_available;
    normal_available.reserve(available.size());
    for (const std::string& tag : available) {
        normal_available.push_back(normalize_locale(tag));
    }
    const auto index_of = [&](std::string_view tag) -> std::ptrdiff_t {
        for (std::size_t i = 0; i < normal_available.size(); ++i) {
            if (normal_available[i] == tag) {
                return static_cast<std::ptrdiff_t>(i);
            }
        }
        return -1;
    };
    for (const std::string& wanted : preferred) {
        const std::vector<std::string> chain = locale_fallbacks(wanted);
        for (const std::string& tag : chain) {
            if (const auto i = index_of(tag); i >= 0) {
                return available[static_cast<std::size_t>(i)];
            }
        }
        if (chain.empty()) {
            continue;
        }
        // Another region of the language: the shortest such tag, then the first.
        const std::string_view language = locale_language(chain.back());
        std::ptrdiff_t best = -1;
        for (std::size_t i = 0; i < normal_available.size(); ++i) {
            if (locale_language(normal_available[i]) == language &&
                (best < 0 || normal_available[i].size() < normal_available[static_cast<std::size_t>(best)].size())) {
                best = static_cast<std::ptrdiff_t>(i);
            }
        }
        if (best >= 0) {
            return available[static_cast<std::size_t>(best)];
        }
    }
    return {};
}

std::vector<std::string> system_locales() {
    std::vector<std::string> out;
    int count = 0;
    SDL_Locale** locales = SDL_GetPreferredLocales(&count);
    if (!locales) {
        return out;
    }
    for (int i = 0; i < count; ++i) {
        const SDL_Locale* l = locales[i];
        if (!l || !l->language) {
            continue;
        }
        std::string tag = l->language;
        if (l->country) {
            tag += '-';
            tag += l->country;
        }
        tag = normalize_locale(tag);
        if (!tag.empty() && std::ranges::find(out, tag) == out.end()) {
            out.push_back(std::move(tag));
        }
    }
    SDL_free(locales);
    return out;
}

// ---------------------------------------------------------------------------
// Language files

bool parse_language_file(std::string_view text, LanguageFile& out, std::vector<std::string>& errors) {
    const JsonParseResult parsed = parse_json(text);
    if (!parsed.ok()) {
        errors.push_back("at byte " + std::to_string(parsed.position) + ": " + parsed.error);
        return false;
    }
    const JsonValue& root = *parsed.value;
    if (!root.is_object()) {
        errors.emplace_back("a language file is a JSON object");
        return false;
    }
    const std::size_t errors_before = errors.size();
    LanguageFile file;
    for (const auto& [name, value] : root.members()) {
        if (name != "locale" && name != "name" && name != "direction" && name != "strings" && !name.starts_with('$')) {
            errors.push_back("unknown member '" + name + "'");
        }
    }
    const JsonValue* locale = root.find("locale");
    if (!locale || !locale->is_string() || !valid_tag(locale->as_string())) {
        errors.emplace_back("'locale' must be a language tag, like \"fr\" or \"pt-BR\"");
    } else {
        file.locale = normalize_locale(locale->as_string());
        file.direction = locale_direction(file.locale);
    }
    file.name = root.string_at("name", file.locale);
    if (const JsonValue* direction = root.find("direction")) {
        if (!direction->is_string() || !parse_direction(direction->as_string(), file.direction)) {
            errors.emplace_back("'direction' must be \"ltr\" or \"rtl\"");
        }
    }
    const JsonValue* strings = root.find("strings");
    if (!strings || !strings->is_object()) {
        errors.emplace_back("'strings' must be an object");
    } else {
        flatten(*strings, {}, file.strings, errors);
    }
    if (errors.size() != errors_before) {
        return false;
    }
    out = std::move(file);
    return true;
}

bool parse_language_csv(std::string_view text, std::vector<LanguageFile>& out, std::vector<std::string>& errors) {
    if (text.starts_with("\xEF\xBB\xBF")) {
        text.remove_prefix(3);
    }
    std::vector<std::vector<std::string>> rows;
    std::vector<i32> lines;
    if (!split_csv(text, rows, lines, errors)) {
        return false;
    }
    if (rows.empty()) {
        errors.emplace_back("the file is empty: it needs a header row like \"key,en,fr\"");
        return false;
    }
    const std::size_t errors_before = errors.size();
    const std::vector<std::string>& header = rows[0];
    std::ptrdiff_t key_column = -1;
    std::vector<std::pair<std::size_t, LanguageFile>> columns;
    for (std::size_t c = 0; c < header.size(); ++c) {
        const std::string_view name = trim(header[c]);
        if (same_text_ignoring_case(name, "key") || same_text_ignoring_case(name, "id")) {
            if (key_column >= 0) {
                errors.emplace_back("line 1: two key columns");
            }
            key_column = static_cast<std::ptrdiff_t>(c);
        } else if (ignored_column(name)) {
            continue;
        } else if (!valid_tag(name)) {
            errors.push_back("line 1: column '" + std::string{name} + "' is not a language tag");
        } else {
            LanguageFile file;
            file.locale = normalize_locale(name);
            file.name = file.locale;
            file.direction = locale_direction(file.locale);
            if (std::ranges::any_of(columns, [&](const auto& col) { return col.second.locale == file.locale; })) {
                errors.push_back("line 1: two columns for '" + file.locale + "'");
            }
            columns.emplace_back(c, std::move(file));
        }
    }
    if (key_column < 0) {
        errors.emplace_back("line 1: no \"key\" column");
    }
    if (columns.empty()) {
        errors.emplace_back("line 1: no language columns");
    }
    if (errors.size() != errors_before) {
        return false;
    }
    for (std::size_t r = 1; r < rows.size(); ++r) {
        const std::vector<std::string>& row = rows[r];
        const std::string line = "line " + std::to_string(lines[r]) + ": ";
        const auto cell = [&](std::size_t c) -> std::string_view {
            return c < row.size() ? std::string_view{row[c]} : std::string_view{};
        };
        const std::string_view key = trim(cell(static_cast<std::size_t>(key_column)));
        if (key.empty() || key.starts_with('#')) {
            continue;
        }
        if (row.size() > header.size() && std::ranges::any_of(row.begin() + static_cast<std::ptrdiff_t>(header.size()),
                                                              row.end(), [](const std::string& f) { return !f.empty(); })) {
            errors.push_back(line + "more fields than the header has columns");
            continue;
        }
        for (auto& [c, file] : columns) {
            const std::string_view value = cell(c);
            if (key == "@name") {
                if (!value.empty()) {
                    file.name = std::string{trim(value)};
                }
            } else if (key == "@direction") {
                if (!value.empty() && !parse_direction(trim(value), file.direction)) {
                    errors.push_back(line + "@direction for '" + file.locale + "' must be ltr or rtl");
                }
            } else if (key.starts_with('@')) {
                errors.push_back(line + "unknown row '" + std::string{key} + "'");
                break;
            } else if (!value.empty() && !file.strings.emplace(std::string{key}, std::string{value}).second) {
                errors.push_back(line + "'" + std::string{key} + "' is given twice");
                break;
            }
        }
    }
    if (errors.size() != errors_before) {
        return false;
    }
    std::vector<LanguageFile> files;
    files.reserve(columns.size());
    for (auto& [c, file] : columns) {
        files.push_back(std::move(file));
    }
    out = std::move(files);
    return true;
}

bool load_language_files(const std::filesystem::path& path, std::vector<LanguageFile>& out,
                         std::vector<std::string>& errors) {
    const std::optional<std::string> text = read_text_file(path);
    if (!text) {
        errors.push_back(path.string() + ": cannot be read");
        return false;
    }
    std::vector<std::string> own;
    bool ok = false;
    if (same_text_ignoring_case(path.extension().string(), ".csv")) {
        ok = parse_language_csv(*text, out, own);
    } else {
        LanguageFile file;
        ok = parse_language_file(*text, file, own);
        if (ok) {
            out.clear();
            out.push_back(std::move(file));
        }
    }
    for (std::string& message : own) {
        errors.push_back(path.filename().string() + ": " + message);
    }
    return ok;
}

std::string write_language_file(const LanguageFile& file) {
    std::ostringstream out;
    JsonWriter json(out);
    json.begin_object();
    json.field("locale", file.locale);
    json.field("name", file.name);
    if (file.direction != locale_direction(file.locale)) {
        json.field("direction", file.direction == TextDirection::RightToLeft ? "rtl" : "ltr");
    }
    std::vector<const std::pair<const std::string, std::string>*> sorted;
    sorted.reserve(file.strings.size());
    for (const auto& entry : file.strings) {
        sorted.push_back(&entry);
    }
    std::ranges::sort(sorted, {}, [](const auto* entry) { return std::string_view{entry->first}; });
    json.key("strings").begin_object();
    for (const auto* entry : sorted) {
        json.field(entry->first, entry->second);
    }
    json.end_object();
    json.end_object();
    out << '\n';
    return out.str();
}

} // namespace kin
