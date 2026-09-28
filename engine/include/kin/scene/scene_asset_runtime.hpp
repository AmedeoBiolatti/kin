#pragma once

#include <kin/scene/scene_asset.hpp>

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace kin {

class ScriptScene;

enum class SceneAssetRuntimeChange {
    None,
    SceneAsset,
    PrefabAsset,
};

class SceneAssetRuntime {
public:
    bool load(const std::filesystem::path& scene_path,
              const std::filesystem::path& asset_root,
              const EcsComponentRegistry& components);

    bool instantiate(EcsWorld& world);
    void clear(EcsWorld& world);

    SceneAssetRuntimeChange poll_changes() const;
    bool reload(ScriptScene& scene);
    bool reload_if_changed(ScriptScene& scene);

    const SceneAsset* scene() const { return _loaded ? &_scene : nullptr; }
    const SceneInstance& instance() const { return _instance; }
    const std::filesystem::path& scene_path() const { return _scene_path; }
    const std::filesystem::path& asset_root() const { return _asset_root; }
    const std::vector<std::string>& diagnostics() const { return _diagnostics; }
    bool loaded() const { return _loaded; }

private:
    std::filesystem::path resolve_asset(const std::filesystem::path& relative) const;
    void refresh_watches();
    bool prefab_assets_valid(const SceneAsset& scene, const EcsComponentRegistry& components);

    std::filesystem::path _scene_path;
    std::filesystem::path _asset_root;
    SceneAsset _scene;
    SceneInstance _instance;
    std::vector<std::string> _diagnostics;
    std::unordered_map<std::string, std::filesystem::file_time_type> _write_times;
    bool _loaded = false;
};

} // namespace kin
