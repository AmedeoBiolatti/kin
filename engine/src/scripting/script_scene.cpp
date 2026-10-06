#include <kin/scripting/script_scene.hpp>

#include <kin/core/json.hpp>
#include <kin/ecs/render.hpp>
#include <kin/ecs/system.hpp>
#include <kin/platform/log.hpp>
#include <kin/scripting/script_engine.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <system_error>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace kin {
namespace {

void clear_script_systems(EcsWorld& world) {
    std::vector<SystemId> to_remove;
    for (const SystemSnapshot& snapshot : world.systems().snapshots()) {
        if (snapshot.kind == SystemKind::Script && snapshot.owner_scope == SystemOwnerScope::Scene) {
            to_remove.push_back(snapshot.id);
        }
    }
    for (const SystemId& id : to_remove) {
        world.systems().remove(id);
    }
}

ScriptDiagnostic parse_script_diagnostic(std::string error, std::string function_name = {}) {
    ScriptDiagnostic diagnostic{
        .message = std::move(error),
        .function_name = std::move(function_name),
    };

    const std::string system_marker = "system '";
    if (const std::size_t system_begin = diagnostic.message.find(system_marker); system_begin != std::string::npos) {
        const std::size_t id_begin = system_begin + system_marker.size();
        const std::size_t id_end = diagnostic.message.find('\'', id_begin);
        if (id_end != std::string::npos) {
            diagnostic.system_id = diagnostic.message.substr(id_begin, id_end - id_begin);
        }
    }
    const std::string entity_marker = "entity=";
    if (const std::size_t entity_begin = diagnostic.message.find(entity_marker); entity_begin != std::string::npos) {
        const std::size_t id_begin = entity_begin + entity_marker.size();
        std::size_t id_end = id_begin;
        while (id_end < diagnostic.message.size() && std::isdigit(static_cast<unsigned char>(diagnostic.message[id_end]))) {
            ++id_end;
        }
        if (id_end != id_begin) {
            diagnostic.entity_id = static_cast<EcsId>(std::stoull(diagnostic.message.substr(id_begin, id_end - id_begin)));
        }
    }

    const std::size_t lua_ext = diagnostic.message.rfind(".lua:");
    if (lua_ext == std::string::npos) {
        return diagnostic;
    }
    const std::size_t line_begin = lua_ext + 5;
    std::size_t line_end = line_begin;
    while (line_end < diagnostic.message.size() && std::isdigit(static_cast<unsigned char>(diagnostic.message[line_end]))) {
        ++line_end;
    }
    if (line_end == line_begin || line_end >= diagnostic.message.size() || diagnostic.message[line_end] != ':') {
        return diagnostic;
    }

    std::size_t file_begin = diagnostic.message.rfind('\n', lua_ext);
    file_begin = file_begin == std::string::npos ? 0 : file_begin + 1;
    const std::size_t prefix = diagnostic.message.find(": ", file_begin);
    if (prefix != std::string::npos && prefix < lua_ext) {
        file_begin = prefix + 2;
    }
    diagnostic.file = diagnostic.message.substr(file_begin, line_begin - 1 - file_begin);
    diagnostic.line = static_cast<i32>(std::stoi(diagnostic.message.substr(line_begin, line_end - line_begin)));
    return diagnostic;
}

std::string dependency_key(const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::path absolute = std::filesystem::absolute(path, error);
    return (error ? path : absolute).lexically_normal().string();
}

const ComponentFieldSnapshot* find_snapshot_field(const ComponentSnapshot& snapshot, std::string_view name) {
    const auto found = std::ranges::find_if(snapshot.fields, [&](const ComponentFieldSnapshot& field) {
        return field.name == name;
    });
    return found == snapshot.fields.end() ? nullptr : &*found;
}

f32 snapshot_f32(const ComponentSnapshot& snapshot, std::string_view name, f32 fallback = 0.0f) {
    const ComponentFieldSnapshot* field = find_snapshot_field(snapshot, name);
    if (!field) {
        return fallback;
    }
    if (const f32* value = std::get_if<f32>(&field->value)) {
        return *value;
    }
    if (const f64* value = std::get_if<f64>(&field->value)) {
        return static_cast<f32>(*value);
    }
    return fallback;
}

bool snapshot_bool(const ComponentSnapshot& snapshot, std::string_view name, bool fallback = false) {
    const ComponentFieldSnapshot* field = find_snapshot_field(snapshot, name);
    if (!field) {
        return fallback;
    }
    if (const bool* value = std::get_if<bool>(&field->value)) {
        return *value;
    }
    return fallback;
}

i32 snapshot_i32(const ComponentSnapshot& snapshot, std::string_view name, i32 fallback = 0) {
    const ComponentFieldSnapshot* field = find_snapshot_field(snapshot, name);
    if (!field) {
        return fallback;
    }
    if (const i32* value = std::get_if<i32>(&field->value)) {
        return *value;
    }
    return fallback;
}

void update_reflected_timers(EcsWorld& world, f32 dt) {
    const ComponentDescriptor* timer_descriptor = world.components().find("Timer");
    if (!timer_descriptor) {
        return;
    }

    const f32 step = std::max(0.0f, dt);
    EcsQueryPlan plan = world.build_query_plan({.all = {"Timer"}});
    if (!plan.valid) {
        return;
    }
    for (EcsEntity entity : world.query_entities(plan)) {
        std::optional<ComponentSnapshot> snapshot = world.components().snapshot(entity, timer_descriptor->id);
        if (!snapshot) {
            continue;
        }
        const bool repeating = snapshot_bool(*snapshot, "repeating");
        if (snapshot_bool(*snapshot, "finished") && !repeating) {
            continue;
        }

        f32 elapsed = snapshot_f32(*snapshot, "elapsed") + step;
        const f32 duration = std::max(snapshot_f32(*snapshot, "duration"), 0.001f);
        bool finished = false;
        i32 ticks = snapshot_i32(*snapshot, "ticks");
        while (elapsed >= duration) {
            ++ticks;
            finished = true;
            if (!repeating) {
                elapsed = duration;
                break;
            }
            elapsed -= duration;
        }

        world.components().patch_field(entity, timer_descriptor->id, "elapsed", elapsed);
        world.components().patch_field(entity, timer_descriptor->id, "finished", finished);
        world.components().patch_field(entity, timer_descriptor->id, "ticks", ticks);
    }
}

void update_reflected_lifetimes(EcsWorld& world, f32 dt) {
    const ComponentDescriptor* lifetime_descriptor = world.components().find("Lifetime");
    if (!lifetime_descriptor) {
        return;
    }

    const f32 step = std::max(0.0f, dt);
    EcsQueryPlan plan = world.build_query_plan({.all = {"Lifetime"}});
    if (!plan.valid) {
        return;
    }

    std::vector<EcsEntity> expired;
    for (EcsEntity entity : world.query_entities(plan)) {
        std::optional<ComponentSnapshot> snapshot = world.components().snapshot(entity, lifetime_descriptor->id);
        if (!snapshot) {
            continue;
        }
        const f32 remaining = snapshot_f32(*snapshot, "remaining") - step;
        world.components().patch_field(entity, lifetime_descriptor->id, "remaining", remaining);
        if (remaining <= 0.0f) {
            expired.push_back(entity);
        }
    }
    for (EcsEntity entity : expired) {
        if (entity.alive()) {
            entity.destroy();
        }
    }
}

} // namespace

