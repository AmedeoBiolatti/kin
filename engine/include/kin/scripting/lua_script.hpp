#pragma once

#include <kin/core/types.hpp>
#include <kin/scripting/lua_runtime.hpp>

#include <sol/sol.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kin {

class FileWatcher;
struct LuaVm;

// A Lua script for code that does not live in the ECS: a game's rules, AI,
// balance formulas or level logic, run by plain C++ (e.g. an engine-free
// simulation). Unlike ScriptScene, it knows nothing of scenes or entities: the
// host binds what a script may ask and do, and calls the functions it defines.
//
//   kin::LuaScript rules{{.setup = [&](sol::state& lua) {
//       sol::table game = lua.create_named_table("game");
//       game["give"] = [&](const std::string& item, int count) { base.give(item, count); };
//   }}};
//   rules.load_file(root / "scripts/rules.lua");
//   rules.call("on_built", "pump", q, r);   // runs the handler if the script defines one
//   rules.watch(files);                     // reload on edit, keeping the last good script
//
// The sandbox is deterministic: Lua's base, math, table, string and utf8
// libraries, without files, time, random numbers or loading code (`load`,
// `dofile`, `loadfile`; `require` only from `module_root`); `print` writes to
// kin's log. `pairs` walks a table in no fixed order, so use `ipairs` (or sort)
// where order matters. Every load and call runs under an instruction budget,
// so a script stuck in a loop fails that call instead of freezing the game.
struct LuaScriptOptions {
    // Binds the host's API. Runs on every fresh state (each load), before the
    // script itself.
    std::function<void(sol::state&)> setup;
    // Where require("a.b") finds a/b.lua. Empty: require is not available.
    std::filesystem::path module_root;
    // Lua instructions one load or call may run (checked every 1,000) and the
    // bytes the state may hold; 0: no limit. runtime().set_limits() changes
    // them for the loads that follow.
    LuaRuntimeLimits limits;
    // Told every failure (load or call, with script:line). Default: logged as a
    // warning under "script".
    std::function<void(std::string_view message)> on_error;
};

// Whether `code` is Lua that reads (it is not run): false, and `error` says
// where (name:line) and why, when it does not.
bool check_lua_syntax(std::string_view code, std::string_view name, std::string& error);

class LuaScript {
public:
    explicit LuaScript(LuaScriptOptions options = {});
    ~LuaScript();
    LuaScript(const LuaScript&) = delete;
    LuaScript& operator=(const LuaScript&) = delete;

    // Runs a script in a fresh sandbox. On success it replaces the script that
    // was loaded; on failure that one stays (see error()).
    bool load_file(const std::filesystem::path& path);
    bool load_string(std::string_view code, std::string_view name = "script");
    bool loaded() const { return _loaded; }

    // Whether the loaded script defines a global function `function`.
    bool has(std::string_view function) const;
    // The loaded script's global function `function` as it is now (invalid
    // when it defines none), for a host calling it many times in a row:
    // call_function() then skips looking it up each time.
    sol::protected_function function(std::string_view function) const { return find(function); }

    // Calls the script's global `function` with `args`, if it defines one.
    // Returns false if it does not, or if the call fails (error() says why).
    template <typename... Args>
    bool call(std::string_view function, Args&&... args);
    // Like call(), and converts the function's first result to R: nullopt if
    // missing, failing, or not an R.
    template <typename R, typename... Args>
    std::optional<R> call_for(std::string_view function, Args&&... args);
    // Calls `target`, found by function(name) in this script's state (false
    // when it is invalid or fails); `name` is what failures and profiles say.
    template <typename... Args>
    bool call_function(const sol::protected_function& target, std::string_view name, Args&&... args);

    // The last failure, with script:line where Lua knows it; empty after a
    // successful load.
    const std::string& error() const { return _error; }
    // The loaded script's file and the modules it required.
    const std::vector<std::filesystem::path>& dependencies() const { return *_dependencies; }

    // Reloads the script whenever its file or a module it required changes,
    // keeping the last good version when the new one fails. Polling `files`
    // runs the reloads; either may be destroyed first.
    void watch(FileWatcher& files);

    // The live Lua state, for what this API does not cover. A load replaces it.
    sol::state& lua();
    // Its limits, memory and per-call profiles (a load starts a fresh one,
    // with the same limits).
    LuaRuntime& runtime();
    const LuaRuntime& runtime() const;

    // The script's `task`s, stepped only by the host's simulation, never wall
    // time. A task belongs to the owner set when it starts (0: none); one
    // whose owner is not owner_alive(owner) is cancelled.
    bool advance_tasks(f32 dt, const std::function<bool(u64)>& owner_alive = {});
    void set_task_owner(u64 owner);
    void cancel_tasks();
    void cancel_tasks(u64 owner);

    // What the state binds, for kin_lua api.
    LuaApiSurface api_surface() const;

private:
    using Dependencies = std::shared_ptr<std::vector<std::filesystem::path>>;
    std::unique_ptr<LuaVm> make_vm(const Dependencies& dependencies) const;
    bool run(std::string_view code, const std::string& chunk_name, std::vector<std::filesystem::path> dependencies);
    sol::protected_function find(std::string_view function) const;
    bool check(const sol::protected_function_result& result, std::string_view function);
    void fail(std::string message);
    void rewatch();

    LuaScriptOptions _options;
    std::unique_ptr<LuaVm> _vm;
    bool _loaded = false;
    std::string _error;
    std::filesystem::path _path;
    Dependencies _dependencies; // shared with the state's require, which adds to it
    FileWatcher* _files = nullptr;
    std::weak_ptr<const void> _files_alive;
    std::vector<u32> _watches;
};

template <typename... Args>
bool LuaScript::call(std::string_view function, Args&&... args) {
    return call_function(find(function), function, std::forward<Args>(args)...);
}

template <typename... Args>
bool LuaScript::call_function(const sol::protected_function& target, std::string_view name, Args&&... args) {
    if (!target.valid()) {
        return false;
    }
    LuaCall call{runtime(), lua().lua_state(), name};
    const auto result = call.invoke(target, std::forward<Args>(args)...);
    return check(result, name);
}

template <typename R, typename... Args>
std::optional<R> LuaScript::call_for(std::string_view function, Args&&... args) {
    sol::protected_function target = find(function);
    if (!target.valid()) {
        return std::nullopt;
    }
    LuaCall call{runtime(), lua().lua_state(), function};
    const sol::protected_function_result result = call.invoke(target, std::forward<Args>(args)...);
    if (!check(result, function)) {
        return std::nullopt;
    }
    const sol::optional<R> value = result.template get<sol::optional<R>>();
    if (!value) {
        fail(std::string(function) + ": returned a value of the wrong type");
        return std::nullopt;
    }
    return *value;
}

} // namespace kin
