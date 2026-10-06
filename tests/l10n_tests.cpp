#include <kin/assets/file_watcher.hpp>
#include <kin/l10n/localization.hpp>
#include <kin/platform/log.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using kin::PluralCategory;

constexpr std::string_view nbsp = "\xC2\xA0";

void write(const fs::path& path, const std::string& text) {
    static fs::file_time_type next = fs::file_time_type::clock::now();
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << text;
    }
    next += std::chrono::seconds(2);
    fs::last_write_time(path, next);
}

bool has_issue(const std::vector<kin::LocalizationIssue>& issues, std::string_view locale, std::string_view key,
               std::string_view fragment, kin::LocalizationIssue::Severity severity) {
    return std::ranges::any_of(issues, [&](const kin::LocalizationIssue& issue) {
        return issue.locale == locale && issue.key == key && issue.severity == severity &&
               issue.message.find(fragment) != std::string::npos;
    });
}

void test_plural_rules() {
    using enum PluralCategory;
    assert(kin::plural_category("en", 1) == One);
    assert(kin::plural_category("en", 0) == Other);
    assert(kin::plural_category("en", 1.5) == Other);
    assert(kin::plural_category("en-GB", 2) == Other);

    assert(kin::plural_category("fr", 0) == One);
    assert(kin::plural_category("fr", 1.5) == One);
    assert(kin::plural_category("fr", 2) == Other);
    assert(kin::plural_category("fr", 1000000) == Many);
    assert(kin::plural_category("fr-CA", 0) == One);
    assert(kin::plural_category("pt", 0) == One);
    assert(kin::plural_category("pt-PT", 0) == Other);

    for (const int n : {1, 21, 101}) assert(kin::plural_category("ru", n) == One);
    for (const int n : {2, 3, 4, 22, 104}) assert(kin::plural_category("ru", n) == Few);
    for (const int n : {0, 5, 11, 12, 14, 25, 111}) assert(kin::plural_category("ru", n) == Many);
    assert(kin::plural_category("ru", 1.5) == Other);
    assert(kin::plural_category("uk", 21) == One);

    assert(kin::plural_category("pl", 1) == One);
    assert(kin::plural_category("pl", 21) == Many);
    assert(kin::plural_category("pl", 22) == Few);
    assert(kin::plural_category("pl", 12) == Many);

    assert(kin::plural_category("cs", 3) == Few);
    assert(kin::plural_category("cs", 5) == Other);
    assert(kin::plural_category("cs", 1.5) == Many);

    assert(kin::plural_category("ar", 0) == Zero);
    assert(kin::plural_category("ar", 1) == One);
    assert(kin::plural_category("ar", 2) == Two);
    assert(kin::plural_category("ar", 7) == Few);
    assert(kin::plural_category("ar", 103) == Few);
    assert(kin::plural_category("ar", 11) == Many);
    assert(kin::plural_category("ar", 100) == Other);

    assert(kin::plural_category("he", 2) == Two);
    assert(kin::plural_category("ja", 1) == Other);
    assert(kin::plural_category("zh-Hans", 1) == Other);
    assert(kin::plural_category("xx", 1) == One); // unknown: one/other

    assert(kin::plural_categories("en").size() == 2);
    assert(kin::plural_categories("ar").size() == 6);
    assert(kin::plural_categories("ja").size() == 1);
    assert(kin::plural_categories("ru").back() == Other);
}

