#pragma once

#include <kin/core/types.hpp>
#include <kin/l10n/message_format.hpp>

#include <filesystem>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kin {

class FileWatcher;

// Which way a language's text runs.
enum class TextDirection : u8 { LeftToRight, RightToLeft };

// ---------------------------------------------------------------------------
// Locale tags

// A tag in BCP 47 form: "fr_ca" and "fr-CA.UTF-8" become "fr-CA", "zh-hans-cn"
// becomes "zh-Hans-CN". Empty for an empty tag.
std::string normalize_locale(std::string_view tag);
// The language part of a tag: "pt" for "pt-BR".
std::string_view locale_language(std::string_view tag);
// RightToLeft for Arabic, Hebrew, Persian, Urdu and the other right-to-left
// scripts' languages; LeftToRight for the rest.
TextDirection locale_direction(std::string_view tag);
// The tag and the shorter tags it falls back to: "zh-Hant-TW", "zh-Hant", "zh".
std::vector<std::string> locale_fallbacks(std::string_view tag);
// The best of `available` for a player who prefers `preferred` (most wanted
// first): for each preferred tag in turn, the same tag, then one it falls back
// to ("fr-CA" takes "fr"), then another region of its language ("fr-CA" takes
// "fr-FR"). Empty when no preferred language is available.
std::string match_locale(std::span<const std::string> preferred, std::span<const std::string> available);
// The player's languages as the system lists them, most wanted first,
// normalized ("en-GB", "en"). Empty when the system says nothing.
std::vector<std::string> system_locales();

// ---------------------------------------------------------------------------
// Language files

// Lookups by std::string_view without a temporary std::string.
struct TranslationHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
};
using TranslationTable = std::unordered_map<std::string, std::string, TranslationHash, std::equal_to<>>;

// One language's strings, from a .kinlang file or a column of a CSV.
struct LanguageFile {
    std::string locale;      // "fr", "pt-BR" (normalized)
    std::string name;        // its own name, for a language menu: "Français"
    TextDirection direction = TextDirection::LeftToRight;
    TranslationTable strings; // key -> pattern (see message_format.hpp)
};

// A .kinlang file is JSON:
//
//   {
//     "locale": "fr",
//     "name": "Français",           // optional, defaults to the tag
//     "direction": "ltr",           // optional: "ltr" or "rtl", defaults from the tag
//     "strings": {
//       "menu": { "play": "Jouer", "quit": "Quitter" },   // nests: "menu.play"
//       "shop.gold": "{gold} pièces d'or",
//       "hand.cards": "{n, plural, one {# carte} other {# cartes}}"
//     }
//   }
//
// Values are strings; objects nest with '.'. Members starting with '$' are
// left for tools ("$schema", "$comment"). The same key written twice
// (as "a.b" and as {"a": {"b"}}) is an error. On failure `out` is untouched
// and `errors` says why.
bool parse_language_file(std::string_view text, LanguageFile& out, std::vector<std::string>& errors);

// A CSV, the form spreadsheets and translation services trade in: a header
// row naming a "key" column and one column per locale, then a row per key.
//
//   key,en,fr,de,comment
//   @name,English,Français,Deutsch,
//   menu.play,Play,Jouer,Spielen,The main menu's first button
//   shop.gold,"{gold} gold","{gold} pièces d'or","{gold} Gold",
//
// Fields follow RFC 4180 (quotes round a field with commas, quotes or line
// breaks; "" inside is a quote). A column named comment, context, notes or
// description, or starting with '#', is ignored, as is a row whose key is
// empty or starts with '#'. An empty cell is an untranslated key. Rows "@name"
// and "@direction" give each language's name and direction. A UTF-8 byte
// order mark is skipped.
bool parse_language_csv(std::string_view text, std::vector<LanguageFile>& out, std::vector<std::string>& errors);

// Either, by the path's extension (.kinlang or .csv).
bool load_language_files(const std::filesystem::path& path, std::vector<LanguageFile>& out,
                         std::vector<std::string>& errors);

// A file in .kinlang form, its keys sorted and written flat ("menu.play").
std::string write_language_file(const LanguageFile& file);

// ---------------------------------------------------------------------------
// Localization

struct LanguageInfo {
    std::string locale;
    std::string name;
    TextDirection direction = TextDirection::LeftToRight;
};

// Something validate() found.
struct LocalizationIssue {
    enum class Severity : u8 { Warning, Error };
    Severity severity = Severity::Warning;
    std::string locale;
    std::string key;
    std::string message;
};

struct PseudoLocaleOptions {
    // How much longer each text grows, as a share of its length: translations
    // run longer than English (German by a third), and a layout should fit them.
    f32 expansion = 0.35f;
    // Square brackets round each text, so cut-off text shows.
    bool brackets = true;
};

// A game's translations and the language it is shown in.
//
//   kin::Localization l10n;
//   l10n.load_directory(root / "lang", errors);   // en.kinlang, fr.kinlang, ...
//   l10n.set_locale(kin::system_locales());       // or the player's saved choice
//   ui.label(l10n.text("menu.play"));
//   ui.label(l10n.tr("shop.gold", {{"gold", 1250}}));
//
// Every key looked up runs down a chain: the locale ("fr-CA"), the tags it
// falls back to ("fr"), then the base locale (English unless changed). A key
// in none of them shows as the key itself, and is logged once and listed in
// missing_keys(), so a run can report what is untranslated.
//
// The pseudo-locale (pseudo_locale, "en-XA") shows the base language's text
// accented and lengthened, "[Ƥĺåý ~~]": text that stays plain was never
// looked up, and text that is cut off will be in a longer language.
//
// Lookups may run on several threads at once; loading and switching may not
// run alongside them.
class Localization {
public:
    static constexpr std::string_view pseudo_locale = "en-XA";

