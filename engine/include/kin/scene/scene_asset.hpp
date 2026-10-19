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
    // Immutable reflected defaults, shared by instances of one loaded source.
    // Contains authored values only; no GPU/physics/animation runtime objects.
    std::shared_ptr<const SceneDocument> source;
};

struct PrefabEntityComponent {
    std::string instance_id;
    std::string authored_entity_id;
};

using ScenePrefabInstanceAsset=PrefabReferenceAsset;

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
    bool preserve_prefabs = true;
};

struct SceneSaveResult {
    std::optional<SceneAsset> scene;
    std::vector<std::string> diagnostics;
    bool ok() const { return scene.has_value() && diagnostics.empty(); }
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
struct SceneInstantiateOptions {
    bool update_scene_header = true;
};
// Explicit header commit for additive loaders that finish binding after
// instantiation. Changes no gameplay entities.
void apply_scene_header(EcsWorld& world, const SceneAsset& scene);
SceneInstance instantiate_scene_asset(EcsWorld& world,
                                      const SceneAsset& scene,
                                      const std::filesystem::path& asset_root,
                                      EcsComponentRegistry& components,PrefabAssetLoadOptions limits={},
                                      SceneInstantiateOptions options={});
// Adopt an authored child into a linked instance for structural saves. Local ID
// is stable; the global authored ID is instance_id + "." + local_id. Does not
// reparent it; its parent must already belong to this same instance.
bool link_prefab_entity(EcsWorld& world,EcsEntity instance_root,EcsEntity child,std::string local_id);
// Editor removal: preserve private copies of all reflected native/data values,
// detach this entity's template inheritance, then remove the component. Plain
// remove<T>() still means "remove my override", exposing inherited defaults.
// Explicitly rebind/rebuild affected render/physics state after structural edits.
bool remove_prefab_component(EcsWorld& world,EcsEntity entity,std::string_view component);
SceneAsset scene_asset_from_world(EcsWorld& world, const SceneSaveOptions& options = {});
// Checked save preparation. Preserves prefab references and minimal field
// field/structure overrides. Unsupported edits return diagnostics; explicitly set
// preserve_prefabs=false to flatten. The convenience function above throws
// std::runtime_error on a failed build, so it never silently discards edits.
SceneSaveResult build_scene_asset(EcsWorld& world, const SceneSaveOptions& options = {});

} // namespace kin