void test_ordinals() {
    using enum PluralCategory;
    assert(kin::ordinal_category("en", 1) == One && kin::ordinal_category("en", 21) == One);
    assert(kin::ordinal_category("en", 11) == Other && kin::ordinal_category("en", 12) == Other);
    assert(kin::ordinal_category("en", 2) == Two && kin::ordinal_category("en", 3) == Few);
    assert(kin::ordinal_category("en", 113) == Other && kin::ordinal_category("en", 104) == Other);
    assert(kin::ordinal_category("fr", 1) == One && kin::ordinal_category("fr", 2) == Other);
    assert(kin::ordinal_category("it", 8) == Many && kin::ordinal_category("it", 9) == Other);
    assert(kin::ordinal_category("sv", 22) == One && kin::ordinal_category("sv", 12) == Other);
    assert(kin::ordinal_category("de", 1) == Other && kin::ordinal_categories("de").size() == 1);
    assert(kin::ordinal_categories("en").size() == 4);

    const std::string_view place = "{n, selectordinal, one {#st} two {#nd} few {#rd} other {#th}}";
    assert(kin::format_message(place, "en", {{"n", 1}}) == "1st");
    assert(kin::format_message(place, "en", {{"n", 22}}) == "22nd");
    assert(kin::format_message(place, "en", {{"n", 13}}) == "13th");
    assert(kin::format_message(place, "en", {{"n", 1003}}) == "1,003rd");
    assert(kin::format_message("{n, selectordinal, one {#er} other {#e}}", "fr", {{"n", 1}}) == "1er");

    kin::MessageShape shape;
    std::string error;
    assert(kin::inspect_message(place, shape, error) && shape.arguments[0].kind == "selectordinal");
    assert(!kin::inspect_message("{n, selectordinal, first {x} other {y}}", shape, error));

    // Validation asks for the ordinal forms the language uses.
    kin::Localization l10n;
    kin::LanguageFile en{.locale = "en", .name = "English"};
    en.strings.emplace("place", std::string{place});
    en.strings.emplace("short", "{n, selectordinal, one {#st} other {#th}}");
    l10n.add("base", std::move(en));
    const auto issues = l10n.validate();
    assert(issues.size() == 1 && issues[0].key == "short" && issues[0].message.find("no two, few") != std::string::npos);
}

void test_numbers() {
    assert(kin::format_number("en", 12500.5) == "12,500.5");
    assert(kin::format_number("en", 999) == "999");
    assert(kin::format_number("en", 1000) == "1,000");
    assert(kin::format_number("en", -1234567) == "-1,234,567");
    assert(kin::format_number("en", 1.23456) == "1.235");
    assert(kin::format_number("en", 2.0) == "2");
    assert(kin::format_number("en", -0.0001) == "0");
    assert(kin::format_number("de", 12500.5) == "12.500,5");
    assert(kin::format_number("fr", 12500.5) == "12" + std::string{nbsp} + "500,5");
    assert(kin::format_number("es", 1234) == "1234"); // Spanish groups from five digits
    assert(kin::format_number("es", 12345) == "12.345");
    assert(kin::format_number("ja", 1234) == "1,234");
}

void test_messages() {
    const auto f = [](std::string_view pattern, std::initializer_list<kin::MessageArg> args, std::string_view locale = "en") {
        return kin::format_message(pattern, locale, args);
    };
    assert(f("Play", {}) == "Play");
    assert(f("{gold} gold", {{"gold", 1250}}) == "1,250 gold");
    assert(f("Hello, {name}!", {{"name", "Ana"}}) == "Hello, Ana!");
    assert(f("{gold} gold", {}) == "{gold} gold"); // a value not given shows as written
    assert(f("{ gold , number }", {{"gold", 3}}) == "3");

    const std::string_view cards = "{n, plural, =0 {no cards} one {# card} other {# cards}}";
    assert(f(cards, {{"n", 0}}) == "no cards");
    assert(f(cards, {{"n", 1}}) == "1 card");
    assert(f(cards, {{"n", 1200}}) == "1,200 cards");

    const std::string_view ru = "{n, plural, one {# карта} few {# карты} many {# карт} other {# карты}}";
    assert(f(ru, {{"n", 3}}, "ru") == "3 карты");
    assert(f(ru, {{"n", 11}}, "ru") == "11 карт");
    assert(f(ru, {{"n", 21}}, "ru") == "21 карта");

    // Nesting, and # meaning the nearest plural's number.
    const std::string_view turn =
        "{who, select, her {{n, plural, one {she draws # card} other {she draws # cards}}} other {{n, plural, one {they draw #} other {they draw #}}}}";
    assert(f(turn, {{"who", "her"}, {"n", 2}}) == "she draws 2 cards");
    assert(f(turn, {{"who", "them"}, {"n", 1}}) == "they draw 1");
    assert(f(turn, {{"n", 1}}) == turn); // no choice given: the select shows as written

    // Quoting: apostrophes before syntax start literal text.
    assert(f("l'or et l'argent", {}) == "l'or et l'argent");
    assert(f("it''s", {}) == "it's");
    assert(f("'{name}' is {name}", {{"name", "Ana"}}) == "{name} is Ana");
    assert(f("{n, plural, other {'#' is #}}", {{"n", 4}}) == "# is 4");
    assert(f("# outside", {}) == "# outside");
    assert(f("can''t '{'", {}) == "can't {");

    // Malformed patterns come back as written.
    assert(f("{oops", {}) == "{oops");
    assert(f("oops}", {}) == "oops}");
    assert(f("{n, plural, one {x}}", {{"n", 1}}) == "{n, plural, one {x}}");
    assert(f("{n, sideways}", {{"n", 1}}) == "{n, sideways}");

    // Literal transforms leave arguments alone.
    kin::MessageFormatOptions upper;
    upper.transform_literal = [](std::string_view text, std::string& out) {
        for (char c : text) out += c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c;
    };
    assert(kin::format_message("hi {name}", "en", {{"name", "ana"}}, upper) == "HI ana");

    kin::MessageShape shape;
    std::string error;
    assert(kin::inspect_message(cards, shape, error));
    assert(shape.arguments.size() == 1 && shape.arguments[0].name == "n" && shape.arguments[0].kind == "plural");
    assert((shape.arguments[0].branches == std::vector<std::string>{"=0", "one", "other"}));
    assert(kin::inspect_message(turn, shape, error) && shape.arguments.size() == 2);
    assert(!kin::inspect_message("{n, plural, one {x}}", shape, error) && error.find("other") != std::string::npos);
    assert(!kin::inspect_message("{n, plural, single {x} other {y}}", shape, error));
    assert(!kin::inspect_message("{a", shape, error));
}

