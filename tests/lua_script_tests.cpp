#include <kin/assets/file_watcher.hpp>
#include <kin/l10n/localization.hpp>
#include <kin/platform/log.hpp>
#include <kin/scripting/lua_script.hpp>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

void write(const fs::path& path, const std::string& text) {
    static fs::file_time_type next = fs::file_time_type::clock::now();
    fs::create_directories(path.parent_path());
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << text;
    }
    next += std::chrono::seconds(2);
    fs::last_write_time(path, next);
}

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

// The host binds what a script may do; the script answers events.
void test_host_api_and_calls() {
    int total = 0;
    kin::LuaScript script{{.setup = [&](sol::state& lua) {
        sol::table host = lua.create_named_table("host");
        host["add"] = [&](int n) { total += n; };
    }}};
    assert(script.load_string(R"(
        function on_event(n) host.add(n * 2) end
        function score(a, b) return a * b end
        function name() return "x" end
    )", "rules"));
    assert(script.loaded() && script.has("on_event") && !script.has("on_missing"));
    assert(script.call("on_event", 5) && total == 10);
    assert(!script.call("on_missing", 1) && script.error().empty()); // not defined: nothing to report
    assert(script.call_for<int>("score", 3, 4) == 12);
    assert(!script.call_for<int>("name"));
    assert(contains(script.error(), "name"));
}

void test_errors_name_the_line() {
    std::vector<std::string> reported;
    kin::LuaScript script{{.on_error = [&](std::string_view message) { reported.emplace_back(message); }}};
    assert(script.load_string("function ok() return 1 end\nfunction bad()\n  error('boom')\nend\n", "rules"));
    assert(!script.call("bad"));
    assert(contains(script.error(), "rules:3:") && contains(script.error(), "boom"));
    assert(reported.size() == 1 && reported[0] == script.error());
    assert(script.call_for<int>("ok") == 1); // a failed call leaves the script working
}

void test_sandbox_is_deterministic() {
    kin::LuaScript script;
    assert(script.load_string(R"(
        function probe()
            return os == nil and io == nil and load == nil and dofile == nil and loadfile == nil
               and require == nil and debug == nil and package == nil and collectgarbage == nil
               and math.random == nil and math.randomseed == nil
               and math.floor(2.5) == 2 and string.upper("a") == "A" and utf8.char(72) == "H"
        end
    )"));
    assert(script.call_for<bool>("probe") == true);
}

void test_print_goes_to_the_log(const std::vector<kin::LogEvent>& events) {
    kin::LuaScript script;
    assert(script.load_string("print('hello', 3, true)"));
    bool found = false;
    for (const kin::LogEvent& event : events) {
        if (event.category == "script" && event.message == "print") {
            for (const auto& field : event.fields) {
                found = found || (field.name == "text" && field.value == "hello\t3\ttrue");
            }
        }
    }
    assert(found);
}

void test_instruction_limit_stops_runaway_scripts() {
    kin::LuaScript script{{.instruction_limit = 100'000}};
    assert(script.load_string("function spin() while true do end end\nfunction one() return 1 end"));
    assert(!script.call("spin"));
    assert(contains(script.error(), "instruction limit"));
    assert(script.call_for<int>("one") == 1); // the next call gets a fresh budget
    // A script that never finishes loading is refused, and the old one stays.
    assert(!script.load_string("while true do end"));
    assert(script.call_for<int>("one") == 1);
}

void test_a_failed_load_keeps_the_last_good_script() {
    kin::LuaScript script;
    assert(script.load_string("function f() return 1 end"));
    assert(!script.load_string("function f( return 2"));   // does not parse
    assert(script.call_for<int>("f") == 1);
    assert(!script.load_string("function f() return 3 end\nerror('half way')")); // fails while running
    assert(script.call_for<int>("f") == 1);
    assert(script.load_string("function f() return 4 end"));
    assert(script.call_for<int>("f") == 4 && script.error().empty());
}

void test_modules_and_hot_reload(const fs::path& dir) {
    write(dir / "lib/util.lua", "return { scale = function(x) return 2 * x end }\n");
    write(dir / "main.lua", "local util = require('lib.util')\nfunction go(x) return util.scale(x) end\n");
    write(dir / "outside.lua", "return 1\n");
    kin::LuaScript script{{.module_root = dir / "lib/.."}};
    assert(script.load_file(dir / "main.lua"));
    assert(script.call_for<int>("go", 21) == 42);
    assert(script.dependencies().size() == 2);

    // Module names are plain dotted names under the root.
    kin::LuaScript strict{{.module_root = dir / "lib"}};
    assert(!strict.load_string("require('../outside')"));
    assert(!strict.load_string("require('missing')"));
    assert(contains(strict.error(), "not found"));

    kin::FileWatcher files;
    script.watch(files);
    write(dir / "lib/util.lua", "return { scale = function(x) return 3 * x end }\n"); // a module changes
    files.poll_now();
    files.poll_now();
    assert(script.call_for<int>("go", 10) == 30);

    write(dir / "lib/util.lua", "return { scale = function(x) return 3 * x end\n"); // a broken edit
    files.poll_now();
    files.poll_now();
    assert(!script.error().empty() && script.call_for<int>("go", 10) == 30);

    write(dir / "main.lua", "local util = require('lib.util')\nfunction go(x) return -x end\n");
    write(dir / "lib/util.lua", "return {}\n"); // fixed
    files.poll_now();
    files.poll_now();
    assert(script.call_for<int>("go", 10) == -10 && script.error().empty());
}

} // namespace

void test_translations() {
    kin::LuaScript script;
    assert(script.load_string(R"(
        function play() return tr("menu.play") end
        function gold(n) return tr("hud.gold", {gold = n, who = "Ana", rich = n > 100}) end
        function info() return l10n.locale() .. "|" .. l10n.direction() .. "|" .. tostring(l10n.has("menu.play")) end
        function own(n) return l10n.format("{n, plural, one {# left} other {# left}}", {n = n}) end
        function can_switch() return l10n.set_locale == nil end
        function names() local out = "" for _, l in ipairs(l10n.languages()) do out = out .. l.name .. ";" end return out end
    )"));
    assert(script.call_for<std::string>("play") == "menu.play"); // no localization: the key
    assert(script.call_for<std::string>("info") == "|ltr|false");

    kin::Localization l10n;
    kin::LanguageFile en{.locale = "en", .name = "English"};
    en.strings.emplace("menu.play", "Play");
    en.strings.emplace("hud.gold", "{who}: {gold, plural, one {# coin} other {# coins}} {rich, select, true {(rich)} other {}}");
    kin::LanguageFile ar{.locale = "ar", .name = "العربية"};
    ar.strings.emplace("menu.play", "العب");
    l10n.add("test", std::vector<kin::LanguageFile>{std::move(en), std::move(ar)});
    kin::set_active_localization(&l10n);
    assert(script.call_for<std::string>("play") == "Play");
    assert(script.call_for<std::string>("gold", 1250) == "Ana: 1,250 coins (rich)");
    assert(script.call_for<std::string>("gold", 1) == "Ana: 1 coin ");
    assert(script.call_for<std::string>("own", 3000) == "3,000 left");
    assert(script.call_for<bool>("can_switch") == true); // a sandbox reads, it does not switch
    assert(script.call_for<std::string>("names") == "العربية;English;");
    l10n.set_locale("ar");
    assert(script.call_for<std::string>("info") == "ar|rtl|true");
    kin::set_active_localization(nullptr);
}

int main() {
    std::vector<kin::LogEvent> log_events;
    kin::set_logger_config({
        .min_level = kin::LogLevel::Debug,
        .format = kin::LogFormat::Text,
        .sdl_sink = false,
        .memory_events = &log_events,
    });
    const fs::path dir = fs::temp_directory_path() / "kin-lua-script-tests";
    fs::remove_all(dir);

    test_host_api_and_calls();
    test_errors_name_the_line();
    test_sandbox_is_deterministic();
    test_print_goes_to_the_log(log_events);
    test_instruction_limit_stops_runaway_scripts();
    test_a_failed_load_keeps_the_last_good_script();
    test_modules_and_hot_reload(dir);
    test_translations();

    fs::remove_all(dir);
    return 0;
}
