#include "script_engine_internal.hpp"
#include "script_scene_api.hpp"
#include "lua_paths.hpp"

#include <kin/assets/content.hpp>
#include <kin/core/json.hpp>
#include <kin/platform/input.hpp>
#include <kin/scene/scene_manager.hpp>
#include <kin/scene/scene_preload2d.hpp>
#include <kin/scripting/script_scene.hpp>
#include <kin/scripting/workflows2d.hpp>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <sol/sol.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace kin {

using namespace scripting_detail;

namespace {

using scripting_detail::module_name_allowed;
using scripting_detail::path_within_root;

std::filesystem::path normalized_dependency_path(const std::filesystem::path& path) {
    return scripting_detail::normalized_path(path);
}

std::optional<std::filesystem::path> resolve_lua_module(const std::filesystem::path& asset_root, std::string_view module) {
    if (!module_name_allowed(module)) {
        return std::nullopt;
    }
    std::string relative{module};
    std::ranges::replace(relative, '.', '/');
    relative += ".lua";

    const std::filesystem::path candidates[] = {
        asset_root / "scripts" / relative,
        asset_root / relative,
    };
    for (const std::filesystem::path& candidate : candidates) {
        if (path_within_root(candidate, asset_root) && content_file_exists(candidate)) {
            return normalized_dependency_path(candidate);
        }
    }
    return std::nullopt;
}

// Loads a script from the content (a mounted pack or disk), named as
// luaL_loadfile names it, so errors read "path:line: message". Nullopt if the
// file cannot be read.
std::optional<sol::load_result> load_lua_file(sol::state_view lua, const std::filesystem::path& path) {
    const std::optional<std::string> code = read_content_file(path);
    if (!code) {
        return std::nullopt;
    }
    return lua.load(*code, "@" + path.string());
}

void write_lua_report_value(JsonWriter& json, const sol::object& value, i32 depth);

void write_lua_report_table(JsonWriter& json, const sol::table& table, i32 depth) {
    json.begin_object();
    if (depth <= 0) {
        json.end_object();
        return;
    }

    for (const auto& entry : table) {
        const sol::object key = entry.first;
        if (!key.is<std::string>()) {
            continue;
        }
        json.key(key.as<std::string>());
        write_lua_report_value(json, entry.second, depth - 1);
    }
    json.end_object();
}

void write_lua_report_value(JsonWriter& json, const sol::object& value, i32 depth) {
    switch (value.get_type()) {
    case sol::type::boolean:
        json.value(value.as<bool>());
        break;
    case sol::type::number:
        json.value(static_cast<f64>(value.as<double>()));
        break;
    case sol::type::string:
        json.value(std::string_view{value.as<std::string>()});
        break;
    case sol::type::table:
        write_lua_report_table(json, value.as<sol::table>(), depth);
        break;
    case sol::type::nil:
    default:
        json.value_null();
        break;
    }
}

} // namespace

ScriptEngine::ScriptEngine(const std::function<void(sol::state&)>& bind, LuaRuntimeLimits limits)
    : _impl(std::make_unique<Impl>(limits)) {
    _impl->bind = bind;
    // Lua seeds math.random from the clock and an address; a fixed seed until on_enter gives it the run's.
    seed_random(RngKey{});
    sol::table package = _impl->lua().create_table();
    package["loaded"] = _impl->lua().create_table();
    package["path"] = "";
    _impl->lua()["package"] = package;
    register_bindings();
    if (bind) {
        bind(_impl->lua());
    }
}

ScriptEngine::~ScriptEngine() {
    if (_impl->host) {
        _impl->host->closing = true;
        _impl->host->clear_systems();
        _impl->host->definitions.clear();
    }
    if (_impl->workflows) {
        _impl->workflows->leave(this);
    }
}

std::shared_ptr<ScriptWorkflows2D> ScriptEngine::workflows() const {
    return _impl->workflows;
}

bool ScriptEngine::load_file(const std::filesystem::path& path) {
    const std::filesystem::path absolute = normalized_dependency_path(path);
    return load_file(absolute.parent_path(), absolute);
}

void ScriptEngine::suspend_systems() {
    if (_impl->host)
        _impl->host->clear_systems();
}

void ScriptEngine::halt(std::string reason) {
    suspend_systems();
    _impl->halted = std::move(reason);
    _last_error = _impl->halted;
}

