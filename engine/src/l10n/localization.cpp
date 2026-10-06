#include <kin/l10n/localization.hpp>

#include <kin/assets/file_watcher.hpp>
#include <kin/core/utf8.hpp>
#include <kin/platform/log.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>

namespace kin {
namespace {

std::atomic<Localization*> g_active{nullptr};

bool is_language_file(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    std::ranges::transform(ext, ext.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; });
    return ext == ".kinlang" || ext == ".csv";
}

std::string source_name(const std::filesystem::path& path) {
    return path.lexically_normal().generic_string();
}

// Accented look-alikes for the pseudo-locale, from Latin-1 and Latin
// Extended-A (letters without a near twin there stay as they are).
constexpr std::array<std::string_view, 26> accented_upper{
    "\xC3\x85", "B", "\xC3\x87", "\xC3\x90", "\xC3\x89", "F", "\xC4\x9C", "\xC4\xA4", "\xC3\x8E",
    "\xC4\xB4", "\xC4\xB6", "\xC4\xB9", "M", "\xC3\x91", "\xC3\x96", "P", "Q", "\xC5\x94",
    "\xC5\xA0", "\xC5\xA6", "\xC3\x9B", "V", "\xC5\xB4", "X", "\xC3\x9D", "\xC5\xBD"};
constexpr std::array<std::string_view, 26> accented_lower{
    "\xC3\xA5", "b", "\xC3\xA7", "\xC3\xB0", "\xC3\xA9", "f", "\xC4\x9D", "\xC4\xA5", "\xC3\xAE",
    "\xC4\xB5", "\xC4\xB7", "\xC4\xBA", "m", "\xC3\xB1", "\xC3\xB6", "\xC3\xBE", "q", "\xC5\x95",
    "\xC5\xA1", "\xC5\xA7", "\xC3\xBB", "v", "\xC5\xB5", "x", "\xC3\xBD", "\xC5\xBE"};

// Accents the letters of literal text, leaving markup ([b], <color>) and
// escapes (\n, %s) alone.
void accent(std::string_view text, std::string& out) {
    char closing = 0;
    for (std::size_t k = 0; k < text.size(); ++k) {
        const char c = text[k];
        if (closing) {
            out += c;
            if (c == closing) {
                closing = 0;
            }
            continue;
        }
        if (c == '[' || c == '<') {
            closing = c == '[' ? ']' : '>';
            out += c;
        } else if ((c == '\\' || c == '%') && k + 1 < text.size()) {
            out += c;
            out += text[++k];
        } else if (c >= 'a' && c <= 'z') {
            out += accented_lower[static_cast<std::size_t>(c - 'a')];
        } else if (c >= 'A' && c <= 'Z') {
            out += accented_upper[static_cast<std::size_t>(c - 'A')];
        } else {
            out += c;
        }
    }
}

std::size_t count_characters(std::string_view text) {
    std::size_t n = 0;
    for (std::size_t k = 0; k < text.size(); k = utf8_next(text, k)) {
        ++n;
    }
    return n;
}

void add_issue(std::vector<LocalizationIssue>& out, LocalizationIssue::Severity severity, std::string_view locale,
               std::string_view key, std::string message) {
    out.push_back({severity, std::string{locale}, std::string{key}, std::move(message)});
}

std::string join_categories(std::span<const PluralCategory> categories) {
    std::string out;
    for (PluralCategory c : categories) {
        if (!out.empty()) {
            out += ", ";
        }
        out += plural_category_name(c);
    }
    return out;
}

// What a translation's plural forms lack for its language.
void check_plurals(const MessageShape& shape, std::string_view locale, std::string_view key,
                   std::vector<LocalizationIssue>& out) {
    const std::span<const PluralCategory> needed = plural_categories(locale);
    for (const MessageShape::Argument& arg : shape.arguments) {
        if (arg.kind != "plural") {
            continue;
        }
        std::string lacking;
        for (PluralCategory c : needed) {
            if (std::ranges::find(arg.branches, plural_category_name(c)) == arg.branches.end()) {
                lacking += lacking.empty() ? "" : ", ";
                lacking += plural_category_name(c);
            }
        }
        if (!lacking.empty()) {
            add_issue(out, LocalizationIssue::Severity::Warning, locale, key,
                      "plural '" + arg.name + "' has no " + lacking + " (" + std::string{locale_language(locale)} +
                          " uses " + join_categories(needed) + "); those numbers take 'other'");
        }
    }
}

} // namespace

