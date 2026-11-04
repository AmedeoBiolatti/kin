#pragma once
#ifdef KIN_EXPERIMENTAL_3D
#include <kin/render3d/errors3d.hpp>
#endif

#include <filesystem>
#include <kin/runtime/development.hpp>
#include <kin/scene/scene_asset.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace kin {

class ScriptScene;
#ifdef KIN_EXPERIMENTAL_3D
class Assets3D;
struct SceneBindingOptions3D;
#endif

enum class SceneAssetRuntimeChange {
    None,
    SceneAsset,
    PrefabAsset,
};

struct SceneAssetLoadLimits {
    u64 max_source_bytes = 0; // per scene/prefab source; zero means unlimited
    std::size_t max_entities = 0; // expanded authored count; zero means unlimited
    std::size_t max_prefab_depth=8,max_prefab_sources=128;
};

class SceneAssetRuntime {
public:
    bool load(const std::filesystem::path& scene_path,
              const std::filesystem::path& asset_root,
              const EcsComponentRegistry& components,
              SceneAssetLoadLimits limits = {});

    bool instantiate(EcsWorld& world, SceneInstantiateOptions options = {});
    // Track an already instantiated, validated authored scene (e.g. a preload).
    void track_instance(EcsWorld& world, std::filesystem::path path, std::filesystem::path root, SceneAsset definition,
                        SceneInstance instance);
    std::unique_ptr<DevelopmentChange> prepare_patch(EcsWorld& world, const std::filesystem::path& revision_root,
                                                     std::string& error,
                                                     std::function<bool(EcsWorld&, std::string&)> validate = {});
#ifdef KIN_EXPERIMENTAL_3D
    Status3D instantiate3d(EcsWorld& world, Assets3D& assets, const SceneBindingOptions3D& options);
#endif
    void clear(EcsWorld& world);

    SceneAssetRuntimeChange poll_changes() const;
#if KIN_ENABLE_SCRIPTING
    bool reload(ScriptScene& scene);
    bool reload_if_changed(ScriptScene& scene);
#endif

    const SceneAsset* scene() const { return _loaded ? &_scene : nullptr; }
    const SceneInstance& instance() const { return _instance; }
    const std::filesystem::path& scene_path() const { return _scene_path; }
    const std::filesystem::path& asset_root() const { return _asset_root; }
    const std::vector<std::string>& diagnostics() const { return _diagnostics; }
    bool loaded() const { return _loaded; }
    std::size_t entity_count() const { return _entity_count; }
    const std::vector<std::filesystem::path>& prefab_sources()const{return _prefab_sources;}

private:
#ifdef KIN_EXPERIMENTAL_3D
    bool instantiate3d_operation(EcsWorld& world, Assets3D& assets, const SceneBindingOptions3D& options);
#endif
    std::filesystem::path resolve_asset(const std::filesystem::path& relative) const;
    void refresh_watches();
    bool prefab_assets_valid(const SceneAsset& scene, const EcsComponentRegistry& components,
                             const std::filesystem::path& root, SceneAssetLoadLimits limits, std::size_t& count,std::vector<std::filesystem::path>& sources);

    std::filesystem::path _scene_path;
    std::filesystem::path _asset_root;
    SceneAsset _scene;
    SceneInstance _instance;
    SceneDocument _authored;
    std::vector<std::string> _diagnostics;
    std::unordered_map<std::string, std::filesystem::file_time_type> _write_times;
    bool _loaded = false;
    SceneAssetLoadLimits _limits;
    std::vector<std::filesystem::path> _prefab_sources;
    std::size_t _entity_count = 0;
};

} // namespace kin