void ScriptEngine::use_content(std::shared_ptr<const MemoryContent> content) {
    _impl->asset_root = content->root();
    _impl->content_owner = content;
    if (_impl->workflows)
        _impl->workflows->use_content(std::move(content));
}
bool ScriptEngine::activate_systems() {
    if (!_impl->host)
        return true;
    if (!_impl->halted.empty())
        return false;
    for (const auto& [descriptor, callback] : _impl->host->definitions) {
        const auto id = _impl->host->world->systems().register_task(descriptor, callback);
        if (id.empty()) {
            _last_error = "cannot activate script system " + descriptor.id;
            return false;
        }
        _impl->host->systems.push_back(id);
    }
    return true;
}

bool ScriptEngine::load_file(const std::filesystem::path& asset_root, const std::filesystem::path& script_path) {
    LuaNativeAllocations setup{_impl->runtime()};
    if (auto view = view_content_file(asset_root / script_path))
        _impl->content_owner = std::move(view->owner);
    _impl->asset_root = normalized_dependency_path(asset_root);
    _impl->dependencies.clear();
    _impl->dependency_keys.clear();
    _impl->lua()["require"] = [this](sol::this_state state, const std::string& module) -> sol::object {
        sol::state_view lua{state};
        sol::table package = lua["package"];
        sol::table loaded_modules = package["loaded"];
        sol::object cached = loaded_modules[module];
        if (cached.get_type() != sol::type::nil && cached.get_type() != sol::type::boolean) {
            return cached;
        }
        if (cached.is<bool>() && cached.as<bool>()) {
            return cached;
        }

        std::optional<std::filesystem::path> module_path = resolve_lua_module(_impl->asset_root, module);
        if (!module_path) {
            throw std::runtime_error("module '" + module + "' was not found under asset root '" + _impl->asset_root.string() + "'");
        }
        record_dependency(*module_path);

        LuaCall call{_impl->runtime(), state, "module.load"};
        std::optional<sol::load_result> read = call.compile([&] { return load_lua_file(lua, *module_path); });
        if (!read) {
            throw std::runtime_error("cannot open " + module_path->string());
        }
        sol::load_result& loaded = *read;
        if (!loaded.valid()) {
            sol::error error = loaded;
            throw std::runtime_error(error.what());
        }
        sol::protected_function chunk = loaded;
        sol::protected_function_result result = call.invoke(chunk, module);
        if (!result.valid()) {
            sol::error error = result;
            throw std::runtime_error(error.what());
        }

        sol::object module_result = result;
        if (module_result.get_type() == sol::type::nil) {
            module_result = sol::make_object(state, true);
        }
        loaded_modules[module] = module_result;
        return module_result;
    };

    const std::filesystem::path resolved_script = script_path.is_absolute()
        ? normalized_dependency_path(script_path)
        : normalized_dependency_path(_impl->asset_root / script_path);
    if (!path_within_root(resolved_script, _impl->asset_root)) {
        _last_error = "script path is outside asset root: " + resolved_script.string();
        return false;
    }
    record_dependency(resolved_script);

    if (_impl->runtime().memory_exceeded()) {
        _last_error = "Lua memory limit exceeded during binding setup";
        return false;
    }
    LuaCall call{_impl->runtime(), _impl->vm.state(), "script.load"};
    std::optional<sol::load_result> read = call.compile([&] { return load_lua_file(_impl->lua(), resolved_script); });
    if (!read) {
        _last_error = "cannot open " + resolved_script.string();
        return false;
    }
    sol::load_result& loaded = *read;
    if (!loaded.valid()) {
        sol::error error = loaded;
        _last_error = error.what();
        return false;
    }

    sol::protected_function script = loaded;
    return record_result(_last_error, "script", call.invoke(script));
}

const std::vector<std::filesystem::path>& ScriptEngine::script_dependencies() const {
    return _impl->dependencies;
}

void ScriptEngine::record_dependency(const std::filesystem::path& path) {
    std::filesystem::path normalized = normalized_dependency_path(path);
    const std::string key = normalized.string();
    if (_impl->dependency_keys.insert(key).second) {
        _impl->dependencies.push_back(std::move(normalized));
    }
}

bool ScriptEngine::call_on_load(ScriptScene& scene) {
    return call_on_load({.world = &scene.ecs(),
                         .components = scene.script_components(),
                         .script_path = scene.resolved_script_path(),
                         .clear_entities = [&scene] { scene.clear_script_entities(); },
                         .set_background = [&scene](Color c) { scene.render_config().clear_color = c; },
                         .mark_render_dirty = [&scene] { scene.mark_render_cache_dirty("script api"); }});
}