void test_locale_tags() {
    assert(kin::normalize_locale("fr_ca") == "fr-CA");
    assert(kin::normalize_locale("fr_CA.UTF-8") == "fr-CA");
    assert(kin::normalize_locale("ZH-hans-cn") == "zh-Hans-CN");
    assert(kin::normalize_locale("es-419") == "es-419");
    assert(kin::normalize_locale("") == "");
    assert(kin::locale_language("pt-BR") == "pt");
    assert((kin::locale_fallbacks("zh-Hant-TW") == std::vector<std::string>{"zh-Hant-TW", "zh-Hant", "zh"}));

    assert(kin::locale_direction("ar") == kin::TextDirection::RightToLeft);
    assert(kin::locale_direction("he-IL") == kin::TextDirection::RightToLeft);
    assert(kin::locale_direction("fa") == kin::TextDirection::RightToLeft);
    assert(kin::locale_direction("en") == kin::TextDirection::LeftToRight);
    assert(kin::locale_direction("pa-Arab") == kin::TextDirection::RightToLeft);
    assert(kin::locale_direction("uz-Latn") == kin::TextDirection::LeftToRight);

    const std::vector<std::string> available{"en", "fr", "pt-BR", "zh-Hant"};
    const auto match = [&](std::initializer_list<std::string> preferred) {
        const std::vector<std::string> p{preferred};
        return kin::match_locale(p, available);
    };
    assert(match({"fr-CA"}) == "fr");
    assert(match({"pt-PT"}) == "pt-BR"); // another region of the language
    assert(match({"de", "fr"}) == "fr"); // the first preference available
    assert(match({"zh-Hant-TW"}) == "zh-Hant");
    assert(match({"de"}).empty());
    assert(match({"EN_us"}) == "en");
}