const ScriptComponentRegistry::ComponentEntry* ScriptComponentRegistry::find(std::string_view name) const {
    const auto found = _components.find(std::string{name});
    return found == _components.end() ? nullptr : found->second.get();
}

ScriptScene::ScriptScene(ScriptSceneConfig config, EcsWorld* shared_world)
    : EcsScene(shared_world),
      _config(std::move(config)) {
    render_config().enabled = _config.render_enabled;
    render_config().clear_color = _config.clear_color;
    render_config().profile = _config.profile;

    if (_config.before_load) {
        _config.before_load(*this);
    }
    ecs().component<ScriptOwned>("ScriptOwned");

    load_script(false);
    refresh_write_times();
}

ScriptScene::~ScriptScene() {
    clear_script_systems(ecs());
}

void ScriptScene::on_enter(SceneContext& ctx) {
    if (_script && !_script->call_on_enter(ctx)) {
        set_script_error(_script->last_error(), "on_enter");
        KIN_LOG_ERROR_F("script", _last_error, (LogFields{{.name = "path", .value = resolved_script_path().string()}}));
    }
}

void ScriptScene::on_exit(SceneContext& ctx) {
    if (_script && !_script->call_on_exit(ctx)) {
        set_script_error(_script->last_error(), "on_exit");
        KIN_LOG_ERROR_F("script", _last_error, (LogFields{{.name = "path", .value = resolved_script_path().string()}}));
    }
}