bool ScriptEngine::call_on_load(ScriptSceneBindings bindings) {
    if (!bindings.world || (_impl->host && _impl->host->world != bindings.world)) {
        _last_error = "script bindings require one live world per Lua state";
        return false;
    }
    if (!_impl->host)
        _impl->host = std::make_unique<ScriptEngineHost>();
    _impl->host->runtime = &_impl->runtime();
    _impl->host->clear_systems();
    static_cast<ScriptSceneBindings&>(*_impl->host) = std::move(bindings);
    try {
        _impl->workflows = ScriptWorkflows2D::shared(*_impl->host->world, _impl->asset_root, _impl->host->workflows);
    } catch (const std::exception& e) {
        _last_error = e.what();
        return false;
    }
    return call_optional("on_load", ScriptSceneApi{_impl->host.get()});
}

bool ScriptEngine::call_on_enter(SceneContext& ctx) {
    // math.random follows the run's --seed: the same numbers in every run with it, on every peer.
    seed_random(fold_in(ctx.rng, script_random_stream));
    return call_on_enter(ScriptFrameContext{ctx.dt, &ctx.input, &ctx.app});
}

void ScriptEngine::seed_random(RngKey key) {
    const sol::protected_function randomseed = _impl->lua()["math"]["randomseed"];
    if (randomseed.valid()) {
        randomseed(static_cast<lua_Integer>(key.lo), static_cast<lua_Integer>(key.hi));
    }
}

bool ScriptEngine::call_on_exit(SceneContext& ctx) {
    return call_on_exit(ScriptFrameContext{ctx.dt, &ctx.input, &ctx.app});
}
bool ScriptEngine::call_update(SceneContext& ctx) {
    const bool result = call_update(ScriptFrameContext{ctx.dt, &ctx.input, &ctx.app});
    if (_impl->workflows)
        while (auto scene = _impl->workflows->take_scene())
            ctx.scenes.replace(std::move(scene));
    return result;
}
bool ScriptEngine::call_on_enter(ScriptFrameContext ctx) {
    return call_optional("on_enter", ScriptContextApi{ctx});
}
bool ScriptEngine::call_on_exit(ScriptFrameContext ctx) {
    const bool result = call_optional("on_exit", ScriptContextApi{ctx});
    _impl->vm.tasks->cancel_all();
    return result;
}
bool ScriptEngine::call_update(ScriptFrameContext ctx) {
    if (!_impl->halted.empty()) {
        _last_error = _impl->halted;
        return false;
    }
    if (_impl->workflows) {
        static const Input absent;
        _impl->workflows->step(this, ctx.input ? *ctx.input : absent, ctx.dt);
    }
    if (!_impl->vm.tasks->advance(ctx.dt, _last_error))
        return false;
    _impl->vm.collect();
    return call_optional("update", ScriptContextApi{ctx});
}

bool ScriptEngine::call_collect_actions(InputActionContext& actions) const {
    return call_optional("collect_actions", ScriptActionsApi{&actions});
}

bool ScriptEngine::call_write_report(JsonWriter& json) const {
    if (!_impl->halted.empty()) {
        _last_error = _impl->halted;
        return false;
    }
    sol::object object = _impl->lua().globals().raw_get<sol::object>("write_report");
    if (object.get_type() == sol::type::nil) {
        return true;
    }
    if (object.get_type() != sol::type::function) {
        _last_error = "write_report is not a function";
        return false;
    }

    sol::protected_function function = object;
    LuaCall call{_impl->runtime(), _impl->vm.state(), "write_report"};
    sol::protected_function_result result = call.invoke(function);
    if (!result.valid()) {
        return record_result(_last_error, "write_report", result);
    }

    sol::object report = result;
    if (report.get_type() == sol::type::nil) {
        _last_error.clear();
        return true;
    }
    if (report.get_type() != sol::type::table) {
        _last_error = "write_report must return a table or nil";
        return false;
    }

    sol::table table = report.as<sol::table>();
    for (const auto& entry : table) {
        const sol::object key = entry.first;
        if (!key.is<std::string>()) {
            continue;
        }
        json.key(key.as<std::string>());
        write_lua_report_value(json, entry.second, 6);
    }
    _last_error.clear();
    return true;
}

LuaRuntimeStats ScriptEngine::runtime_stats() const {
    return _impl->runtime().stats();
}

void ScriptEngine::set_runtime_limits(LuaRuntimeLimits limits) {
    _impl->runtime().set_limits(limits);
}

} // namespace kin
