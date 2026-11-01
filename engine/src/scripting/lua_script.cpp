#include <kin/scripting/lua_script.hpp>

#include "lua_l10n.hpp"
#include "lua_paths.hpp"

#include <kin/assets/file_watcher.hpp>
#include <kin/core/script_debugger.hpp>
#include <kin/platform/log.hpp>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <utility>

namespace kin {

namespace {

// The budget of the load or call running on this thread (null: no limit). The
// count hook takes 1,000 instructions from it at a time and stops the script
// with an error when it runs out.
thread_local u64* t_budget = nullptr;
constexpr int hook_interval = 1000;
// Which of the host's calls into a script runs on this thread (each its own
// number): how the debugger knows a function stepped in has returned.
thread_local u64 t_call = 0;
std::atomic<u64> g_calls{0};

void count_hook(lua_State* state) {
    if (!t_budget) {
        return;
    }
    if (*t_budget <= static_cast<u64>(hook_interval)) {
        *t_budget = 0;
        luaL_error(state, "instruction limit reached (a loop that never ends?)");
        return;
    }
    *t_budget -= static_cast<u64>(hook_interval);
}

// --- What a stop shows -------------------------------------------------------

constexpr std::size_t shown_text = 120;   // bytes of a string shown
constexpr std::size_t shown_fields = 64;  // of a table
constexpr i32 shown_depth = 2;            // tables in tables
constexpr i32 shown_frames = 32;

std::string cut_text(std::string_view text) {
    if (text.size() <= shown_text) {
        return std::string(text);
    }
    std::size_t end = shown_text;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
        --end; // not inside a character
    }
    return std::string(text.substr(0, end)) + "...";
}

std::string quoted(std::string_view text) {
    std::string out = "\"";
    for (const char c : cut_text(text)) {
        switch (c) {
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        default: out += c;
        }
    }
    return out + "\"";
}

std::string number_text(lua_State* state, int index) {
    char text[40];
    if (lua_isinteger(state, index)) {
        std::snprintf(text, sizeof(text), "%lld", static_cast<long long>(lua_tointeger(state, index)));
    } else {
        std::snprintf(text, sizeof(text), "%.9g", static_cast<double>(lua_tonumber(state, index)));
    }
    return text;
}

int to_text(lua_State* state) {
    luaL_tolstring(state, 1, nullptr);
    return 1;
}

// A userdata as its __tostring tells it (called protected: a binding's may
// fail), else by its type's name.
std::string userdata_text(lua_State* state, int index) {
    index = lua_absindex(state, index);
    if (luaL_getmetafield(state, index, "__tostring") != LUA_TNIL) {
        lua_pop(state, 1);
        lua_pushcfunction(state, to_text);
        lua_pushvalue(state, index);
        if (lua_pcall(state, 1, 1, 0) == LUA_OK) {
            size_t size = 0;
            const char* text = lua_tolstring(state, -1, &size);
            std::string out = text ? cut_text({text, size}) : std::string("userdata");
            lua_pop(state, 1);
            return out;
        }
        lua_pop(state, 1);
    }
    if (luaL_getmetafield(state, index, "__name") == LUA_TSTRING) {
        // A bound type's name, without its namespaces ("sol.kin::ScriptEntity" -> "ScriptEntity").
        std::string out = lua_tostring(state, -1);
        lua_pop(state, 1);
        if (const std::size_t colon = out.rfind("::"); colon != std::string::npos) {
            out.erase(0, colon + 2);
        } else if (out.starts_with("sol.")) {
            out.erase(0, 4);
        }
        return out.empty() ? "userdata" : out;
    }
    return "userdata";
}

ScriptValue describe(lua_State* state, int index, std::string name, i32 depth);

// A table's fields: whole numbers in order, then the rest by name.
void describe_fields(lua_State* state, int index, i32 depth, ScriptValue& value) {
    struct Field {
        bool numbered = false;
        lua_Integer number = 0;
        std::string key;
        ScriptValue value;
    };
    std::vector<Field> fields;
    std::size_t count = 0;
    lua_pushnil(state);
    while (lua_next(state, index) != 0) {
        ++count;
        if (fields.size() < shown_fields && depth > 0) {
            Field field;
            const int key_type = lua_type(state, -2);
            if (key_type == LUA_TNUMBER && lua_isinteger(state, -2)) {
                field.numbered = true;
                field.number = lua_tointeger(state, -2);
                field.key = "[" + std::to_string(field.number) + "]";
            } else if (key_type == LUA_TSTRING) {
                size_t size = 0;
                const char* text = lua_tolstring(state, -2, &size); // a string key: no conversion, lua_next is safe
                field.key = cut_text({text, size});
            } else if (key_type == LUA_TNUMBER) {
                field.key = "[" + number_text(state, -2) + "]";
            } else {
                field.key = std::string("[") + luaL_typename(state, -2) + "]";
            }
            field.value = describe(state, -1, field.key, depth - 1);
            fields.push_back(std::move(field));
        }
        lua_pop(state, 1);
    }
    std::ranges::stable_sort(fields, [](const Field& a, const Field& b) {
        if (a.numbered != b.numbered) {
            return a.numbered;
        }
        return a.numbered ? a.number < b.number : a.key < b.key;
    });
    for (Field& field : fields) {
        value.fields.push_back(std::move(field.value));
    }
    value.more = depth > 0 && count > value.fields.size();
    value.value = count == 0 ? "table (empty)" : "table (" + std::to_string(count) + ")";
}

ScriptValue describe(lua_State* state, int index, std::string name, i32 depth) {
    index = lua_absindex(state, index);
    ScriptValue value;
    value.name = std::move(name);
    value.kind = luaL_typename(state, index);
    luaL_checkstack(state, 8, nullptr);
    switch (lua_type(state, index)) {
    case LUA_TNIL: value.value = "nil"; break;
    case LUA_TBOOLEAN: value.value = lua_toboolean(state, index) ? "true" : "false"; break;
    case LUA_TNUMBER: value.value = number_text(state, index); break;
    case LUA_TSTRING: {
        size_t size = 0;
        const char* text = lua_tolstring(state, index, &size);
        value.value = quoted({text, size});
        break;
    }
    case LUA_TTABLE: describe_fields(state, index, depth, value); break;
    case LUA_TFUNCTION: {
        lua_Debug info{};
        lua_pushvalue(state, index);
        lua_getinfo(state, ">S", &info);
        value.value = info.what && std::string_view(info.what) == "C"
                          ? "function (host)"
                          : "function (" + std::string(info.short_src) + ":" + std::to_string(info.linedefined) + ")";
        break;
    }
    case LUA_TUSERDATA:
    case LUA_TLIGHTUSERDATA: value.value = userdata_text(state, index); break;
    default: value.value = value.kind; break;
    }
    return value;
}

// The file breakpoints name a chunk by ("@scripts/a.lua" -> "scripts/a.lua",
// "=rules" -> "rules"); empty for code not from a file (stepped through, never
// stopped in).
std::string_view chunk_file(const char* source) {
    if (!source || (source[0] != '@' && source[0] != '=')) {
        return {};
    }
    return std::string_view(source + 1);
}

i32 stack_depth(lua_State* state) {
    lua_Debug level{};
    i32 depth = 0;
    while (lua_getstack(state, depth, &level)) {
        ++depth;
    }
    return depth;
}

// The global a function is, if one is ("step" for a function the host called:
// Lua names a function by its call, and a host's call gives it none).
std::string global_name(lua_State* state, lua_Debug& level) {
    std::string name;
    lua_getinfo(state, "f", &level);
    lua_rawgeti(state, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
    lua_pushnil(state);
    while (lua_next(state, -2) != 0) {
        if (lua_type(state, -2) == LUA_TSTRING && lua_rawequal(state, -1, -4)) {
            name = lua_tostring(state, -2);
            lua_pop(state, 2);
            break;
        }
        lua_pop(state, 1);
    }
    lua_pop(state, 2); // the globals, the function
    return name;
}

ScriptStop make_stop(lua_State* state, std::string_view reason) {
    ScriptStop stop;
    stop.reason = std::string(reason);
    lua_Debug level{};
    for (i32 at = 0; at < shown_frames && lua_getstack(state, at, &level); ++at) {
        lua_getinfo(state, "nSl", &level);
        ScriptFrame frame;
        const std::string_view what = level.what ? level.what : "";
        frame.file = what == "C" ? std::string{} : std::string(chunk_file(level.source));
        frame.line = std::max(0, level.currentline);
        if (what == "main") {
            frame.function = "main chunk";
        } else if (level.name) {
            frame.function = level.name;
        } else if (std::string global = global_name(state, level); !global.empty()) {
            frame.function = std::move(global);
        } else {
            frame.function = what == "C" ? "(host)" : "?";
        }
        if (what != "C") {
            for (int local = 1;; ++local) {
                const char* name = lua_getlocal(state, &level, local);
                if (!name) {
                    break;
                }
                if (name[0] != '(') { // not one of Lua's own temporaries
                    frame.locals.push_back(describe(state, -1, name, shown_depth));
                }
                lua_pop(state, 1);
            }
            lua_getinfo(state, "f", &level);
            for (int upvalue = 1;; ++upvalue) {
                const char* name = lua_getupvalue(state, -1, upvalue);
                if (!name) {
                    break;
                }
                if (std::string_view(name) != "_ENV" && name[0] != '\0') { // not the globals
                    frame.upvalues.push_back(describe(state, -1, name, shown_depth));
                }
                lua_pop(state, 1);
            }
            lua_pop(state, 1);
        }
        stop.frames.push_back(std::move(frame));
    }
    return stop;
}

// The breakpoints this thread last read, by the generation they were.
struct KnownBreakpoints {
    u64 generation = ~u64{0};
    std::shared_ptr<const ScriptDebugger::Breakpoints> lines;
};
thread_local KnownBreakpoints t_breakpoints;

void line_hook(lua_State* state, lua_Debug* at) {
    ScriptDebugger& debugger = script_debugger();
    if (!debugger.armed()) {
        return; // disarmed since this call set the hook
    }
    lua_getinfo(state, "S", at);
    const std::string_view file = chunk_file(at->source);
    if (file.empty()) {
        return;
    }
    if (!debugger.seeking()) {
        // Only a breakpoint stops it: most lines are told apart here, unlocked.
        const u64 generation = debugger.generation();
        if (t_breakpoints.generation != generation) {
            t_breakpoints = {.generation = generation, .lines = debugger.breakpoints()};
        }
        const auto found = t_breakpoints.lines->find(file);
        if (found == t_breakpoints.lines->end() || !found->second.contains(at->currentline)) {
            return;
        }
    }
    const i32 depth = stack_depth(state);
    const std::string_view reason = debugger.should_stop(file, at->currentline, t_call, depth);
    if (reason.empty()) {
        return;
    }
    debugger.stopped(make_stop(state, reason), t_call, depth);
}

void script_hook(lua_State* state, lua_Debug* at) {
    if (at->event == LUA_HOOKCOUNT) {
        count_hook(state);
    } else if (at->event == LUA_HOOKLINE) {
        line_hook(state, at);
    }
}

std::string error_text(const sol::protected_function_result& result) {
    const sol::error error = result;
    return error.what();
}

} // namespace

LuaScript::Budget::Budget(const LuaScript& script, lua_State* state)
    : _remaining(script._options.instruction_limit), _previous(t_budget), _previous_call(t_call) {
    t_budget = _remaining > 0 ? &_remaining : nullptr;
    t_call = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    // Lines are seen only while the debugger may stop at one: a script runs
    // at its full pace otherwise.
    const int mask = (_remaining > 0 ? LUA_MASKCOUNT : 0) | (script_debugger().armed() ? LUA_MASKLINE : 0);
    if (lua_gethookmask(state) != mask) {
        lua_sethook(state, mask != 0 ? script_hook : nullptr, mask, hook_interval);
    }
}

LuaScript::Budget::~Budget() {
    t_budget = _previous;
    t_call = _previous_call;
}

LuaScript::LuaScript(LuaScriptOptions options)
    : _options(std::move(options)), _dependencies(std::make_shared<std::vector<std::filesystem::path>>()) {
    _lua = make_state(_dependencies);
}

LuaScript::~LuaScript() {
    if (_files && !_files_alive.expired()) {
        for (const u32 id : _watches) {
            _files->unwatch(id);
        }
    }
}

std::unique_ptr<sol::state> LuaScript::make_state(const Dependencies& dependencies) const {
    auto lua = std::make_unique<sol::state>();
    scripting_detail::bind_script_errors(*lua);
    lua->open_libraries(sol::lib::base, sol::lib::math, sol::lib::table, sol::lib::string, sol::lib::utf8);
    // Nothing that reads the world outside the script, or makes up numbers.
    for (const char* name : {"dofile", "loadfile", "load", "require", "collectgarbage"}) {
        (*lua)[name] = sol::lua_nil;
    }
    (*lua)["math"]["random"] = sol::lua_nil;
    (*lua)["math"]["randomseed"] = sol::lua_nil;
    // Translations to read, not the language to change.
    scripting_detail::bind_l10n(*lua, false);
    (*lua)["print"] = [](sol::this_state state, sol::variadic_args args) {
        sol::state_view view{state};
        const sol::protected_function to_text = view["tostring"];
        std::string line;
        for (const sol::stack_proxy argument : args) {
            if (!line.empty()) {
                line += '\t';
            }
            const sol::protected_function_result text = to_text(argument);
            line += text.valid() ? text.get<std::string>() : std::string("?");
        }
        KIN_LOG_INFO_F("script", "print", (LogFields{{.name = "text", .value = line}}));
    };

    if (!_options.module_root.empty()) {
        // Modules load once per state, from module_root only; each becomes a
        // dependency, so watch() reloads when it changes.
        const std::filesystem::path root = scripting_detail::normalized_path(_options.module_root);
        sol::table loaded = lua->create_table();
        (*lua)["require"] = [root, loaded, dependencies](sol::this_state state, const std::string& module) mutable -> sol::object {
            sol::object cached = loaded[module];
            if (cached.valid() && cached.get_type() != sol::type::lua_nil) {
                return cached;
            }
            const std::optional<std::filesystem::path> path = scripting_detail::module_path(root, module);
            if (!path) {
                throw std::runtime_error("module '" + module + "' not found under " + root.string());
            }
            dependencies->push_back(*path);
            sol::state_view view{state};
            const std::optional<std::string> code = read_text_file(*path);
            if (!code) {
                throw std::runtime_error("module '" + module + "' could not be read");
            }
            const std::string chunk_name = "@" + path->lexically_relative(root).generic_string();
            sol::load_result chunk = view.load(*code, chunk_name);
            if (!chunk.valid()) {
                const sol::error error = chunk;
                throw std::runtime_error(error.what());
            }
            sol::protected_function run = chunk;
            const sol::protected_function_result result = run(module);
            if (!result.valid()) {
                throw std::runtime_error(error_text(result));
            }
            sol::object value = result;
            if (!value.valid() || value.get_type() == sol::type::lua_nil) {
                value = sol::make_object(state, true);
            }
            loaded[module] = value;
            return value;
        };
    }

    if (_options.setup) {
        _options.setup(*lua);
    }
    return lua;
}

bool LuaScript::load_file(const std::filesystem::path& path) {
    const std::optional<std::string> code = read_text_file(path);
    if (!code) {
        fail("could not read " + path.string());
        return false;
    }
    // Named as breakpoints name it: under module_root, from there
    // ("scripts/player.lua"); else by its file's name.
    std::string chunk = path.filename().string();
    const std::filesystem::path full = scripting_detail::normalized_path(path);
    if (!_options.module_root.empty()) {
        const std::filesystem::path relative = full.lexically_relative(scripting_detail::normalized_path(_options.module_root));
        if (!relative.empty() && *relative.begin() != "..") {
            chunk = relative.generic_string();
        }
    }
    if (!run(*code, "@" + chunk, {full})) {
        return false;
    }
    _path = path;
    rewatch();
    return true;
}

bool LuaScript::load_string(std::string_view code, std::string_view name) {
    return run(code, "=" + std::string(name), {});
}

bool LuaScript::run(std::string_view code, const std::string& chunk_name, std::vector<std::filesystem::path> dependencies) {
    // A fresh state, replacing the current one only if the script loads and runs.
    auto fresh_dependencies = std::make_shared<std::vector<std::filesystem::path>>(std::move(dependencies));
    std::unique_ptr<sol::state> fresh = make_state(fresh_dependencies);
    sol::load_result chunk = fresh->load(code, chunk_name);
    if (!chunk.valid()) {
        const sol::error error = chunk;
        fail(error.what());
        return false;
    }
    sol::protected_function script = chunk;
    {
        const Budget budget{*this, fresh->lua_state()};
        const sol::protected_function_result result = script();
        if (!result.valid()) {
            fail(error_text(result));
            return false;
        }
    }
    _dependencies = std::move(fresh_dependencies);
    _lua = std::move(fresh);
    _loaded = true;
    _error.clear();
    return true;
}

bool LuaScript::has(std::string_view function) const {
    return find(function).valid();
}

sol::protected_function LuaScript::find(std::string_view function) const {
    if (!_loaded) {
        return {};
    }
    const sol::object value = (*_lua)[std::string(function)];
    if (value.get_type() != sol::type::function) {
        return {};
    }
    return value.as<sol::protected_function>();
}

bool LuaScript::check(const sol::protected_function_result& result, std::string_view function) {
    if (result.valid()) {
        return true;
    }
    fail(std::string(function) + ": " + error_text(result));
    return false;
}

void LuaScript::fail(std::string message) {
    _error = std::move(message);
    if (_options.on_error) {
        _options.on_error(_error);
        return;
    }
    KIN_LOG_WARN_F("script", "lua script failed", (LogFields{{.name = "error", .value = _error}}));
}

void LuaScript::watch(FileWatcher& files) {
    if (_files && _files != &files && !_files_alive.expired()) {
        for (const u32 id : _watches) {
            _files->unwatch(id);
        }
        _watches.clear();
    }
    _files = &files;
    _files_alive = files.lifetime();
    rewatch();
}

void LuaScript::rewatch() {
    if (!_files || _files_alive.expired()) {
        return;
    }
    for (const u32 id : _watches) {
        _files->unwatch(id);
    }
    _watches.clear();
    if (_path.empty()) {
        return;
    }
    // The script and every module it required: a change to any reloads it all.
    // A failed reload keeps the current watches, so fixing the file reloads it.
    for (const std::filesystem::path& path : *_dependencies) {
        _watches.push_back(_files->watch(path, [this](const std::filesystem::path&) { load_file(_path); }));
    }
    if (_dependencies->empty()) {
        _watches.push_back(_files->watch(_path, [this](const std::filesystem::path&) { load_file(_path); }));
    }
}

bool check_lua_syntax(std::string_view code, std::string_view name, std::string& error) {
    sol::state lua;
    const sol::load_result loaded = lua.load(code, "=" + std::string(name)); // "=": named as it is, not [string "..."]
    if (loaded.valid()) {
        error.clear();
        return true;
    }
    const sol::error why = loaded;
    error = why.what();
    return false;
}

} // namespace kin