void ScriptScene::update(SceneContext& ctx) {
    if (_config.before_update) {
        _config.before_update(*this, ctx);
    }
    poll_hot_reload();
    update_reflected_timers(ecs(), ctx.dt);
    update_reflected_lifetimes(ecs(), ctx.dt);
    if (!ecs().run_frame(ctx.dt)) {
        set_script_error(ecs().systems().last_error(), "system");
        KIN_LOG_ERROR_F("script", _last_error, (LogFields{{.name = "path", .value = resolved_script_path().string()}}));
    }
    if (_script && !_script->call_update(ctx)) {
        set_script_error(_script->last_error(), "update");
        KIN_LOG_ERROR_F("script", _last_error, (LogFields{{.name = "path", .value = resolved_script_path().string()}}));
    }
}

void ScriptScene::collect_actions(InputActionContext& context) const {
    if (_script && !_script->call_collect_actions(context)) {
        KIN_LOG_ERROR_F("script", _script->last_error(), (LogFields{{.name = "path", .value = resolved_script_path().string()}}));
    }
}

void ScriptScene::write_report(JsonWriter& json) const {
    if (_script && !_script->call_write_report(json)) {
        KIN_LOG_ERROR_F("script", _script->last_error(), (LogFields{{.name = "path", .value = resolved_script_path().string()}}));
    }
    if (!_last_diagnostic.message.empty()) {
        json.key("script_error").begin_object();
        json.field("message", std::string_view{_last_diagnostic.message});
        if (!_last_diagnostic.file.empty()) {
            json.field("file", std::string_view{_last_diagnostic.file});
        }
        if (_last_diagnostic.line > 0) {
            json.field("line", _last_diagnostic.line);
        }
        if (!_last_diagnostic.function_name.empty()) {
            json.field("function", std::string_view{_last_diagnostic.function_name});
        }
        if (!_last_diagnostic.system_id.empty()) {
            json.field("system", std::string_view{_last_diagnostic.system_id});
        }
        if (_last_diagnostic.entity_id != 0) {
            json.field("entity", _last_diagnostic.entity_id);
        }
        json.end_object();
    }
}

std::filesystem::path ScriptScene::resolved_script_path() const {
    return _config.asset_root / _config.script_path;
}

void ScriptScene::set_script_path(std::filesystem::path path) {
    _config.script_path = std::move(path);
    refresh_write_times();
}

void ScriptScene::clear_script_entities() {
    std::vector<EcsEntity> entities;
    ecs().query<ScriptOwned>().each_entity([&](EcsEntity entity, const ScriptOwned&) {
        entities.push_back(entity);
    });

    for (EcsEntity entity : entities) {
        entity.destroy();
    }

    mark_render_cache_dirty("script clear");
    KIN_LOG_DEBUG_F("script",
                    "script entities cleared",
                    (LogFields{
                        {.name = "scene", .value = _config.name},
                        {.name = "path", .value = resolved_script_path().string()},
                        {.name = "count", .value = std::to_string(entities.size())},
                    }));
}

