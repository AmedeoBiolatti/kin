#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <kin/core/types.hpp>
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

// One accountant per VM. It must outlive the lua_State using allocator().
// Native binding setup is accounted, but limits are enforced at protected Lua
// entry points (LuaCall), where allocation failure can safely become an error.
class LuaRuntime {
public:
    explicit LuaRuntime(LuaRuntimeLimits limits = {}) : _limits(limits) {}
    static void* allocator(void* context, void* pointer, size_t old_size, size_t new_size);
    const LuaRuntimeLimits& limits() const { return _limits; }
    void set_limits(LuaRuntimeLimits limits) { _limits = limits; }
    bool over_memory_limit() const {
        return _protected_depth && _limits.memory_limit_bytes && _stats.memory_bytes > _limits.memory_limit_bytes;
    }
    LuaRuntimeStats stats() const;
    void collect(lua_State* state, i32 work = 16);
    void set_profile_sink(std::function<void(std::string_view, u64)> sink) { _profile_sink = std::move(sink); }

private:
    friend class LuaCall;
    friend class LuaNativeAllocations;
    LuaRuntimeLimits _limits;
    LuaRuntimeStats _stats;
    std::map<std::string, LuaCallbackProfile, std::less<>> _profiles;
    u32 _protected_depth = 0;
    lua_State* _active_state = nullptr;
    bool _compiling = false;
    std::function<void(std::string_view, u64)> _profile_sink;
};

// Bounded native preparation (e.g. copying reload data) uses Lua's unprotected
// allocation APIs. Account these allocations, then validate the resulting heap
// before executing candidate Lua, instead of letting Lua panic across C++ frames.
class LuaNativeAllocations {
public:
    explicit LuaNativeAllocations(LuaRuntime& runtime) : _runtime(runtime), _depth(runtime._protected_depth) {
        runtime._protected_depth = 0;
    }
    ~LuaNativeAllocations() { _runtime._protected_depth = _depth; }
    LuaNativeAllocations(const LuaNativeAllocations&) = delete;
    LuaNativeAllocations& operator=(const LuaNativeAllocations&) = delete;

private:
    LuaRuntime& _runtime;
    u32 _depth;
};

// Shared instruction/debugger hook and profiling scope for every Lua host.
// Nested calls share their caller's remaining instruction budget. A budget
// violation continues trapping until control returns to C++, even through pcall.
class LuaCall {
public:
    LuaCall(LuaRuntime& runtime, lua_State* state, std::string_view name, u64 instruction_limit = 0);
    ~LuaCall();
    LuaCall(const LuaCall&) = delete;
    LuaCall& operator=(const LuaCall&) = delete;
    void result(bool success) { _success = success; }
    // Keep the hook/budget active through lua_close without touching the dead
    // state when the scope ends.
    void closing() {
        if (_state) {
            // Native binding finalizers must be able to release their Lua
            // registry entries even when the rejected VM is over its budget.
            --_runtime._protected_depth;
            _state = nullptr;
        }
    }
    bool exhausted() const { return _remaining == 0 && _limited; }
    void compiling(bool active = true) { _runtime._compiling = active; }

private:
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
