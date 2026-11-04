#pragma once

// The Lua VM every kin host is made of (LuaScript, ScriptEngine), and the
// runtime pieces only hosts use: native-allocation scopes and shared budgets.

#include "lua_tasks.hpp"

#include <kin/scripting/lua_runtime.hpp>

#include <memory>
#include <sol/sol.hpp>

namespace kin {

// The instruction budget of host work outside script calls: closing a VM, a
// garbage-collection step, preparing a reload.
inline constexpr u64 lua_maintenance_budget = 1'000'000;

// One VM: its accountant, its state (base, math, table, string and utf8, with
// kin's script errors) and its `task` scheduler. The host adds its sandbox and
// API. Tasks are released before the VM, which closes under a bounded budget.
struct LuaVm {
    explicit LuaVm(LuaRuntimeLimits limits);
    ~LuaVm();
    LuaVm(const LuaVm&) = delete;
    LuaVm& operator=(const LuaVm&) = delete;

    lua_State* state() const { return lua->lua_state(); }
    void collect(i32 work = 16) { runtime.collect(state(), work); }

    LuaRuntime runtime;
    std::unique_ptr<sol::state> lua;
    std::unique_ptr<LuaTasks> tasks;
};

// Bounded native preparation (e.g. copying reload data) uses Lua's unprotected
// allocation APIs. Account these allocations, then validate the resulting heap
// (LuaRuntime::memory_exceeded) before executing candidate Lua, instead of
// letting Lua panic across C++ frames.
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

// Caps every LuaCall made inside it, together, at `limit` instructions, as an
// enclosing LuaCall would, without tying the scope to one VM (reload
// preparation builds candidate states and replaces them).
class LuaInstructionBudget {
public:
    explicit LuaInstructionBudget(u64 limit);
    ~LuaInstructionBudget();
    LuaInstructionBudget(const LuaInstructionBudget&) = delete;
    LuaInstructionBudget& operator=(const LuaInstructionBudget&) = delete;

private:
    u64 _remaining;
    u64* _previous;
};

} // namespace kin