Localization::Localization() = default;

Localization::~Localization() {
    for (const Watch& watch : _watches) {
        if (!watch.lifetime.expired()) {
            watch.watcher->unwatch(watch.id);
        }
    }
    Localization* self = this;
    g_active.compare_exchange_strong(self, nullptr);
}

// --- Loading -----------------------------------------------------------------

bool Localization::load_file(const std::filesystem::path& path, std::vector<std::string>& errors) {
    std::vector<LanguageFile> languages;
    if (!load_language_files(path, languages, errors)) {
        return false;
    }
    add(source_name(path), std::move(languages));
    return true;
}

bool Localization::load_text(const std::filesystem::path& path, std::string_view text, std::vector<std::string>& errors) {
    std::vector<LanguageFile> languages;
    std::vector<std::string> own;
    std::string ext = path.extension().string();
    std::ranges::transform(ext, ext.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; });
    bool ok = false;
    if (ext == ".csv") {
        ok = parse_language_csv(text, languages, own);
    } else {
        LanguageFile file;
        ok = parse_language_file(text, file, own);
        if (ok) {
            languages.push_back(std::move(file));
        }
    }
    for (std::string& message : own) {
        errors.push_back(path.filename().string() + ": " + message);
    }
    if (ok) {
        add(source_name(path), std::move(languages));
    }
    return ok;
}

bool Localization::load_directory(const std::filesystem::path& dir, std::vector<std::string>& errors,
                                  FileWatcher* watcher) {
    std::error_code ec;
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec) && is_language_file(entry.path())) {
            files.push_back(entry.path());
        }
    }
    if (ec) {
        errors.push_back(dir.string() + ": " + ec.message());
        return false;
    }
    std::ranges::sort(files);
    bool ok = true;
    for (const std::filesystem::path& path : files) {
        if (!watcher) {
            ok = load_file(path, errors) && ok;
            continue;
        }
        u32 id = 0;
        const bool loaded = watcher->load_and_watch(
            path,
            [this, path](std::string_view text, std::vector<std::string>& messages) {
                return load_text(path, text, messages);
            },
            &errors, &id);
        ok = loaded && ok;
        _watches.push_back({watcher, watcher->lifetime(), id});
    }
    return ok;
}

void Localization::add(std::string_view source, std::vector<LanguageFile> languages) {
    for (LanguageFile& language : languages) {
        language.locale = normalize_locale(language.locale);
    }
    auto found = std::ranges::find(_sources, source, &Source::name);
    if (found == _sources.end()) {
        _sources.push_back({std::string{source}, std::move(languages)});
    } else {
        found->languages = std::move(languages);
    }
    rebuild();
}

void Localization::add(std::string_view source, LanguageFile language) {
    std::vector<LanguageFile> languages;
    languages.push_back(std::move(language));
    add(source, std::move(languages));
}

void Localization::remove_source(std::string_view source) {
    const auto removed = std::erase_if(_sources, [&](const Source& s) { return s.name == source; });
    if (removed) {
        rebuild();
    }
}

void Localization::clear() {
    _sources.clear();
    rebuild();
}

void Localization::rebuild() {
    _languages.clear();
    for (const Source& source : _sources) {
        for (const LanguageFile& file : source.languages) {
            auto [it, inserted] = _languages.try_emplace(file.locale);
            Language& language = it->second;
            if (inserted) {
                language.info = {file.locale, file.locale, locale_direction(file.locale)};
            }
            if (!file.name.empty() && file.name != file.locale) {
                language.info.name = file.name;
            }
            language.info.direction = file.direction;
            for (const auto& [key, pattern] : file.strings) {
                language.strings.insert_or_assign(key, pattern);
            }
        }
    }
    resolve_chain();
}

std::vector<LanguageInfo> Localization::languages() const {
    std::vector<LanguageInfo> out;
    out.reserve(_languages.size());
    for (const auto& [locale, language] : _languages) {
        out.push_back(language.info);
    }
    return out;
}

bool Localization::has_language(std::string_view locale) const {
    return _languages.contains(normalize_locale(locale));
}

// --- The language shown --------------------------------------------------------

void Localization::set_base_locale(std::string_view locale) {
    _base_locale = normalize_locale(locale);
    resolve_chain();
}