void test_language_files() {
    kin::LanguageFile file;
    std::vector<std::string> errors;
    const bool ok = kin::parse_language_file(R"({
        "$comment": "for tools",
        "locale": "fr_fr",
        "name": "Français",
        "strings": {
            "menu": { "play": "Jouer", "quit": "Quitter" },
            "shop.gold": "{gold} pièces d'or"
        }
    })", file, errors);
    assert(ok && errors.empty());
    assert(file.locale == "fr-FR" && file.name == "Français");
    assert(!file.direction); // the tag's
    assert(file.strings.size() == 3 && file.strings.at("menu.play") == "Jouer");

    // A failed parse leaves the output alone.
    assert(!kin::parse_language_file(R"({"locale": "fr", "strings": {"a.b": "x", "a": {"b": "y"}, "n": 3}})", file, errors));
    assert(file.locale == "fr-FR");
    assert(std::ranges::any_of(errors, [](const std::string& e) { return e.find("given twice") != std::string::npos; }));
    assert(std::ranges::any_of(errors, [](const std::string& e) { return e.find("'n' is not a string") != std::string::npos; }));
    errors.clear();
    assert(!kin::parse_language_file(R"({"strings": {}})", file, errors));
    assert(!kin::parse_language_file(R"({"locale": "fr", "strings": {}, "extra": 1})", file, errors));
    assert(!kin::parse_language_file(R"({"locale": "ar", "direction": "up", "strings": {}})", file, errors));
    assert(!kin::parse_language_file("[1, 2", file, errors));

    errors.clear();
    assert(kin::parse_language_file(R"({"locale": "ar", "direction": "rtl", "strings": {}})", file, errors));
    assert(file.direction == kin::TextDirection::RightToLeft);

    // Writing then reading gives the file back.
    kin::LanguageFile again;
    file.strings["b.x"] = "line\nbreak \"quoted\"";
    file.strings["a"] = "first";
    const std::string text = kin::write_language_file(file);
    assert(text.find("\"a\"") < text.find("\"b.x\""));
    assert(kin::parse_language_file(text, again, errors));
    assert(again.locale == "ar" && again.strings == file.strings && again.direction == file.direction);
}

void test_csv() {
    std::vector<kin::LanguageFile> files;
    std::vector<std::string> errors;
    const std::string csv = "\xEF\xBB\xBFkey,en,fr,comment\r\n"
                            "@name,English,Français,\r\n"
                            "# a comment row,,,\r\n"
                            "menu.play,Play,Jouer,the first button\r\n"
                            "shop.gold,\"{gold} gold, at most\",\"{gold} pièces d'or, au plus\",\r\n"
                            "story.intro,\"Line one\r\nLine \"\"two\"\"\",,untranslated in French\r\n"
                            "\r\n"
                            "last,Last,Dernier";
    assert(kin::parse_language_csv(csv, files, errors));
    assert(errors.empty() && files.size() == 2);
    const kin::LanguageFile& en = files[0];
    const kin::LanguageFile& fr = files[1];
    assert(en.locale == "en" && en.name == "English" && fr.name == "Français");
    assert(en.strings.size() == 4 && fr.strings.size() == 3);
    assert(en.strings.at("shop.gold") == "{gold} gold, at most");
    assert(en.strings.at("story.intro") == "Line one\nLine \"two\"");
    assert(!fr.strings.contains("story.intro"));
    assert(fr.strings.at("last") == "Dernier");

    assert(!kin::parse_language_csv("en,fr\nPlay,Jouer\n", files, errors)); // no key column
    assert(!kin::parse_language_csv("key,en\nx,\"open\n", files, errors));   // unclosed quote
    assert(!kin::parse_language_csv("key,en\nx,1\nx,2\n", files, errors));   // a key twice
    assert(!kin::parse_language_csv("key,en,en\n", files, errors));
    assert(!kin::parse_language_csv("key,en\n@colour,red\n", files, errors));
    assert(files.size() == 2); // failures leave the output alone
}

kin::LanguageFile language(std::string locale, std::initializer_list<std::pair<const std::string, std::string>> strings) {
    kin::LanguageFile file;
    file.locale = std::move(locale);
    file.name = file.locale;
    for (const auto& [key, value] : strings) {
        file.strings.emplace(key, value);
    }
    return file;
}

