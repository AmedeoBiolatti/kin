#include <kin/scripting/lua_runtime.hpp>

#include "lua_paths.hpp"
#include "lua_vm.hpp"

#include <kin/core/profile.hpp>
#include <kin/core/script_debugger.hpp>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <sol/sol.hpp>
#include <utility>

namespace kin {

namespace {

// The budget of the load or call running on this thread (null: no limit). The
// count hook takes 1,000 instructions from it at a time and stops the script
// with an error when it runs out.
void script_hook(lua_State*, lua_Debug*);
thread_local u64* t_budget = nullptr;
thread_local LuaRuntime* t_runtime = nullptr;
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
        lua_sethook(state, script_hook, LUA_MASKCOUNT, 1);
        luaL_error(state, "instruction limit reached (instruction budget exhausted; a loop that never ends?)");
        return;
    }
    *t_budget -= static_cast<u64>(hook_interval);
}

// --- What a stop shows -------------------------------------------------------

constexpr std::size_t shown_text = 120;  // bytes of a string shown
constexpr std::size_t shown_fields = 64; // of a table
constexpr i32 shown_depth = 2;           // tables in tables
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
        case '\n':
            out += "\\n";
            break;
        case '\t':
            out += "\\t";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        default:
            out += c;
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
    case LUA_TNIL:
        value.value = "nil";
        break;
    case LUA_TBOOLEAN:
        value.value = lua_toboolean(state, index) ? "true" : "false";
        break;
    case LUA_TNUMBER:
        value.value = number_text(state, index);
        break;
    case LUA_TSTRING: {
        size_t size = 0;
        const char* text = lua_tolstring(state, index, &size);
        value.value = quoted({text, size});
        break;
    }
    case LUA_TTABLE:
        describe_fields(state, index, depth, value);
        break;
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
    case LUA_TLIGHTUSERDATA:
        value.value = userdata_text(state, index);
        break;
    default:
        value.value = value.kind;
        break;
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
    if (at->event == LUA_HOOKCALL || at->event == LUA_HOOKTAILCALL) {
        // Once per entry, at its first call: the host's arguments are on the
        // heap by then. Further in, the allocator refuses what would exceed
        // the limit itself, so later calls run without this hook.
        lua_sethook(state, script_hook, lua_gethookmask(state) & ~LUA_MASKCALL, lua_gethookcount(state));
        if (t_runtime && t_runtime->over_memory_limit()) {
            luaL_error(state, "Lua memory limit exceeded");
        }
    } else if (at->event == LUA_HOOKCOUNT) {
        count_hook(state);
    } else if (at->event == LUA_HOOKLINE) {
        line_hook(state, at);
    }
}

} // namespace

void* LuaRuntime::allocator(void* context, void* pointer, size_t old_size, size_t new_size) {
    auto& vm = *static_cast<LuaRuntime*>(context);
    const u64 old = pointer ? old_size : 0;
    if (new_size == 0) {
        std::free(pointer);
        vm._stats.memory_bytes -= old;
        return nullptr;
    }
    const u64 base = vm._stats.memory_bytes - old;
    lua_Debug frame{};
    // Host argument pushes and binding setup run outside Lua's error handler.
    // Refuse allocations only inside protected parsing or an executing Lua/C
    // frame. The call hook rejects oversized host arguments before the body.
    const bool protected_allocation =
        vm._protected_depth && (vm._compiling || (vm._active_state && lua_getstack(vm._active_state, 0, &frame)));
    if (new_size > std::numeric_limits<u64>::max() - base ||
        (new_size > old && protected_allocation && vm._limits.memory_limit_bytes &&
         new_size > vm._limits.memory_limit_bytes - std::min(base, vm._limits.memory_limit_bytes))) {
        ++vm._stats.denied_allocations;
        return nullptr;
    }
    void* next = std::realloc(pointer, new_size);
    if (next) {
        vm._stats.memory_bytes = base + new_size;
        vm._stats.peak_memory_bytes = std::max(vm._stats.peak_memory_bytes, vm._stats.memory_bytes);
        ++vm._stats.allocations;
        if (new_size > old) {
            vm._stats.allocated_bytes += new_size - old;
        }
    }
    return next;
}

LuaRuntimeStats LuaRuntime::stats() const {
    auto result = _stats;
    for (const auto& [name, profile] : _profiles) {
        result.callbacks.push_back(profile);
    }
    return result;
}

