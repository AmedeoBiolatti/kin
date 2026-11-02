#include <kin/assets/file_watcher.hpp>
#include <kin/core/script_debugger.hpp>
#include <kin/l10n/localization.hpp>
#include <kin/platform/log.hpp>
#include <kin/scripting/lua_script.hpp>

#include <cassert>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <string>
#include <stdexcept>
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

void test_native_errors_name_the_line() {
    kin::LuaScript script{{.setup = [](sol::state& lua) {
        lua["boom"] = [] { throw std::runtime_error("native boom"); };
    }}};
    assert(script.load_string("function caught()\n local ok, message=pcall(boom)\n return not ok and message:find('rules:2:',1,true) ~= nil\nend\nfunction direct()\n boom()\nend\n", "rules"));
    assert(script.call_for<bool>("caught") == true);
    assert(!script.call("direct"));
    assert(contains(script.error(), "rules:6:") && contains(script.error(), "native boom"));
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

const kin::ScriptValue* value_named(const std::vector<kin::ScriptValue>& values, std::string_view name) {
    for (const kin::ScriptValue& value : values) {
        if (value.name == name) {
            return &value;
        }
    }
    return nullptr;
}

// Breakpoints, a pause and steps stop a script inside the waiter, which sees
// its frames and their values; with no waiter nothing stops.
void test_debugger_stops_and_steps(const fs::path& dir) {
    const fs::path content = dir / "debugged";
    write(content / "scripts/walker.lua",
          "local speed = 3\n"                  // 1
          "local function double(x)\n"         // 2
          "    local twice = x * 2\n"          // 3
          "    return twice\n"                 // 4
          "end\n"                              // 5
          "function step(self, dt)\n"          // 6
          "    local moved = double(dt)\n"     // 7
          "    self.x = self.x + moved\n"      // 8
          "    return self.x * speed\n"        // 9
          "end\n");                            // 10
    kin::LuaScript script{{.module_root = content}};
    assert(script.load_file(content / "scripts/walker.lua"));
    sol::table self = script.lua().create_table_with("x", 1, "name", "walker");
    self["list"] = script.lua().create_table_with(1, 10, 2, 20, 3, 30);

    kin::ScriptDebugger& debugger = kin::script_debugger();
    std::vector<kin::ScriptStop> stops;
    std::deque<kin::ScriptResume> then;
    const auto set_breakpoints = [&](std::set<kin::i32> lines) {
        debugger.set_breakpoints({{"scripts/walker.lua", std::move(lines)}});
    };

    // No waiter: breakpoints are kept, but nothing stops.
    set_breakpoints({7});
    assert(!debugger.armed());
    assert(script.call("step", self, 0.5));
    assert(stops.empty());

    debugger.set_waiter([&](kin::ScriptDebugger& at) {
        assert(at.stop() && at.stop()->serial > 0);
        stops.push_back(*at.stop());
        const kin::ScriptResume how = then.empty() ? kin::ScriptResume::Continue : then.front();
        if (!then.empty()) {
            then.pop_front();
        }
        return how;
    });
    assert(debugger.armed() && !debugger.seeking());

    // A breakpoint, then into double(), over its next line, and back out.
    then = {kin::ScriptResume::Into, kin::ScriptResume::Over, kin::ScriptResume::Over};
    assert(script.call("step", self, 0.5));
    assert(stops.size() == 4);
    const kin::ScriptStop& hit = stops[0];
    assert(hit.reason == "breakpoint" && hit.frames.size() >= 1);
    assert(hit.frames[0].function == "step" && hit.frames[0].file == "scripts/walker.lua" && hit.frames[0].line == 7);
    const kin::ScriptValue* dt = value_named(hit.frames[0].locals, "dt");
    assert(dt && dt->kind == "number" && dt->value == "0.5");
    const kin::ScriptValue* seen = value_named(hit.frames[0].locals, "self");
    assert(seen && seen->kind == "table" && seen->value == "table (3)");
    assert(seen->fields.size() == 3 && seen->fields[0].name == "list" && seen->fields[1].value == "\"walker\"" &&
           seen->fields[2].name == "x" && seen->fields[2].value == "2"); // 1 + double(0.5), from the call above
    assert(seen->fields[0].fields.size() == 3 && seen->fields[0].fields[1].name == "[2]" && seen->fields[0].fields[1].value == "20");
    const kin::ScriptValue* helper = value_named(hit.frames[0].upvalues, "double");
    assert(helper && helper->kind == "function" && contains(helper->value, "walker.lua:2"));
    assert(value_named(hit.frames[0].upvalues, "speed"));
    assert(stops[1].reason == "step" && stops[1].frames[0].function == "double" && stops[1].frames[0].line == 3);
    assert(stops[1].frames.size() >= 2 && stops[1].frames[1].function == "step" && stops[1].frames[1].line == 7);
    assert(value_named(stops[1].frames[0].locals, "x")->value == "0.5");
    assert(stops[2].frames[0].function == "double" && stops[2].frames[0].line == 4);
    assert(value_named(stops[2].frames[0].locals, "twice")->value == "1"); // 1.0 shown as Lua prints it: %.9g
    assert(stops[3].frames[0].function == "step" && (stops[3].frames[0].line == 7 || stops[3].frames[0].line == 8));
    assert(stops[0].serial < stops[3].serial);
    assert(!debugger.stop() && !debugger.seeking()); // gone on

    // Out of double(), back in step().
    stops.clear();
    set_breakpoints({3});
    then = {kin::ScriptResume::Out};
    assert(script.call("step", self, 0.5));
    assert(stops.size() == 2 && stops[0].frames[0].function == "double");
    assert(stops[1].reason == "step" && stops[1].frames[0].function == "step");

    // Over from a function's last line: the call ends, and the step ends at
    // the next line run, in the host's next call.
    stops.clear();
    set_breakpoints({9});
    then = {kin::ScriptResume::Over};
    assert(script.call("step", self, 0.5));
    assert(stops.size() == 1 && stops[0].frames[0].line == 9 && debugger.seeking());
    assert(script.call("step", self, 0.5));
    assert(stops.size() == 3 && stops[1].reason == "step" && stops[1].frames[0].line == 7);
    assert(stops[2].reason == "breakpoint" && stops[2].frames[0].line == 9);

    // A pause: the next line run, wherever it is.
    stops.clear();
    set_breakpoints({});
    assert(!debugger.armed());
    debugger.pause();
    assert(debugger.armed() && debugger.seeking());
    assert(script.call("step", self, 0.5));
    assert(stops.size() == 1 && stops[0].reason == "pause" && stops[0].frames[0].line == 7);

    // The instruction budget still stops a loop while lines are watched.
    set_breakpoints({100});
    kin::LuaScript runaway{{.instruction_limit = 10'000}};
    assert(runaway.load_string("function spin() while true do end end"));
    assert(!runaway.call("spin") && contains(runaway.error(), "instruction limit"));

    // No waiter again: nothing stops, nothing is sought.
    debugger.set_waiter({});
    set_breakpoints({7});
    stops.clear();
    assert(script.call("step", self, 0.5) && stops.empty() && !debugger.armed());
    debugger.set_breakpoints({});
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
    test_native_errors_name_the_line();
    test_sandbox_is_deterministic();
    test_print_goes_to_the_log(log_events);
    test_instruction_limit_stops_runaway_scripts();
    test_a_failed_load_keeps_the_last_good_script();
    test_modules_and_hot_reload(dir);
    test_translations();
    test_debugger_stops_and_steps(dir);

    fs::remove_all(dir);
    return 0;
}
