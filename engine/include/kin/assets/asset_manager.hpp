#pragma once

#include <kin/assets/asset_handle.hpp>
#include <kin/assets/image.hpp>
#include <kin/platform/log.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <typeindex>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kin {

struct AnimationRegistryFragment;
struct AnimationStateMachine;
class AudioCatalog;
struct DialogueDocument;
struct GameInfo;
class InputMap;
class ParticleCatalog;
struct PrefabAsset;
class SpriteCatalog;
class TileMap;
struct SkeletonAsset2D;

enum class AssetType {
    Unknown,
    Image,
    Animation,
    AnimationStateMachine,
    GameInfo,
    InputMap,
    Particles,
    Script,
    SpriteCatalog,
    AudioCatalog,
    TileMap,
    Dialogue,
    Prefab,
    Scene,  // .kinscene
    Audio,  // a sound: .wav, .ogg, .mp3, .flac
    Font,   // .ttf, .otf
    Model,  // a 3D model: .glb, .gltf
    TileSet, // .kintileset: tiles maps share
    Shape,   // .svg: a vector shape (SVG-lite)
    Shader,  // .glsl (.frag.glsl), .spv: a fragment shader
    ColorLut, // .cube: a colour look (a LUT image is an Image)
    UiScreen, // .kinui: a UI screen
    UiTheme,  // .kintheme: a UI look
    Skeleton2D, // .kinskel: a 2D skeleton, weighted mesh and clips
    Animator3D,  // .kinanimator: a 3D animator's machine, blends, layers, actions, events and IK (kin.animator3d/1)
    Collider3D,  // .kincollider: a 3D collider recipe, a hull, a mesh or a compound (kin.collider3d/1)
    Materials3D, // .kinmaterials: changes to a 3D model's materials, and its variants
};

enum class AssetStatus {
    Discovered,
    Loaded,
    Missing,
};

struct AssetMetadata {
    std::string name;
    std::string path;
    AssetType type = AssetType::Unknown;
    AssetStatus status = AssetStatus::Discovered;
    std::uintmax_t size_bytes = 0;
    Vec2i dimensions{};
};

std::string_view asset_type_name(AssetType type);
std::string_view asset_status_name(AssetStatus status);
AssetType infer_asset_type(const std::filesystem::path& path);

struct AssetResidency {
    std::string key, path;
    u64 bytes = 0, last_access = 0;
    std::size_t external_references = 0;
    bool loaded = false, size_estimated = true;
    std::vector<std::string> retained_by; // root -> ... -> this entry
    std::vector<std::string> dependencies;
};
struct AssetMemoryReport {
    u64 resident_bytes = 0, budget_bytes = 0, pinned_bytes = 0;
    std::size_t evicted = 0;
    bool over_budget = false;
};
// Totals of residency rows: their payload, the part live owners retain, and
// whether the payload exceeds `budget_bytes` (0: no budget).
AssetMemoryReport summarize_residency(std::span<const AssetResidency> rows, u64 budget_bytes);
// Least recently used first, ties by key: the order budgets evict in.
bool least_recent_first(const AssetResidency& a, const AssetResidency& b);

class AssetManager {
public:
    struct Stats {
        std::size_t loaded_assets = 0;
        std::size_t registered_loaders = 0;
    };

    template<typename T>
    using Loader = std::function<T(const std::filesystem::path&)>;

    explicit AssetManager(std::filesystem::path root = ".");

    template<typename T>
    void register_loader(Loader<T> loader) {
        _loaders[typeid(T)] = std::make_shared<Loader<T>>(std::move(loader));
        KIN_LOG_DEBUG_F("asset",
                        "asset loader registered",
                        (LogFields{{.name = "type", .value = std::string{asset_type_name(asset_type_for<T>())}}}));
    }

