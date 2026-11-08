#pragma once

#include "script_scene_host.hpp"
#include "lua_vm.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace kin {

struct ScriptEngine::Impl {
    explicit Impl(LuaRuntimeLimits limits) : vm(limits) {}
    sol::state& lua() { return *vm.lua; }
    LuaRuntime& runtime() { return vm.runtime; }

    std::unique_ptr<ScriptEngineHost> host;
    std::shared_ptr<ScriptWorkflows2D> workflows;
    LuaVm vm;
    std::filesystem::path asset_root;
    std::vector<std::filesystem::path> dependencies;
    std::unordered_set<std::string> dependency_keys;
    std::function<void(sol::state&)> bind;
    std::shared_ptr<const void> content_owner;
    std::string halted; // why every call fails (halt)
};

namespace scripting_detail {

inline bool record_result(std::string& last_error, std::string_view name, const sol::protected_function_result& result) {
    if (result.valid()) {
        last_error.clear();
        return true;
    }

    sol::error error = result;
    last_error = std::string{name} + ": " + error.what();
    return false;
}

} // namespace scripting_detail
} // namespace kin