void LuaRuntime::collect(lua_State* state, i32 work) {
    const auto start = std::chrono::steady_clock::now();
    LuaCall budget{*this, state, "vm.gc", lua_maintenance_budget};
    LuaNativeAllocations cleanup{*this};
    lua_gc(state, LUA_GCSTEP, std::clamp(work, 1, 1024));
    ++_stats.gc_steps;
    _stats.gc_ns +=
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
}

LuaCall::LuaCall(LuaRuntime& runtime, lua_State* state, std::string_view name, u64 limit)
    : _runtime(runtime), _state(state), _name(name), _remaining(limit ? limit : runtime.limits().instruction_limit),
      _previous(t_budget), _previous_call(t_call), _allocated(runtime._stats.allocated_bytes),
      _limited(_remaining != 0), _hook(lua_gethook(state)), _mask(lua_gethookmask(state)),
      _count(lua_gethookcount(state)), _previous_state(runtime._active_state), _previous_compiling(runtime._compiling),
      _previous_runtime(t_runtime), _start(std::chrono::steady_clock::now()) {
    if (_previous && (!_limited || *_previous < _remaining)) {
        _remaining = *_previous;
        _limited = true;
    }
    t_budget = _limited ? &_remaining : nullptr;
    t_call = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    ++runtime._protected_depth;
    runtime._active_state = state;
    runtime._compiling = false;
    t_runtime = &runtime;
    const int mask = (_limited ? LUA_MASKCOUNT : 0) | (script_debugger().armed() ? LUA_MASKLINE : 0) |
                     (runtime.limits().memory_limit_bytes ? LUA_MASKCALL : 0);
    lua_sethook(state, mask ? script_hook : nullptr, mask, hook_interval);
}

LuaCall::~LuaCall() {
    if (_previous && _limited) {
        *_previous = std::min(*_previous, _remaining);
    }
    t_budget = _previous;
    t_call = _previous_call;
    if (_state) {
        lua_sethook(_state, _hook, _mask, _count);
        --_runtime._protected_depth;
    }
    _runtime._active_state = _previous_state;
    _runtime._compiling = _previous_compiling;
    t_runtime = _previous_runtime;
    const auto ns = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - _start).count());
    auto found = _runtime._profiles.find(_name);
    if (found == _runtime._profiles.end()) {
        std::string key = _runtime._profiles.size() < 128 ? _name : std::string("<other>");
        found = _runtime._profiles.try_emplace(key, LuaCallbackProfile{.name = key}).first;
    }
    LuaCallbackProfile& profile = found->second;
    ++profile.calls;
    profile.failures += !_success || exhausted();
    profile.total_ns += ns;
    profile.max_ns = std::max(profile.max_ns, ns);
    profile.allocated_bytes += _runtime._stats.allocated_bytes - _allocated;
    if (ProfileSession* session = current_profile_session()) {
        session->record(_name, "lua", ns);
    }
}

void LuaCall::closing() {
    if (_state) {
        // Native binding finalizers must be able to release their Lua
        // registry entries even when the rejected VM is over its budget.
        --_runtime._protected_depth;
        _state = nullptr;
    }
}

LuaVm::LuaVm(LuaRuntimeLimits limits)
    : runtime(limits), lua(std::make_unique<sol::state>(sol::default_at_panic, LuaRuntime::allocator, &runtime)) {
    scripting_detail::bind_script_errors(*lua);
    lua->open_libraries(sol::lib::base, sol::lib::math, sol::lib::table, sol::lib::string, sol::lib::utf8);
    tasks = std::make_unique<LuaTasks>(*lua, runtime);
}

LuaVm::~LuaVm() {
    tasks.reset(); // task references are released before their VM
    LuaCall close{runtime, state(), "vm.close", lua_maintenance_budget};
    close.closing();
    lua.reset();
}

LuaInstructionBudget::LuaInstructionBudget(u64 limit) : _remaining(limit), _previous(t_budget) {
    if (_previous) {
        _remaining = std::min(_remaining, *_previous);
    }
    t_budget = &_remaining;
}

LuaInstructionBudget::~LuaInstructionBudget() {
    if (_previous) {
        *_previous = std::min(*_previous, _remaining);
    }
    t_budget = _previous;
}

} // namespace kin