    template <typename T> void register_memory_sizer(std::function<u64(const T&)> sizer) {
        if (!sizer)
            throw std::invalid_argument("memory sizer is empty");
        _memory_sizers[typeid(T)] = std::make_shared<std::function<u64(const T&)>>(std::move(sizer));
    }

    template<typename T>
    std::shared_ptr<const T> load(std::string_view relative_path) {
        return load_handle<T>(relative_path).shared();
    }

    template<typename T>
    AssetHandle<T> load_handle(std::string_view relative_path) {
        const std::string key = cache_key<T>(relative_path);
        if (auto found = _assets.find(key); found != _assets.end()) {
            auto storage = std::static_pointer_cast<AssetStorage<T>>(found->second);
            if (storage->is_loaded) {
                storage->last_access = ++_access_clock;
                KIN_LOG_DEBUG_F("asset",
                                "asset cache hit",
                                (LogFields{
                                    {.name = "path", .value = std::string{relative_path}},
                                    {.name = "type", .value = std::string{asset_type_name(asset_type_for<T>())}},
                                    {.name = "status", .value = "Loaded"},
                                }));
                return AssetHandle<T>{storage};
            }
            // Storage was reserved by an in-flight async load that has not
            // completed yet. Complete it synchronously so the sync API keeps
            // its blocking contract and both paths share one storage.
            return finish_load<T>(std::move(storage), key, relative_path);
        }

        return finish_load<T>(std::make_shared<AssetStorage<T>>(), key, relative_path);
    }

    // Reserves (or returns) the cache storage for an asset without running its
    // loader. The storage starts unloaded; the async AssetServer fills it on
    // completion, and a concurrent sync load_handle completes it in place.
    template<typename T>
    std::shared_ptr<AssetStorage<T>> reserve_storage(std::string_view relative_path) {
        const std::string key = cache_key<T>(relative_path);
        if (auto found = _assets.find(key); found != _assets.end()) {
            return std::static_pointer_cast<AssetStorage<T>>(found->second);
        }
        auto storage = std::make_shared<AssetStorage<T>>();
        _assets[key] = storage;
        return storage;
    }

    // Runs a registered loader without touching the cache. Safe to call from a
    // background thread as long as no loader is being registered concurrently
    // (loaders are registered once at startup). Used by AssetServer workers.
    template<typename T>
    T run_loader(const std::filesystem::path& resolved) const {
        const auto loader = loader_for<T>();
        return (*loader)(resolved);
    }

    // Stable cache key for an asset of type T at a relative path: the path as
    // the shown language has it (see localized_path), so a load after a
    // language change gets that language's file.
    template<typename T>
    std::string cache_key(std::string_view relative_path) const {
        return std::string(typeid(T).name()) + ":" + localized_path(relative_path);
    }
    // `relative_path` as the active localization has it: its variant at
    // l10n/<locale>/<relative_path> if the shown language (or one it falls
    // back to) has one (Localization::localized_path); else unchanged.
    std::string localized_path(std::string_view relative_path) const;

    template<typename T>
    bool loaded(std::string_view relative_path) const {
        const auto found = _assets.find(cache_key<T>(relative_path));
        return found != _assets.end() && found->second->loaded();
    }

    // Internal server-facing lookup used to keep asynchronous state honest
    // after unload()/clear(). The key is the same stable type-and-path key
    // returned by cache_key().
    bool loaded_key(std::string_view key) const {
        const auto found = _assets.find(std::string{key});
        return found != _assets.end() && found->second->loaded();
    }

    bool contains_key(std::string_view key) const {
        return _assets.find(std::string{key}) != _assets.end();
    }

    template<typename T>
    bool is_current_storage(std::string_view relative_path,
                            const std::shared_ptr<AssetStorage<T>>& storage) const {
        const auto found = _assets.find(cache_key<T>(relative_path));
        return found != _assets.end() && found->second.get() == storage.get();
    }

