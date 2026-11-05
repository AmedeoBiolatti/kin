#include <kin/scripting/lua_script.hpp>

#include "lua_api_surface.hpp"
#include "lua_l10n.hpp"
#include "lua_services.hpp"
#include "lua_paths.hpp"
#include "lua_vm.hpp"

#include <kin/assets/file_watcher.hpp>
#include <kin/platform/log.hpp>

#include <algorithm>
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
    _vm = make_vm(_dependencies);
}

LuaScript::~LuaScript() {
    if (_files && !_files_alive.expired()) {
        for (const u32 id : _watches) {
            _files->unwatch(id);
        }
    }
}

std::unique_ptr<LuaVm> LuaScript::make_vm(const Dependencies& dependencies) const {
    auto vm = std::make_unique<LuaVm>(_vm ? _vm->runtime.limits() : _options.limits);
    sol::state* lua = vm->lua.get();
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

    if (_options.setup) {
        _options.setup(*lua);
    }
    return vm;
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
    // A fresh state, replacing the current one only if the script loads and
    // runs (a rejected one closes, with its tasks, when it goes).
    auto fresh_dependencies = std::make_shared<std::vector<std::filesystem::path>>(std::move(dependencies));
    std::unique_ptr<LuaVm> fresh = make_vm(fresh_dependencies);
    {
        LuaCall call{fresh->runtime, fresh->state(), "script.load"};
        sol::load_result chunk = call.compile([&] { return fresh->lua->load(code, chunk_name); });
        if (!chunk.valid()) {
            const sol::error error = chunk;
            fail(error.what());
            return false;
        }
        sol::protected_function script = chunk;
        const sol::protected_function_result result = call.invoke(script);
        if (!result.valid()) {
            fail(error_text(result));
            return false;
        }
    }
    _dependencies = std::move(fresh_dependencies);
    _vm.swap(fresh); // the old state closes here
    _loaded = true;
    _error.clear();
    return true;
}

sol::state& LuaScript::lua() {
    return *_vm->lua;
}

LuaRuntime& LuaScript::runtime() {
    return _vm->runtime;
}

const LuaRuntime& LuaScript::runtime() const {
    return _vm->runtime;
}

bool LuaScript::advance_tasks(f32 dt, const std::function<bool(u64)>& owner_alive) {
    std::string error;
    if (!_vm->tasks->advance(dt, error, owner_alive)) {
        fail("task: " + error);
        return false;
    }
    _vm->collect();
    return true;
}

void LuaScript::set_task_owner(u64 owner) {
    _vm->tasks->set_owner(owner);
}

void LuaScript::cancel_tasks() {
    _vm->tasks->cancel_all();
}

void LuaScript::cancel_tasks(u64 owner) {
    _vm->tasks->cancel(owner);
}

LuaApiSurface LuaScript::api_surface() const {
    return scripting_detail::lua_api_surface(*_vm->lua);
}

bool LuaScript::has(std::string_view function) const {
    return find(function).valid();
}

sol::protected_function LuaScript::find(std::string_view function) const {
    if (!_loaded) {
        return {};
    }
    const sol::object value = _vm->lua->globals().raw_get<sol::object>(std::string(function));
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