void test_lookups() {
    kin::Localization l10n;
    l10n.add("base", language("en", {{"menu.play", "Play"},
                                     {"menu.quit", "Quit"},
                                     {"shop.gold", "{gold} gold"},
                                     {"quote", "it''s"},
                                     {"cards", "{n, plural, one {# card} other {# cards}}"}}));
    l10n.add("fr", language("fr", {{"menu.play", "Jouer"},
                                   {"shop.gold", "{gold} pièces d'or"},
                                   {"cards", "{n, plural, one {# carte} other {# cartes}}"}}));
    l10n.add("fr-ca", language("fr-CA", {{"menu.play", "Jouer!"}}));

    assert(l10n.locale() == "en" && l10n.text("menu.play") == "Play");
    assert(l10n.text("quote") == "it's");
    const kin::u64 before = l10n.generation();

    assert(l10n.set_locale("fr-CA") == "fr-CA");
    assert(l10n.generation() > before);
    assert(l10n.text("menu.play") == "Jouer!");                     // fr-CA
    assert(l10n.tr("shop.gold", {{"gold", 1250}}) == "1" + std::string{nbsp} + "250 pièces d'or"); // fr
    assert(l10n.text("menu.quit") == "Quit");                       // the base
    assert(l10n.tr("cards", {{"n", 0}}) == "0 carte");               // French: 0 is singular

    assert(l10n.set_locale("fr-BE") == "fr");
    assert(l10n.set_locale("de") == "en");
    const std::vector<std::string> preferred{"de-DE", "fr-FR", "en"};
    assert(l10n.set_locale(preferred) == "fr");
    assert(l10n.direction() == kin::TextDirection::LeftToRight);

    // A view stays valid, and the same, until the next change.
    const std::string_view play = l10n.text("menu.play");
    assert(play.data() == l10n.text("menu.play").data());

    // Missing keys show as the key, and are listed.
    assert(l10n.text("menu.options") == "menu.options");
    assert(l10n.tr("hud.score", {{"n", 1}}) == "hud.score");
    assert((l10n.missing_keys() == std::vector<std::string>{"hud.score", "menu.options"}));
    l10n.clear_missing_keys();
    assert(l10n.missing_keys().empty());

    // A source loaded again replaces what it gave.
    l10n.add("fr", language("fr", {{"menu.play", "Lancer"}}));
    assert(l10n.text("menu.play") == "Lancer");
    assert(l10n.text("shop.gold") == "{gold} gold");
    l10n.remove_source("fr");
    assert(!l10n.has_language("fr") && l10n.locale() == "fr-CA"); // the nearest French left
    // ...and the asked-for locale comes back when its language does.
    l10n.add("fr", language("fr", {{"menu.play", "Jouer"}}));
    assert(l10n.locale() == "fr" && l10n.text("menu.play") == "Jouer");

    const auto infos = l10n.languages();
    assert(infos.size() == 3 && infos[0].locale == "en" && infos[2].locale == "fr-CA");

    // Direction: the tag's, unless a file says otherwise.
    l10n.add("ar", language("ar", {{"menu.play", "العب"}}));
    l10n.set_locale("ar");
    assert(l10n.direction() == kin::TextDirection::RightToLeft);
    kin::LanguageFile odd = language("ar", {});
    odd.direction = kin::TextDirection::LeftToRight;
    l10n.add("ar-ltr", std::move(odd));
    assert(l10n.direction() == kin::TextDirection::LeftToRight);
    l10n.remove_source("ar-ltr");
    l10n.remove_source("ar");
    l10n.set_locale("fr");

    // The active localization behind kin::tr.
    assert(kin::tr("menu.play") == "menu.play");
    kin::set_active_localization(&l10n);
    assert(kin::tr("menu.play") == "Jouer");
    assert(kin::tr("cards", {{"n", 2}}) == "2 cards");
    kin::set_active_localization(nullptr);
}

void test_pseudo_locale() {
    kin::Localization l10n;
    l10n.add("base", language("en", {{"play", "Play"}, {"gold", "{gold} gold [b]now[/b]"}}));
    l10n.set_locale(kin::Localization::pseudo_locale);
    assert(l10n.pseudo() && l10n.formatting_locale() == "en");
    const std::string play{l10n.text("play")};
    assert(play == "[P\xC4\xBA\xC3\xA5\xC3\xBD ~]"); // "[Pĺåý ~]": accented, a third longer, bracketed
    const std::string gold = l10n.tr("gold", {{"gold", 12}});
    assert(gold.find("12 ") != std::string::npos);         // numbers untouched
    assert(gold.find("[b]") != std::string::npos);         // markup untouched
    assert(gold.find("gold") == std::string::npos);        // letters accented

    l10n.set_pseudo_options({.expansion = 0.0f, .brackets = false});
    assert(l10n.text("play") == "P\xC4\xBA\xC3\xA5\xC3\xBD");
}