bool ScriptScene::reload_script() {
    KIN_LOG_INFO_F("script",
                   "script reload requested",
                   (LogFields{
                       {.name = "scene", .value = _config.name},
                       {.name = "path", .value = resolved_script_path().string()},
                       {.name = "clear_entities", .value = _config.reload_policy == ScriptReloadPolicy::ClearScriptEntities ? "true" : "false"},
                       {.name = "hot_reload", .value = _config.hot_reload ? "true" : "false"},
                   }));
    return load_script(_config.reload_policy == ScriptReloadPolicy::ClearScriptEntities);
}

bool ScriptScene::load_script(bool clear_entities) {
    KIN_LOG_INFO_F("script",
                   "script load started",
                   (LogFields{
                       {.name = "scene", .value = _config.name},
                       {.name = "path", .value = resolved_script_path().string()},
                       {.name = "clear_entities", .value = clear_entities ? "true" : "false"},
                       {.name = "hot_reload", .value = _config.hot_reload ? "true" : "false"},
                   }));
    auto next = std::make_unique<ScriptEngine>(_config.bind);
    if (!next->load_file(_config.asset_root, _config.script_path)) {
        set_script_error(next->last_error(), "load");
        _script_loaded = _script != nullptr;
        KIN_LOG_ERROR_F("script", _last_error, (LogFields{{.name = "path", .value = resolved_script_path().string()}}));
        return false;
    }

    clear_script_systems(ecs());
    if (clear_entities) {
        clear_script_entities();
    }

    _script = std::move(next);
    _script_loaded = true;
    if (!_script->call_on_load(*this)) {
        set_script_error(_script->last_error(), "on_load");
        KIN_LOG_ERROR_F("script", _last_error, (LogFields{{.name = "path", .value = resolved_script_path().string()}}));
        return false;
    }

    clear_script_error();
    refresh_write_times();
    KIN_LOG_INFO_F("script",
                   "script load succeeded",
                   (LogFields{
                       {.name = "scene", .value = _config.name},
                       {.name = "path", .value = resolved_script_path().string()},
                       {.name = "clear_entities", .value = clear_entities ? "true" : "false"},
                       {.name = "hot_reload", .value = _config.hot_reload ? "true" : "false"},
                   }));
    return true;
}

void ScriptScene::poll_hot_reload() {
    if (!_config.hot_reload) {
        return;
    }

    if (_script_write_times.empty()) {
        refresh_write_times();
        return;
    }

    for (const auto& [path, previous] : _script_write_times) {
        std::error_code error;
        const std::filesystem::file_time_type current = std::filesystem::last_write_time(path, error);
        if (!error && current != previous) {
            reload_script();
            return;
        }
    }
}

void ScriptScene::refresh_write_times() {
    _script_write_times.clear();
    std::vector<std::filesystem::path> dependencies;
    if (_script) {
        dependencies = _script->script_dependencies();
    }
    if (dependencies.empty()) {
        dependencies.push_back(resolved_script_path());
    }

    for (const std::filesystem::path& path : dependencies) {
        std::error_code error;
        const std::filesystem::file_time_type write_time = std::filesystem::last_write_time(path, error);
        if (!error) {
            _script_write_times[dependency_key(path)] = write_time;
        }
    }
}

void ScriptScene::set_script_error(std::string error, std::string function_name) {
    _last_error = std::move(error);
    _last_diagnostic = parse_script_diagnostic(_last_error, std::move(function_name));
}

void ScriptScene::clear_script_error() {
    _last_error.clear();
    _last_diagnostic = {};
}

} // namespace kin
