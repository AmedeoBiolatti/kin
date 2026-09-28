#include <kin/scene/scene_asset_runtime.hpp>

#include <kin/platform/log.hpp>
#include <kin/prefab/prefab_asset.hpp>
#include <kin/scripting/script_scene.hpp>

#include <algorithm>
#include <optional>
#include <system_error>
#include <unordered_set>

namespace kin {
namespace {

std::string watch_key(const std::filesystem::path& path) {
    return path.lexically_normal().string();
}

std::optional<std::filesystem::file_time_type> write_time(const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::file_time_type value = std::filesystem::last_write_time(path, error);
    if (error) {
        return std::nullopt;
    }
    return value;
}

} // namespace

bool SceneAssetRuntime::load(const std::filesystem::path& scene_path,
                             const std::filesystem::path& asset_root,
                             const EcsComponentRegistry& components) {
    _diagnostics.clear();

    SceneLoadResult loaded = load_scene_asset(scene_path, components);
    if (!loaded.ok()) {
        _diagnostics = std::move(loaded.diagnostics);
        return false;
    }
    _scene_path = scene_path;
    _asset_root = asset_root;
    if (!prefab_assets_valid(*loaded.scene, components)) {
        return false;
    }

    _scene = std::move(*loaded.scene);
    _loaded = true;
    refresh_watches();
    return true;
}

bool SceneAssetRuntime::instantiate(EcsWorld& world) {
    _diagnostics.clear();
    if (!_loaded) {
        _diagnostics.push_back("scene asset runtime is not loaded");
        return false;
    }

    _instance = instantiate_scene_asset(world, _scene, _asset_root, world.components());
    if (!_instance.ok()) {
        _diagnostics = _instance.diagnostics;
        return false;
    }
    return true;
}

void SceneAssetRuntime::clear(EcsWorld& world) {
    std::unordered_map<EcsId, EcsEntity> managed;
    for (const auto& [authored_id, entity] : _instance.by_authored_id) {
        (void)authored_id;
        if (entity) {
            managed[entity.id()] = entity;
        }
    }

    std::vector<EcsEntity> roots;
    roots.reserve(managed.size());
    for (const auto& [id, entity] : managed) {
        (void)id;
        const EcsEntity parent = entity.parent();
        if (!parent || !managed.contains(parent.id())) {
            roots.push_back(entity);
        }
    }

    std::ranges::sort(roots, [](EcsEntity a, EcsEntity b) {
        return a.id() > b.id();
    });
    for (EcsEntity entity : roots) {
        if (entity) {
            world.entities().destroy(entity);
        }
    }

    _instance = {};
}

SceneAssetRuntimeChange SceneAssetRuntime::poll_changes() const {
    if (!_loaded) {
        return SceneAssetRuntimeChange::None;
    }

    const auto scene_key = watch_key(_scene_path);
    if (const auto current = write_time(_scene_path)) {
        const auto found = _write_times.find(scene_key);
        if (found == _write_times.end() || found->second != *current) {
            return SceneAssetRuntimeChange::SceneAsset;
        }
    }

    for (const ScenePrefabInstanceAsset& prefab : _scene.prefabs) {
        const std::filesystem::path path = resolve_asset(prefab.asset);
        const auto key = watch_key(path);
        if (const auto current = write_time(path)) {
            const auto found = _write_times.find(key);
            if (found == _write_times.end() || found->second != *current) {
                return SceneAssetRuntimeChange::PrefabAsset;
            }
        }
    }

    return SceneAssetRuntimeChange::None;
}

bool SceneAssetRuntime::reload_if_changed(ScriptScene& scene) {
    const SceneAssetRuntimeChange change = poll_changes();
    if (change == SceneAssetRuntimeChange::None) {
        return false;
    }
    const bool reloaded = reload(scene);
    if (reloaded) {
        scene.mark_render_cache_dirty(change == SceneAssetRuntimeChange::SceneAsset ? "scene asset hot reload" : "prefab asset hot reload");
    }
    return reloaded;
}

bool SceneAssetRuntime::reload(ScriptScene& scene) {
    EcsComponentRegistry& components = scene.ecs().components();

    SceneLoadResult loaded = load_scene_asset(_scene_path, components);
    if (!loaded.ok()) {
        _diagnostics = std::move(loaded.diagnostics);
        KIN_LOG_ERROR_F("scene", "scene hot reload failed", (LogFields{{.name = "path", .value = _scene_path.string()}}));
        return false;
    }
    if (loaded.scene->script.empty()) {
        _diagnostics = {_scene_path.string() + ": scene requires a script"};
        KIN_LOG_ERROR_F("scene", _diagnostics.front(), (LogFields{{.name = "path", .value = _scene_path.string()}}));
        return false;
    }
    if (!prefab_assets_valid(*loaded.scene, components)) {
        KIN_LOG_ERROR_F("scene", "scene hot reload prefab validation failed", (LogFields{{.name = "path", .value = _scene_path.string()}}));
        return false;
    }

    scene.clear_script_entities();
    clear(scene.ecs());
    _scene = std::move(*loaded.scene);
    refresh_watches();
    scene.set_script_path(_scene.script);
    scene.render_config().clear_color = _scene.clear_color;
    if (!instantiate(scene.ecs())) {
        return false;
    }
    scene.mark_render_cache_dirty("scene asset reload");
    scene.reload_script();
    return true;
}

std::filesystem::path SceneAssetRuntime::resolve_asset(const std::filesystem::path& relative) const {
    if (relative.is_absolute()) {
        return relative;
    }
    return _asset_root / relative;
}

void SceneAssetRuntime::refresh_watches() {
    _write_times.clear();
    if (const auto current = write_time(_scene_path)) {
        _write_times[watch_key(_scene_path)] = *current;
    }
    for (const ScenePrefabInstanceAsset& prefab : _scene.prefabs) {
        const std::filesystem::path path = resolve_asset(prefab.asset);
        if (const auto current = write_time(path)) {
            _write_times[watch_key(path)] = *current;
        }
    }
}

bool SceneAssetRuntime::prefab_assets_valid(const SceneAsset& scene, const EcsComponentRegistry& components) {
    _diagnostics.clear();
    for (const ScenePrefabInstanceAsset& prefab : scene.prefabs) {
        PrefabLoadResult loaded = load_prefab_asset(resolve_asset(prefab.asset), components);
        if (!loaded.ok()) {
            _diagnostics.push_back("failed to load prefab '" + prefab.asset.string() + "'");
            _diagnostics.insert(_diagnostics.end(), loaded.diagnostics.begin(), loaded.diagnostics.end());
            return false;
        }
    }
    return true;
}

} // namespace kin