    Localization();
    ~Localization();
    Localization(const Localization&) = delete;
    Localization& operator=(const Localization&) = delete;

    // --- Loading -------------------------------------------------------------
    //
    // Strings come from sources: a file, or a name given to add(). Loading a
    // source again replaces what it gave before (keys it no longer has are
    // gone), and several sources may add to one locale (ui.kinlang and
    // dialogue.kinlang both in French). A later source wins a key both give.

    // A .kinlang or .csv file. False, with messages, if it cannot be read or
    // parsed; what the file gave before stays.
    bool load_file(const std::filesystem::path& path, std::vector<std::string>& errors);
    // Every .kinlang and .csv file in `dir`, in name order. With `watcher`,
    // each reloads when it changes (see FileWatcher::load_and_watch).
    bool load_directory(const std::filesystem::path& dir, std::vector<std::string>& errors,
                        FileWatcher* watcher = nullptr);
    // A file's contents, as if read from `path` (whose extension picks the format).
    bool load_text(const std::filesystem::path& path, std::string_view text, std::vector<std::string>& errors);
    // Languages from code, under the source name `source`.
    void add(std::string_view source, std::vector<LanguageFile> languages);
    void add(std::string_view source, LanguageFile language);
    void remove_source(std::string_view source);
    void clear();

    // The loaded languages, by tag (the pseudo-locale is not listed).
    std::vector<LanguageInfo> languages() const;
    bool has_language(std::string_view locale) const;

    // --- The language shown ----------------------------------------------------

    // The language every chain ends in, and the one validate() checks the
    // others against. "en" unless changed.
    void set_base_locale(std::string_view locale);
    const std::string& base_locale() const { return _base_locale; }

    // Shows the loaded language that best fits `tag` (see match_locale), or
    // the base locale. Returns the locale now shown.
    const std::string& set_locale(std::string_view tag);
    // The same for a list of preferences, most wanted first (system_locales()).
    const std::string& set_locale(std::span<const std::string> preferred);
    const std::string& locale() const { return _locale; }
    TextDirection direction() const;
    bool pseudo() const { return _locale == pseudo_locale; }
    void set_pseudo_options(PseudoLocaleOptions options);

    // Goes up whenever what lookups return may have changed (a locale switch,
    // a load): text kept from an earlier lookup is stale if it differs.
    u64 generation() const { return _generation; }

    // --- Lookups -------------------------------------------------------------

    // Whether some language in the chain has `key`.
    bool has(std::string_view key) const;
    // The raw pattern for `key` from the chain, or null.
    const std::string* pattern(std::string_view key) const;
    // The text for `key` with nothing put in (quotes resolved, `{arg}` left).
    // Valid until the next load or locale change; costs no allocation after
    // a key's first lookup.
    std::string_view text(std::string_view key) const;
    // The text for `key` with `args` put in (see message_format.hpp).
    std::string tr(std::string_view key) const { return std::string{text(key)}; }
    std::string tr(std::string_view key, MessageArgs args) const;
    std::string tr(std::string_view key, std::initializer_list<MessageArg> args) const {
        return tr(key, MessageArgs{args.begin(), args.size()});
    }
    // `pattern` formatted for the locale shown (pseudo-localized if it is).
    std::string format(std::string_view pattern, MessageArgs args = {}) const;
    // The locale plural rules and numbers follow: the shown one, or the base
    // for the pseudo-locale.
    const std::string& formatting_locale() const;

    // Keys looked up and not found, sorted.
    std::vector<std::string> missing_keys() const;
    void clear_missing_keys();

    // Checks every language against the base: keys missing or extra, broken
    // patterns, arguments the base does not give, plural forms the language
    // needs. Errors break text on screen; warnings are worth a look.
    std::vector<LocalizationIssue> validate() const;

private:
    struct Source {
        std::string name;
        std::vector<LanguageFile> languages;
    };
    struct Language {
        LanguageInfo info;
        TranslationTable strings; // all sources merged
    };
    struct Watch {
        FileWatcher* watcher = nullptr;
        std::weak_ptr<const void> lifetime;
        u32 id = 0;
    };

    void rebuild();
    void resolve_chain();
    const std::string* find(std::string_view key) const;
    std::string_view missing(std::string_view key) const;
    MessageFormatOptions format_options() const;
    std::string pseudo_wrap(std::string text) const;

    std::vector<Source> _sources;
    std::vector<Watch> _watches;
    std::map<std::string, Language, std::less<>> _languages;
    std::string _base_locale = "en";
    std::string _requested_locale = "en";
    std::string _locale = "en";
    std::vector<const TranslationTable*> _chain;
    PseudoLocaleOptions _pseudo;
    u64 _generation = 1;

    mutable std::mutex _mutex; // guards the caches below
    mutable std::unordered_map<std::string, std::unique_ptr<std::string>, TranslationHash, std::equal_to<>> _text_cache;
    mutable std::set<std::string, std::less<>> _missing;
};

// The Localization kin's own systems read (retained ui2 text, dialogue,
// scripts' tr()), or null. Set it once at start-up; the object must outlive
// its use.
Localization* active_localization();
void set_active_localization(Localization* localization);

// active_localization()'s lookups; without one, the key itself.
std::string tr(std::string_view key);
std::string tr(std::string_view key, MessageArgs args);
std::string tr(std::string_view key, std::initializer_list<MessageArg> args);

} // namespace kin
