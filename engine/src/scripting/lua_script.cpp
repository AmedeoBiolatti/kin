#include "lua_api_surface.hpp"
#include "lua_l10n.hpp"
#include "lua_services.hpp"
#include "lua_paths.hpp"
#include "lua_tasks.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <kin/assets/file_watcher.hpp>
#include <kin/core/script_debugger.hpp>
#include <kin/platform/log.hpp>
#include <kin/scripting/lua_script.hpp>
#include <stdexcept>
#include <utility>

namespace kin {

namespace {
std::string error_text(const sol::protected_function_result& result) {
    const sol::error error = result;
    return error.what();
}
} // namespace

LuaScript::LuaScript(LuaScriptOptions options)
    : _options(std::move(options)), _dependencies(std::make_shared<std::vector<std::filesystem::path>>()) {
    _lua = make_state(_dependencies, _runtime, _tasks);
}

LuaScript::~LuaScript() {
    if (_files && !_files_alive.expired()) {
        for (const u32 id : _watches) {
            _files->unwatch(id);
        }
    }
    _tasks.reset();
    if (_lua) {
        LuaCall budget{*_runtime, _lua->lua_state(), "vm.close", 1'000'000};
        budget.closing();
        _lua.reset();
    }
}

std::unique_ptr<sol::state> LuaScript::make_state(const Dependencies& dependencies,
                                                  std::shared_ptr<LuaRuntime>& runtime,
                                                  std::unique_ptr<LuaTasks>& tasks) const {
    const auto limits =
        _runtime ? _runtime->limits() : LuaRuntimeLimits{_options.instruction_limit, _options.memory_limit_bytes};
    runtime = std::make_shared<LuaRuntime>(limits);
    runtime->set_profile_sink(_options.on_profile);
    auto lua = std::make_unique<sol::state>(sol::default_at_panic, LuaRuntime::allocator, runtime.get());
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
    scripting_detail::bind_services(*lua);
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

    tasks = std::make_unique<LuaTasks>(*lua, *runtime);
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
    std::shared_ptr<LuaRuntime> fresh_runtime;
    std::unique_ptr<LuaTasks> fresh_tasks;
    std::unique_ptr<sol::state> fresh = make_state(fresh_dependencies, fresh_runtime, fresh_tasks);
    // Task references must close before their VM, including a rejected candidate.
    struct Cleanup {
        std::unique_ptr<LuaTasks>& tasks;
        std::unique_ptr<sol::state>& lua;
        std::shared_ptr<LuaRuntime>& runtime;
        ~Cleanup() {
            tasks.reset();
            if (lua) {
                LuaCall budget{*runtime, lua->lua_state(), "vm.close", 1'000'000};
                budget.closing();
                lua.reset();
            }
        }
    } cleanup{fresh_tasks, fresh, fresh_runtime};
    LuaCall budget{*fresh_runtime, fresh->lua_state(), "script.load"};
    budget.compiling();
    budget.result(false);
    sol::load_result chunk = fresh->load(code, chunk_name);
    if (!chunk.valid()) {
        const sol::error error = chunk;
        fail(error.what());
        return false;
    }
    sol::protected_function script = chunk;
    budget.compiling(false);
    {
        const sol::protected_function_result result = script();
        budget.result(result.valid());
        if (!result.valid()) {
            fail(error_text(result));
            return false;
        }
    }
    _dependencies = std::move(fresh_dependencies);
    // Close objects in the old VM before closing the VM/accountant.
    _tasks.reset();
    _lua.swap(fresh);
    _runtime.swap(fresh_runtime);
    _tasks = std::move(fresh_tasks);
    _loaded = true;
    _error.clear();
    return true;
}

bool LuaScript::advance_tasks(f32 dt, const std::function<bool(std::string_view)>& alive) {
    std::string error;
    if (!_tasks->advance(dt, error, alive)) {
        fail("task: " + error);
        return false;
    }
    _runtime->collect(_lua->lua_state());
    return true;
}
void LuaScript::set_task_owner(std::string owner) {
    _tasks->owner(std::move(owner));
}
void LuaScript::cancel_tasks(std::string_view owner) {
    _tasks->cancel(owner);
}
std::map<std::string, std::vector<std::string>> LuaScript::api_surface() const {
    return scripting_detail::lua_api_surface(*_lua);
}

bool LuaScript::has(std::string_view function) const {
    return find(function).valid();
}

sol::protected_function LuaScript::find(std::string_view function) const {
    if (!_loaded) {
        return {};
    }
    const sol::object value = _lua->globals().raw_get<sol::object>(std::string(function));
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