const std::string& Localization::set_locale(std::string_view tag) {
    _requested_locale = tag == pseudo_locale ? std::string{pseudo_locale} : normalize_locale(tag);
    resolve_chain();
    return _locale;
}

const std::string& Localization::set_locale(std::span<const std::string> preferred) {
    std::vector<std::string> available;
    for (const auto& [locale, language] : _languages) {
        available.push_back(locale);
    }
    std::string chosen = match_locale(preferred, available);
    return set_locale(chosen.empty() ? std::string_view{_base_locale} : std::string_view{chosen});
}

void Localization::resolve_chain() {
    if (_requested_locale == pseudo_locale) {
        _locale = std::string{pseudo_locale};
    } else {
        std::vector<std::string> available;
        for (const auto& [locale, language] : _languages) {
            available.push_back(locale);
        }
        const std::array wanted{_requested_locale};
        std::string chosen = match_locale(wanted, available);
        _locale = chosen.empty() ? _base_locale : std::move(chosen);
    }
    _chain.clear();
    const auto push = [&](std::string_view tag) {
        for (const std::string& part : locale_fallbacks(tag)) {
            const auto found = _languages.find(part);
            if (found != _languages.end() && std::ranges::find(_chain, &found->second.strings) == _chain.end()) {
                _chain.push_back(&found->second.strings);
            }
        }
    };
    if (!pseudo()) {
        push(_locale);
    }
    push(_base_locale);
    ++_generation;
    const std::scoped_lock lock(_mutex);
    _text_cache.clear();
}

TextDirection Localization::direction() const {
    const std::string& shown = pseudo() ? _base_locale : _locale;
    const auto found = _languages.find(shown);
    return found != _languages.end() ? found->second.info.direction : locale_direction(shown);
}

void Localization::set_pseudo_options(PseudoLocaleOptions options) {
    _pseudo = options;
    ++_generation;
    const std::scoped_lock lock(_mutex);
    _text_cache.clear();
}

const std::string& Localization::formatting_locale() const {
    return pseudo() ? _base_locale : _locale;
}

// --- Lookups -------------------------------------------------------------------

const std::string* Localization::find(std::string_view key) const {
    for (const TranslationTable* table : _chain) {
        if (const auto found = table->find(key); found != table->end()) {
            return &found->second;
        }
    }
    return nullptr;
}

bool Localization::has(std::string_view key) const {
    return find(key) != nullptr;
}

const std::string* Localization::pattern(std::string_view key) const {
    return find(key);
}

std::string_view Localization::missing(std::string_view key) const {
    const std::scoped_lock lock(_mutex);
    auto [it, inserted] = _missing.emplace(key);
    if (inserted) {
        log(LogLevel::Warn, "l10n", "missing text", {{"key", std::string{key}}, {"locale", _locale}});
    }
    return *it;
}

MessageFormatOptions Localization::format_options() const {
    MessageFormatOptions options;
    if (pseudo()) {
        options.transform_literal = accent;
    }
    return options;
}

std::string Localization::pseudo_wrap(std::string text) const {
    const std::size_t pad = static_cast<std::size_t>(std::lround(static_cast<f64>(count_characters(text)) * _pseudo.expansion));
    if (pad > 0) {
        text += ' ';
        text.append(pad > 1 ? pad - 1 : 1, '~');
    }
    if (_pseudo.brackets) {
        text.insert(text.begin(), '[');
        text += ']';
    }
    return text;
}

std::string Localization::format(std::string_view pattern, MessageArgs args) const {
    std::string out = format_message(pattern, formatting_locale(), args, format_options());
    return pseudo() ? pseudo_wrap(std::move(out)) : out;
}

std::string_view Localization::text(std::string_view key) const {
    const std::string* found = find(key);
    if (!found) {
        return missing(key);
    }
    if (!pseudo() && found->find_first_of("{}'") == std::string::npos) {
        return *found;
    }
    {
        const std::scoped_lock lock(_mutex);
        if (const auto cached = _text_cache.find(key); cached != _text_cache.end()) {
            return *cached->second;
        }
    }
    auto formatted = std::make_unique<std::string>(format(*found));
    const std::scoped_lock lock(_mutex);
    auto [it, inserted] = _text_cache.try_emplace(std::string{key}, std::move(formatted));
    return *it->second;
}

std::string Localization::tr(std::string_view key, MessageArgs args) const {
    const std::string* found = find(key);
    if (!found) {
        return std::string{missing(key)};
    }
    return format(*found, args);
}

