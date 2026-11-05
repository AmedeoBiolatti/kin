#pragma once

#include <kin/core/types.hpp>

#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct lua_State;
struct lua_Debug;

namespace kin {

struct LuaRuntimeLimits {
    u64 instruction_limit = 50'000'000;        // per entry into Lua; 0 disables
    u64 memory_limit_bytes = 64 * 1024 * 1024; // per VM; 0 disables
};

struct LuaCallbackProfile {
    std::string name;
    u64 calls = 0, failures = 0, total_ns = 0, max_ns = 0, allocated_bytes = 0;
};

struct LuaRuntimeStats {
    u64 memory_bytes = 0, peak_memory_bytes = 0;
    u64 allocated_bytes = 0, allocations = 0, denied_allocations = 0;
    u64 gc_steps = 0, gc_ns = 0;
    std::vector<LuaCallbackProfile> callbacks;
};

// What a host's scripts can reach and what it calls them with, as `kin_lua api`
// writes it for editors and the static checker (tools/kin_lua.py).
struct LuaApiSurface {
    // Every global table and bound type, and its members.
    std::map<std::string, std::vector<std::string>> tables;
    // The members of a type read as fields (ctx.dt), not called.
    std::map<std::string, std::vector<std::string>> properties;
    // The short component names the host accepts ("transform" -> "Transform2D").
    std::map<std::string, std::string> component_aliases;
    // The fields get() returns, with their kinds, for components the host
    // shapes itself instead of as their schema describes them (none: a tag,
    // whose get() is true or nil).
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> component_fields;
    // The global functions the host calls, with the types of what it passes.
    std::map<std::string, std::vector<std::string>> callbacks;
};

// One accountant per VM. It must outlive the lua_State using its allocator.
// Native binding setup is accounted, but limits are enforced at protected Lua
// entry points (LuaCall), where allocation failure can safely become an error.
// Every call is also recorded in the current ProfileSession (category "lua").
class LuaRuntime {
public:
    explicit LuaRuntime(LuaRuntimeLimits limits = {}) : _limits(limits) {}
    const LuaRuntimeLimits& limits() const { return _limits; }
    void set_limits(LuaRuntimeLimits limits) { _limits = limits; }
    // Whether the VM holds more than its memory limit (checked by hosts after
    // native work, such as copying reload state, that cannot fail midway).
    bool memory_exceeded() const {
        return _limits.memory_limit_bytes && _stats.memory_bytes > _limits.memory_limit_bytes;
    }
    // memory_exceeded() inside a protected entry, where it can become a Lua error.
    bool over_memory_limit() const { return _protected_depth && memory_exceeded(); }
    LuaRuntimeStats stats() const;
    void collect(lua_State* state, i32 work = 16);

private:
    friend class LuaCall;
    friend class LuaNativeAllocations;
    friend struct LuaVm;
    static void* allocator(void* context, void* pointer, size_t old_size, size_t new_size);

    LuaRuntimeLimits _limits;
    LuaRuntimeStats _stats;
    std::map<std::string, LuaCallbackProfile, std::less<>> _profiles;
    u32 _protected_depth = 0;
    lua_State* _active_state = nullptr;
    bool _compiling = false;
};

// Shared instruction/debugger hook and profiling scope for every Lua host.
// Nested calls share their caller's remaining instruction budget. A budget
// violation continues trapping until control returns to C++, even through pcall.
//
//     LuaCall call{runtime, lua.lua_state(), "update"};
//     sol::protected_function_result result = call.invoke(update, dt);
class LuaCall {
public:
    LuaCall(LuaRuntime& runtime, lua_State* state, std::string_view name, u64 instruction_limit = 0);
    ~LuaCall();
    LuaCall(const LuaCall&) = delete;
    LuaCall& operator=(const LuaCall&) = delete;

    // Calls `function` with `args` in this scope; the profile counts a failed
    // call (or one that ran out of budget) as a failure.
    template <typename Function, typename... Args> auto invoke(Function& function, Args&&... args) {
        auto called = function(std::forward<Args>(args)...);
        _success = called.valid();
        return called;
    }

    // Compiles a chunk with `load` (whose allocations may fail as Lua errors
    // instead of panics). A chunk that does not compile counts as a failure;
    // invoke() then decides for one that does.
    template <typename Load> auto compile(Load&& load) {
        _success = false;
        _runtime._compiling = true;
        auto chunk = std::forward<Load>(load)();
        _runtime._compiling = false;
        return chunk;
    }

    void result(bool success) { _success = success; }
    bool exhausted() const { return _remaining == 0 && _limited; }

private:
    friend struct LuaVm;
    // Keep the hook/budget active through lua_close without touching the dead
    // state when the scope ends.
    void closing();

    LuaRuntime& _runtime;
    lua_State* _state;
    std::string _name;
    u64 _remaining, *_previous, _previous_call, _allocated;
    bool _limited, _success = true;
    void (*_hook)(lua_State*, lua_Debug*);
    int _mask, _count;
    lua_State* _previous_state;
    bool _previous_compiling;
    LuaRuntime* _previous_runtime;
    std::chrono::steady_clock::time_point _start;
};

} // namespace kin