    template<typename T>
    bool replace_loaded(std::string_view relative_path, T value) {
        const std::string key = cache_key<T>(relative_path);
        const auto found = _assets.find(key);
        if (found == _assets.end()) {
            return false;
        }
        auto storage = std::static_pointer_cast<AssetStorage<T>>(found->second);
        storage->data = std::move(value);
        storage->is_loaded = true;
        describe_memory<T>(*storage);
        _loaded_paths[key] = std::string(relative_path);
        record_loaded_metadata(std::string(relative_path), storage->data);
        if (over_memory_budget()) {
            trim_memory();
        }
        return true;
    }

    void clear();
    void unload(std::string_view relative_path);
    // Main-thread collection. External handles/shared() values are roots; their
    // transitive async dependencies stay resident. Unreferenced cycles can be
    // collected. Returns the number of cache entries released.
    std::size_t unload_unused();
    // The cache keys an entry depends on (AssetServer records what its loaders
    // require). Retention follows these edges; empty `dependencies` removes them.
    void set_dependencies(std::string key, std::vector<std::string> dependencies);
    // Null when the entry has no recorded dependencies.
    const std::vector<std::string>* dependencies(std::string_view key) const;
    // Zero is unlimited. Budget overruns are reported when live owners prevent
    // eviction; live handles and their transitive dependencies are never invalidated.
    void set_memory_budget(u64 bytes) {
        _memory_budget = bytes;
        trim_memory();
    }
    u64 memory_budget() const { return _memory_budget; }
    // Whether the cached payload exceeds the budget now. Cheap: per-frame
    // callers check it before trim_memory().
    bool over_memory_budget() const;
    AssetMemoryReport trim_memory();
    std::vector<AssetResidency> residency() const;
    AssetMemoryReport memory_report() const;
    void discover();

    const std::filesystem::path& root() const { return _root; }
    // The file for `relative_path` (localized, as cache_key is).
    std::filesystem::path resolve(std::string_view relative_path) const;
    Stats stats() const;
    std::vector<std::string> loaded_asset_paths() const;
    std::vector<AssetMetadata> assets_metadata() const;
    const AssetMetadata* metadata(std::string_view relative_path) const;

private:
    template <typename T> void describe_memory(AssetStorage<T>& storage) {
        const auto found = _memory_sizers.find(typeid(T));
        storage.size_estimated = found == _memory_sizers.end();
        storage.resident_bytes =
            found == _memory_sizers.end()
                ? sizeof(T)
                : (*std::static_pointer_cast<std::function<u64(const T&)>>(found->second))(storage.data);
        storage.last_access = ++_access_clock;
    }
    // Runs the loader for `storage`, fills and caches it, records metadata, and
    // returns a handle. Shared by the fresh-load and async-completion paths.
    template<typename T>
    AssetHandle<T> finish_load(std::shared_ptr<AssetStorage<T>> storage,
                               const std::string& key,
                               std::string_view relative_path) {
        const auto loader = loader_for<T>();
        const std::filesystem::path resolved = resolve(relative_path);
        try {
            storage->data = (*loader)(resolved);
        } catch (const std::exception& error) {
            KIN_LOG_ERROR_F("asset",
                            "asset load failed",
                            (LogFields{
                                {.name = "path", .value = std::string{relative_path}},
                                {.name = "resolved", .value = resolved.string()},
                                {.name = "type", .value = std::string{asset_type_name(asset_type_for<T>())}},
                                {.name = "error", .value = error.what()},
                            }));
            throw;
        }
        storage->is_loaded = true;
        describe_memory<T>(*storage);
        _assets[key] = storage;
        _loaded_paths[key] = std::string(relative_path);
        record_loaded_metadata(std::string(relative_path), storage->data);
        KIN_LOG_INFO_F("asset",
                       "asset loaded",
                       (LogFields{
                           {.name = "path", .value = std::string{relative_path}},
                           {.name = "resolved", .value = resolved.string()},
                           {.name = "type", .value = std::string{asset_type_name(asset_type_for<T>())}},
                           {.name = "status", .value = "Loaded"},
                       }));
        AssetHandle<T> handle{storage};
        if (over_memory_budget()) {
            trim_memory();
        }
        return handle;
    }