void test_validate() {
    using Severity = kin::LocalizationIssue::Severity;
    kin::Localization l10n;
    l10n.add("base", language("en", {{"play", "Play"},
                                     {"gold", "{gold} gold"},
                                     {"cards", "{n, plural, one {# card} other {# cards}}"},
                                     {"who", "{who, select, her {her} other {their}}"},
                                     {"broken", "{oops"}}));
    l10n.add("pl", language("pl", {{"gold", "{coins} złota"},
                                   {"cards", "{n, plural, one {# karta} other {# kart}}"},
                                   {"who", "{who, plural, one {x} few {y} many {z} other {w}}"},
                                   {"extra", "Dodatek"}}));
    l10n.add("en-GB", language("en-GB", {{"play", "Play!"}}));
    const auto issues = l10n.validate();
    assert(has_issue(issues, "en", "broken", "unclosed", Severity::Error));
    assert(has_issue(issues, "pl", "play", "not translated", Severity::Warning));
    assert(has_issue(issues, "pl", "gold", "'{coins}' is not in the base text", Severity::Warning));
    assert(has_issue(issues, "pl", "gold", "does not use '{gold}'", Severity::Warning));
    assert(has_issue(issues, "pl", "cards", "has no few, many", Severity::Warning));
    assert(has_issue(issues, "pl", "who", "a number here but a choice", Severity::Error));
    assert(has_issue(issues, "pl", "extra", "not in the base language", Severity::Warning));
    // en-GB falls back to en: nothing it lacks is missing.
    assert(std::ranges::none_of(issues, [](const auto& i) { return i.locale == "en-GB"; }));
}

void test_files_and_reload(const fs::path& dir) {
    write(dir / "en.kinlang", R"({"locale": "en", "name": "English", "strings": {"menu": {"play": "Play"}}})");
    write(dir / "extra.csv", "key,en,de\n@name,,Deutsch\nmenu.quit,Quit,Beenden\n");
    write(dir / "notes.txt", "not a language file");

    kin::FileWatcher files;
    std::vector<std::string> errors;
    {
        kin::Localization l10n;
        assert(l10n.load_directory(dir, errors, &files) && errors.empty());
        assert(files.size() == 2);
        assert(l10n.has_language("de") && l10n.languages()[0].name == "Deutsch");
        l10n.set_locale("de");
        assert(l10n.text("menu.quit") == "Beenden" && l10n.text("menu.play") == "Play");

        const kin::u64 before = l10n.generation();
        write(dir / "extra.csv", "key,en,de\nmenu.quit,Quit,Verlassen\n");
        files.poll_now();
        files.poll_now();
        assert(l10n.generation() > before);
        assert(l10n.text("menu.quit") == "Verlassen");

        // A broken edit keeps the last good strings.
        write(dir / "extra.csv", "key,en,de\nmenu.quit,\"Quit\n");
        files.poll_now();
        files.poll_now();
        assert(l10n.text("menu.quit") == "Verlassen");
    }
    assert(files.empty()); // the localization stopped watching when it went

    kin::Localization l10n;
    assert(!l10n.load_file(dir / "missing.kinlang", errors) && !errors.empty());
    assert(!l10n.load_directory(dir / "no-such-dir", errors));
}

} // namespace

int main() {
    std::vector<kin::LogEvent> log_events;
    kin::set_logger_config({
        .min_level = kin::LogLevel::Debug,
        .format = kin::LogFormat::Text,
        .sdl_sink = false,
        .memory_events = &log_events,
    });
    const fs::path dir = fs::temp_directory_path() / "kin-l10n-tests";
    fs::remove_all(dir);
    fs::create_directories(dir);

    test_plural_rules();
    test_ordinals();
    test_numbers();
    test_messages();
    test_locale_tags();
    test_language_files();
    test_csv();
    test_lookups();
    test_pseudo_locale();
    test_validate();
    test_files_and_reload(dir);

    fs::remove_all(dir);
    return 0;
}
