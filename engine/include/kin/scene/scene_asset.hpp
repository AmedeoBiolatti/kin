#pragma once

#include <kin/ecs/component.hpp>
#include <kin/prefab/prefab_asset.hpp>

#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kin {

struct PrefabInstanceComponent {
    std::string instance_id;
    std::string prefab_id;
    std::string asset_path;
};

struct PrefabEntityComponent {
    std::string instance_id;
    std::string authored_entity_id;
};

struct ScenePrefabInstanceAsset {
    std::string id;
    std::filesystem::path asset;
    std::string name;
    std::string parent;
    Vec2f pos{};
    PrefabOverrideSet overrides;
};

struct SceneAsset {
    std::string schema = "kin.scene/1";
    std::string name = "Scene";
    std::filesystem::path script;
    Color clear_color = Color::rgb(0, 0, 0);
    std::vector<SceneEntityDocument> entities;
    std::vector<ScenePrefabInstanceAsset> prefabs;
};

struct SceneLoadResult {
    std::optional<SceneAsset> scene;
    std::vector<std::string> diagnostics;

    bool ok() const { return scene.has_value() && diagnostics.empty(); }
};

struct SceneInstance {
    std::vector<EcsEntity> roots;
    std::unordered_map<std::string, EcsEntity> by_authored_id;
    std::unordered_map<std::string, PrefabInstance> prefab_instances;
    std::vector<std::string> diagnostics;

    bool ok() const { return diagnostics.empty(); }
};

struct SceneSaveOptions {
    bool include_runtime_entities = false;
};

void register_builtin_editor_components(EcsWorld& world);
void register_scene_metadata_components(EcsWorld& world);

SceneLoadResult load_scene_asset(const std::filesystem::path& path,
                                 const EcsComponentRegistry& components);
SceneLoadResult parse_scene_asset(std::string_view text,
                                  const EcsComponentRegistry& components,
                                  std::string_view source = {});
bool save_scene_asset(const SceneAsset& scene, const std::filesystem::path& path);
void write_scene_asset(std::ostream& out, const SceneAsset& scene);

std::vector<std::string> validate_scene_asset(const SceneAsset& scene,
                                              const EcsComponentRegistry& components);
SceneInstance instantiate_scene_asset(EcsWorld& world,
                                      const SceneAsset& scene,
                                      const std::filesystem::path& asset_root,
                                      EcsComponentRegistry& components);
SceneAsset scene_asset_from_world(EcsWorld& world, const SceneSaveOptions& options = {});

} // namespace kin
