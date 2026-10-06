#pragma once

#include <kin/assets/asset_handle.hpp>
#include <kin/assets/image.hpp>
#include <kin/platform/log.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
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
        _loaded_paths[key] = std::string(relative_path);
        record_loaded_metadata(std::string(relative_path), storage->data);
        return true;
    }

    void clear();
    void unload(std::string_view relative_path);
    void discover();

    const std::filesystem::path& root() const { return _root; }
    // The file for `relative_path` (localized, as cache_key is).
    std::filesystem::path resolve(std::string_view relative_path) const;
    Stats stats() const;
    std::vector<std::string> loaded_asset_paths() const;
    std::vector<AssetMetadata> assets_metadata() const;
    const AssetMetadata* metadata(std::string_view relative_path) const;

private:
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
        return AssetHandle<T>{storage};
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
    std::unordered_map<std::string, std::shared_ptr<IAssetStorage>> _assets;
    std::unordered_map<std::string, std::string> _loaded_paths;
    std::unordered_map<std::string, AssetMetadata> _metadata;
};

void register_default_asset_loaders(AssetManager& assets);

} // namespace kin