    template<typename T>
    std::shared_ptr<Loader<T>> loader_for() const {
        const auto found = _loaders.find(typeid(T));
        if (found == _loaders.end()) {
            KIN_LOG_ERROR_F("asset",
                            "asset loader missing",
                            (LogFields{{.name = "type", .value = std::string{asset_type_name(asset_type_for<T>())}}}));
            throw std::runtime_error("No asset loader registered for requested type");
        }

        return std::static_pointer_cast<Loader<T>>(found->second);
    }

    template<typename T>
    void record_loaded_metadata(std::string relative_path, const T& asset) {
        AssetMetadata metadata = make_metadata(relative_path);
        metadata.status = AssetStatus::Loaded;
        metadata.type = asset_type_for<T>();
        if constexpr (std::is_same_v<T, Image>) {
            metadata.dimensions = asset.size;
        }
        _metadata[metadata.path] = metadata;
    }

    AssetMetadata make_metadata(std::string relative_path) const;

    // Every entry a live owner keeps resident (handles, shared() views, pending
    // loads, and what they depend on), mapped to the entry retaining it: empty
    // for an owned entry itself. Visited in key order, so chains are stable.
    using Retainers = std::unordered_map<std::string, std::string>;
    Retainers retainers() const;
    AssetMemoryReport memory_totals(const Retainers& retained) const;
    // Drops cache entries and their edges, then refreshes their paths' metadata.
    void erase_entries(const std::vector<std::string>& keys);
    // Loaded while another type still caches the path; else Discovered or Missing.
    void refresh_metadata_status(const std::vector<std::string>& paths);

    template<typename T>
    static constexpr AssetType asset_type_for() {
        if constexpr (std::is_same_v<T, Image>) {
            return AssetType::Image;
        } else if constexpr (std::is_same_v<T, AnimationRegistryFragment>) {
            return AssetType::Animation;
        } else if constexpr (std::is_same_v<T, AnimationStateMachine>) {
            return AssetType::AnimationStateMachine;
        } else if constexpr (std::is_same_v<T, GameInfo>) {
            return AssetType::GameInfo;
        } else if constexpr (std::is_same_v<T, InputMap>) {
            return AssetType::InputMap;
        } else if constexpr (std::is_same_v<T, ParticleCatalog>) {
            return AssetType::Particles;
        } else if constexpr (std::is_same_v<T, SpriteCatalog>) {
            return AssetType::SpriteCatalog;
        } else if constexpr (std::is_same_v<T, SkeletonAsset2D>) {
            return AssetType::Skeleton2D;
        } else if constexpr (std::is_same_v<T, AudioCatalog>) {
            return AssetType::AudioCatalog;
        } else if constexpr (std::is_same_v<T, TileMap>) {
            return AssetType::TileMap;
        } else if constexpr (std::is_same_v<T, PrefabAsset>) {
            return AssetType::Prefab;
        } else {
            return AssetType::Unknown;
        }
    }

    std::filesystem::path _root;
    std::unordered_map<std::type_index, std::shared_ptr<void>> _loaders;
    std::unordered_map<std::type_index, std::shared_ptr<void>> _memory_sizers;
    u64 _memory_budget = 0, _access_clock = 0;
    std::unordered_map<std::string, std::shared_ptr<IAssetStorage>> _assets;
    std::unordered_map<std::string, std::vector<std::string>> _dependencies;
    std::unordered_map<std::string, std::string> _loaded_paths;
    std::unordered_map<std::string, AssetMetadata> _metadata;
};

void register_default_asset_loaders(AssetManager& assets);

} // namespace kin
