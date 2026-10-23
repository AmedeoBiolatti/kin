// Optional scene/editor integration lives above the engine base. Keep the
// existing reload entry points without introducing a dependency back to Lua.
#include <kin/ecs/editor.hpp>
#include <kin/scene/scene_asset_runtime.hpp>
#include <kin/scripting/script_scene.hpp>
#include <kin/platform/log.hpp>
#include <kin/assets/content.hpp>

namespace kin {
namespace {
bool source_within_limit(const std::filesystem::path& path, SceneAssetLoadLimits limits) {
    if (!limits.max_source_bytes) return true;
    const auto size = content_file_size(path);
    return size && *size <= limits.max_source_bytes;
}
}

ReloadResult EcsReloadCoordinator::reload_script_scene(ScriptScene& scene) {
    if (!scene.reload_script()) {
        return {.ok = false, .diagnostics = {scene.last_script_error()}};
    }
    return {};
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

} // namespace kin
