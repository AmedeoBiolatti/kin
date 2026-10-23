#pragma once

#include <kin/ecs/component.hpp>
#include <kin/prefab/prefab_registry.hpp>

#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kin {

struct PrefabFieldAsset {
    std::string name;
    ComponentFieldValue value = false;
};

struct PrefabComponentAsset {
    std::string name;
    std::vector<PrefabFieldAsset> fields;
};

struct PrefabEntityAsset {
    std::string id;
    std::string name;
    std::string parent;
    std::vector<PrefabComponentAsset> components;
};

struct PrefabOverrideSet {
    std::unordered_map<std::string, std::vector<PrefabComponentAsset>> entities;
    std::vector<PrefabEntityAsset> add_entities;
    std::vector<std::string> remove_entities; // subtrees, after reparenting
    std::unordered_map<std::string,std::vector<std::string>> remove_components;
    std::unordered_map<std::string,std::string> parents,names;
    bool structural() const {return !add_entities.empty()||!remove_entities.empty()||!remove_components.empty()||!parents.empty()||!names.empty();}
};

// Shared by scenes and composed prefabs; paths use the supplied content root.
struct PrefabReferenceAsset {
    std::string id;
    std::filesystem::path asset;
    std::string name;
    std::string parent;
    Vec2f pos{};
    PrefabOverrideSet overrides;
};
struct PrefabAsset {
    std::string schema = "kin.prefab/1";
    std::string id;
    std::string name;
    std::string root;
    std::vector<PrefabEntityAsset> entities;
    std::vector<PrefabReferenceAsset> prefabs;
    // CPU-only load products. Writing preserves the declared composition links.
    std::shared_ptr<const PrefabAsset> expanded;
    std::vector<std::filesystem::path> source_files;
};
struct PrefabAssetLoadOptions {
    std::filesystem::path content_root; // empty: the outer file's directory
    u64 max_source_bytes=8u*1024u*1024u;
    std::size_t max_entities=10000,max_depth=8,max_sources=128;
};
const PrefabAsset& resolved_prefab_asset(const PrefabAsset& prefab);

struct PrefabInstance {
    std::string instance_id;
    std::string source_prefab_id;
    EcsEntity root;
    std::vector<EcsEntity> entities;
    std::unordered_map<std::string, EcsEntity> by_authored_id;
    std::vector<std::string> diagnostics;
    std::shared_ptr<const SceneDocument> source; // reflected source defaults, no runtime resources

    bool ok() const { return root && diagnostics.empty(); }
};

using PrefabInstanceId = std::string;

struct PrefabInstanceRecord {
    PrefabInstanceId id;
    std::string prefab_id;
    EcsEntity root;
    std::vector<EcsEntity> entities;
    std::unordered_map<std::string, EcsEntity> by_authored_id;
    PrefabOverrideSet overrides;
};

struct PrefabLoadResult {
    std::optional<PrefabAsset> prefab;
    std::vector<std::string> diagnostics;

    bool ok() const { return prefab.has_value() && diagnostics.empty(); }
};

struct PrefabBuildResult {
    std::optional<PrefabAsset> prefab;
    std::vector<std::string> diagnostics;

    bool ok() const { return prefab.has_value() && diagnostics.empty(); }
};

struct PrefabFromSceneOptions {
    std::string prefab_id;
    std::string name;
    std::string root;
};

PrefabLoadResult load_prefab_asset(const std::filesystem::path& path,
                                   const EcsComponentRegistry& components);
PrefabLoadResult load_prefab_asset(const std::filesystem::path& path,const EcsComponentRegistry& components,
                                   PrefabAssetLoadOptions options);
PrefabLoadResult parse_prefab_asset(std::string_view text,
                                    const EcsComponentRegistry& components,
                                    std::string_view source = {});
bool save_prefab_asset(const PrefabAsset& prefab, const std::filesystem::path& path);
void write_prefab_asset(std::ostream& out, const PrefabAsset& prefab);

std::vector<std::string> validate_prefab_asset(const PrefabAsset& prefab,
                                               const EcsComponentRegistry& components);
std::vector<std::string> validate_prefab_overrides(const PrefabAsset& prefab,
                                                   const PrefabOverrideSet& overrides,
                                                   const EcsComponentRegistry& components);
// Pure CPU recipe edit. Field patches may add components; remove+patch resets
// one to its defaults. Input is unchanged on failure. Resolve composition first.
PrefabBuildResult apply_prefab_overrides(const PrefabAsset& prefab,const PrefabOverrideSet& overrides,
                                        const EcsComponentRegistry& components,bool include_fields=true);

PrefabBuildResult prefab_asset_from_scene_document(const SceneDocument& document,
                                                   const PrefabFromSceneOptions& options,
                                                   const EcsComponentRegistry& components);
SceneDocument scene_document_from_prefab_asset(const PrefabAsset& prefab);

PrefabInstance instantiate_prefab(EcsWorld& world,
                                  const PrefabAsset& prefab,
                                  const PrefabSpawn& spawn,
                                  EcsComponentRegistry& components,
                                  const PrefabOverrideSet& overrides = {});

class PrefabInstanceRegistry {
public:
    PrefabInstance instantiate(EcsWorld& world,
                               const PrefabAsset& prefab,
                               const PrefabSpawn& spawn,
                               EcsComponentRegistry& components,
                               const PrefabOverrideSet& overrides = {});

    const PrefabInstanceRecord* find(std::string_view id) const;
    std::vector<PrefabInstanceRecord> records() const;
    bool remove(std::string_view id);
    void clear();
    const std::string& last_error() const { return _last_error; }

private:
    std::string next_instance_id(const PrefabAsset& prefab);

    std::unordered_map<PrefabInstanceId, PrefabInstanceRecord> _records;
    u64 _next_instance_sequence = 1;
    std::string _last_error;
};

} // namespace kin
