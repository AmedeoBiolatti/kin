#include <kin/scene/scene_asset_runtime.hpp>

#include <kin/platform/log.hpp>
#include <kin/assets/content.hpp>
#include <kin/prefab/prefab_asset.hpp>
#if KIN_ENABLE_SCRIPTING
#include <kin/scripting/script_scene.hpp>
#endif

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

bool source_within_limit(const std::filesystem::path& path, SceneAssetLoadLimits limits) {
    if (!limits.max_source_bytes) return true;
    const auto size = content_file_size(path);
    return size && *size <= limits.max_source_bytes;
}

} // namespace

bool SceneAssetRuntime::load(const std::filesystem::path& scene_path,
                             const std::filesystem::path& asset_root,
                             const EcsComponentRegistry& components, SceneAssetLoadLimits limits) {
    _diagnostics.clear();
    if (!source_within_limit(scene_path, limits)) {
        _diagnostics = {"scene source missing or exceeds byte limit: " + scene_path.string()};
        return false;
    }

    SceneLoadResult loaded = load_scene_asset(scene_path, components);
    if (!loaded.ok()) {
        _diagnostics = std::move(loaded.diagnostics);
        return false;
    }
    std::size_t count = 0;
    std::vector<std::filesystem::path> sources;
    if (!prefab_assets_valid(*loaded.scene, components, asset_root, limits, count,sources)) {
        return false;
    }

    _scene_path = scene_path;
    _asset_root = asset_root;
    _limits = limits;
    _entity_count = count;
    _prefab_sources=std::move(sources);
    _scene = std::move(*loaded.scene);
    _loaded = true;
    refresh_watches();
    return true;
}

bool SceneAssetRuntime::instantiate(EcsWorld& world, SceneInstantiateOptions options) {
    _diagnostics.clear();
    if (!_loaded) {
        _diagnostics.push_back("scene asset runtime is not loaded");
        return false;
    }
    if (!_instance.by_authored_id.empty()) {
        _diagnostics = {"clear the previous scene instance before instantiating again"};
        return false;
    }

    auto instance = instantiate_scene_asset(world, _scene, _asset_root, world.components(),{.content_root=_asset_root,.max_source_bytes=_limits.max_source_bytes,.max_entities=_limits.max_entities,.max_depth=_limits.max_prefab_depth,.max_sources=_limits.max_prefab_sources},options);
    if (!instance.ok()) {
        _diagnostics = std::move(instance.diagnostics);
        return false;
    }
    _instance = std::move(instance);
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
    {
        const auto current = write_time(_scene_path);
        const auto found = _write_times.find(scene_key);
        if (found != _write_times.end() && (!current || found->second != *current)) {
            return SceneAssetRuntimeChange::SceneAsset;
        }
    }

    for (const auto& path : _prefab_sources) {
        const auto key = watch_key(path);
        {
            const auto current = write_time(path);
            const auto found = _write_times.find(key);
            if (found != _write_times.end() && (!current || found->second != *current)) {
                return SceneAssetRuntimeChange::PrefabAsset;
            }
        }
    }

    return SceneAssetRuntimeChange::None;
}

#if KIN_ENABLE_SCRIPTING
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

    if (!source_within_limit(_scene_path, _limits)) {
        _diagnostics = {"scene source missing or exceeds byte limit: " + _scene_path.string()};
        return false;
    }
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
    std::size_t count = 0;
    std::vector<std::filesystem::path> sources;
    if (!prefab_assets_valid(*loaded.scene, components, _asset_root, _limits, count,sources)) {
        KIN_LOG_ERROR_F("scene", "scene hot reload prefab validation failed", (LogFields{{.name = "path", .value = _scene_path.string()}}));
        return false;
    }

    scene.clear_script_entities();
    clear(scene.ecs());
    _scene = std::move(*loaded.scene);
    _entity_count = count;
    _prefab_sources=std::move(sources);
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

#endif

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
    for (const auto& path : _prefab_sources) {
        if (const auto current = write_time(path)) {
            _write_times[watch_key(path)] = *current;
        }
    }
}

bool SceneAssetRuntime::prefab_assets_valid(const SceneAsset& scene, const EcsComponentRegistry& components,
                                           const std::filesystem::path& root, SceneAssetLoadLimits limits, std::size_t& count,std::vector<std::filesystem::path>& files) {
    _diagnostics.clear();
    count = scene.entities.size();
    std::unordered_map<std::string, PrefabLoadResult> sources;
    if (limits.max_entities && count > limits.max_entities) {
        _diagnostics = {"scene exceeds expanded entity limit"}; return false;
    }
    for (const ScenePrefabInstanceAsset& prefab : scene.prefabs) {
        const auto path = (root / prefab.asset).lexically_normal();
        if (!source_within_limit(path, limits)) {
            _diagnostics = {"prefab source missing or exceeds byte limit: " + path.string()}; return false;
        }
        auto [cached, inserted] = sources.try_emplace(path.string());
        if (inserted) cached->second = load_prefab_asset(path, components,{.content_root=root,.max_source_bytes=limits.max_source_bytes,.max_entities=limits.max_entities,.max_depth=limits.max_prefab_depth,.max_sources=limits.max_prefab_sources});
        const auto& loaded = cached->second;
        if (!loaded.ok()) {
            _diagnostics.push_back("failed to load prefab '" + prefab.asset.string() + "'");
            _diagnostics.insert(_diagnostics.end(), loaded.diagnostics.begin(), loaded.diagnostics.end());
            return false;
        }
        auto errors = validate_prefab_overrides(*loaded.prefab, prefab.overrides, components);
        if (!errors.empty()) { _diagnostics = std::move(errors); return false; }
        auto edited=apply_prefab_overrides(*loaded.prefab,prefab.overrides,components);
        if(!edited.ok()){_diagnostics=std::move(edited.diagnostics);return false;}
        if (limits.max_entities && edited.prefab->entities.size() > limits.max_entities - count) {
            _diagnostics = {"scene exceeds expanded entity limit"}; return false;
        }
        count += edited.prefab->entities.size();
        files.insert(files.end(),loaded.prefab->source_files.begin(),loaded.prefab->source_files.end());
        if(files.size()>limits.max_prefab_sources){std::ranges::sort(files);files.erase(std::unique(files.begin(),files.end()),files.end());if(files.size()>limits.max_prefab_sources){_diagnostics={"scene exceeds prefab source limit"};return false;}}
    }
    return true;
}

} // namespace kin