std::vector<std::string> Localization::missing_keys() const {
    const std::scoped_lock lock(_mutex);
    return {_missing.begin(), _missing.end()};
}

void Localization::clear_missing_keys() {
    const std::scoped_lock lock(_mutex);
    _missing.clear();
}

// --- Validation ----------------------------------------------------------------

std::vector<LocalizationIssue> Localization::validate() const {
    using Severity = LocalizationIssue::Severity;
    std::vector<LocalizationIssue> issues;
    const auto base_found = _languages.find(_base_locale);
    if (base_found == _languages.end()) {
        add_issue(issues, Severity::Error, _base_locale, {}, "the base language is not loaded");
        return issues;
    }
    const TranslationTable& base = base_found->second.strings;

    std::unordered_map<std::string_view, MessageShape> base_shapes;
    for (const auto& [key, pattern] : base) {
        MessageShape shape;
        std::string error;
        if (!inspect_message(pattern, shape, error)) {
            add_issue(issues, Severity::Error, _base_locale, key, error);
            continue;
        }
        check_plurals(shape, _base_locale, key, issues);
        base_shapes.emplace(key, std::move(shape));
    }

    for (const auto& [locale, language] : _languages) {
        if (locale == _base_locale) {
            continue;
        }
        // The tables this language falls back to before the base.
        std::vector<const TranslationTable*> chain;
        bool reaches_base = false;
        for (const std::string& part : locale_fallbacks(locale)) {
            if (part == _base_locale) {
                reaches_base = true;
            }
            if (const auto found = _languages.find(part); found != _languages.end()) {
                chain.push_back(&found->second.strings);
            }
        }
        if (!reaches_base) {
            for (const auto& [key, pattern] : base) {
                const bool translated = std::ranges::any_of(chain, [&](const TranslationTable* t) { return t->contains(key); });
                if (!translated) {
                    add_issue(issues, Severity::Warning, locale, key, "not translated: shows the base text");
                }
            }
        }
        for (const auto& [key, pattern] : language.strings) {
            const auto base_shape = base_shapes.find(key);
            if (!base.contains(key)) {
                add_issue(issues, Severity::Warning, locale, key, "not in the base language");
            }
            MessageShape shape;
            std::string error;
            if (!inspect_message(pattern, shape, error)) {
                add_issue(issues, Severity::Error, locale, key, error);
                continue;
            }
            check_plurals(shape, locale, key, issues);
            if (base_shape == base_shapes.end()) {
                continue;
            }
            const auto& wanted = base_shape->second.arguments;
            for (const MessageShape::Argument& arg : shape.arguments) {
                const auto in_base = std::ranges::find(wanted, arg.name, &MessageShape::Argument::name);
                if (in_base == wanted.end()) {
                    add_issue(issues, Severity::Error, locale, key,
                              "'{" + arg.name + "}' is not in the base text, so nothing will fill it");
                } else if ((arg.kind == "plural" || arg.kind == "number") && in_base->kind == "select") {
                    add_issue(issues, Severity::Error, locale, key,
                              "'{" + arg.name + "}' is a number here but a choice in the base text");
                }
            }
            for (const MessageShape::Argument& arg : wanted) {
                if (std::ranges::find(shape.arguments, arg.name, &MessageShape::Argument::name) == shape.arguments.end()) {
                    add_issue(issues, Severity::Warning, locale, key, "does not use '{" + arg.name + "}'");
                }
            }
        }
    }
    std::ranges::stable_sort(issues, [](const LocalizationIssue& a, const LocalizationIssue& b) {
        return std::tie(a.locale, a.key) < std::tie(b.locale, b.key);
    });
    return issues;
}

// --- The active localization -----------------------------------------------------

Localization* active_localization() {
    return g_active.load(std::memory_order_acquire);
}

void set_active_localization(Localization* localization) {
    g_active.store(localization, std::memory_order_release);
}

std::string tr(std::string_view key) {
    const Localization* l10n = active_localization();
    return l10n ? l10n->tr(key) : std::string{key};
}

std::string tr(std::string_view key, MessageArgs args) {
    const Localization* l10n = active_localization();
    return l10n ? l10n->tr(key, args) : std::string{key};
}

std::string tr(std::string_view key, std::initializer_list<MessageArg> args) {
    return tr(key, MessageArgs{args.begin(), args.size()});
}

} // namespace kin
