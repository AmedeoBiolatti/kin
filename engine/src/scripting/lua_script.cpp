#include <kin/scripting/lua_script.hpp>

#include "lua_paths.hpp"

#include <kin/assets/file_watcher.hpp>
#include <kin/platform/log.hpp>

#include <stdexcept>
#include <utility>

namespace kin {

namespace {

// The budget of the load or call running on this thread (null: no limit). The
// count hook takes 1,000 instructions from it at a time and stops the script
// with an error when it runs out.
thread_local u64* t_budget = nullptr;
constexpr int hook_interval = 1000;

void count_hook(lua_State* state, lua_Debug*) {
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

std::string error_text(const sol::protected_function_result& result) {
    const sol::error error = result;
    return error.what();
}

} // namespace

LuaScript::Budget::Budget(const LuaScript& script)
    : _remaining(script._options.instruction_limit), _previous(t_budget) {
    t_budget = _remaining > 0 ? &_remaining : nullptr;
}

LuaScript::Budget::~Budget() {
    t_budget = _previous;
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
    lua->open_libraries(sol::lib::base, sol::lib::math, sol::lib::table, sol::lib::string, sol::lib::utf8);
    // Nothing that reads the world outside the script, or makes up numbers.
    for (const char* name : {"dofile", "loadfile", "load", "require", "collectgarbage"}) {
        (*lua)[name] = sol::lua_nil;
    }
    (*lua)["math"]["random"] = sol::lua_nil;
    (*lua)["math"]["randomseed"] = sol::lua_nil;
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

    if (_options.instruction_limit > 0) {
        lua_sethook(lua->lua_state(), count_hook, LUA_MASKCOUNT, hook_interval);
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
    if (!run(*code, "@" + path.filename().string(), {scripting_detail::normalized_path(path)})) {
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
        const Budget budget{*this};
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

} // namespace kin
