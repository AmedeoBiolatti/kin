#include <kin/assets/asset_manager.hpp>
#include <kin/assets/content.hpp>

#include <kin/l10n/localization.hpp>

#include <kin/audio/audio_catalog.hpp>
#include <kin/dialogue/dialogue.hpp>
#include <kin/particles/particle_catalog.hpp>
#include <kin/platform/input.hpp>
#include <kin/platform/log.hpp>
#include <kin/prefab/prefab_asset.hpp>
#include <kin/runtime/game_info.hpp>
#include <kin/scene/scene_asset.hpp>
#include <kin/tilemap/tilemap.hpp>

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <utility>

namespace kin {

void register_animation_asset_loaders(AssetManager& assets);

namespace {

std::string normalized_relative_path(const std::filesystem::path& path) {
    return path.generic_string();
}

std::string lowercase_extension(std::filesystem::path path) {
    std::string ext = path.extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext;
}

AssetStatus status_for_path(const std::filesystem::path& path) {
    return content_file_exists(path) ? AssetStatus::Discovered : AssetStatus::Missing;
}

} // namespace

AssetType infer_asset_type(const std::filesystem::path& path) {
    const std::string ext = lowercase_extension(path);
    if (ext == ".bmp" || ext == ".png") {
        return AssetType::Image;
    }
    if (ext == ".kinanim" || ext == ".kinanim2") {
        return AssetType::Animation;
    }
    if (ext == ".kinanimsm") {
        return AssetType::AnimationStateMachine;
    }
    if (ext == ".kininfo") {
        return AssetType::GameInfo;
    }
    if (ext == ".kininput") {
        return AssetType::InputMap;
    }
    if (ext == ".kinparticles") {
        return AssetType::Particles;
    }
    if (ext == ".lua" || ext == ".kinlua") {
        return AssetType::Script;
    }
    if (ext == ".kinsprites") {
        return AssetType::SpriteCatalog;
    }
    if (ext == ".kinaudio") {
        return AssetType::AudioCatalog;
    }
    if (ext == ".kintilemap") {
        return AssetType::TileMap;
    }
    if (ext == ".kindialogue") {
        return AssetType::Dialogue;
    }
    if (ext == ".kinprefab") {
        return AssetType::Prefab;
    }
    return AssetType::Unknown;
}

std::string_view asset_type_name(AssetType type) {
    switch (type) {
    case AssetType::Unknown: return "Unknown";
    case AssetType::Image: return "Image";
    case AssetType::Animation: return "Animation";
    case AssetType::AnimationStateMachine: return "AnimationStateMachine";
    case AssetType::GameInfo: return "GameInfo";
    case AssetType::InputMap: return "InputMap";
    case AssetType::Particles: return "Particles";
    case AssetType::Script: return "Script";
    case AssetType::SpriteCatalog: return "SpriteCatalog";
    case AssetType::AudioCatalog: return "AudioCatalog";
    case AssetType::TileMap: return "TileMap";
    case AssetType::Dialogue: return "Dialogue";
    case AssetType::Prefab: return "Prefab";
    }
    return "Unknown";
}

std::string_view asset_status_name(AssetStatus status) {
    switch (status) {
    case AssetStatus::Discovered: return "Discovered";
    case AssetStatus::Loaded: return "Loaded";
    case AssetStatus::Missing: return "Missing";
    }
    return "Missing";
}

AssetManager::AssetManager(std::filesystem::path root)
    : _root(std::move(root)) {
    KIN_LOG_INFO_F("asset",
                   "asset manager created",
                   (LogFields{{.name = "root", .value = _root.string()}}));
    register_default_asset_loaders(*this);
}

void AssetManager::clear() {
    const std::size_t asset_count = _assets.size();
    for (auto& [key, asset] : _assets) {
        asset->clear_loaded();
    }
    _assets.clear();
    _loaded_paths.clear();
    for (auto& [path, metadata] : _metadata) {
        metadata.status = status_for_path(resolve(path));
    }
    KIN_LOG_DEBUG_F("asset",
                    "asset manager cleared",
                    (LogFields{{.name = "count", .value = std::to_string(asset_count)}}));
}

void AssetManager::unload(std::string_view relative_path) {
    const std::string suffix = ":" + std::string(relative_path);
    std::size_t removed = 0;
    for (auto it = _assets.begin(); it != _assets.end();) {
        if (it->first.ends_with(suffix)) {
            it->second->clear_loaded();
            _loaded_paths.erase(it->first);
            it = _assets.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }

    const std::string path{relative_path};
    if (auto found = _metadata.find(path); found != _metadata.end()) {
        found->second.status = status_for_path(resolve(path));
    }
    KIN_LOG_DEBUG_F("asset",
                    "asset unloaded",
                    (LogFields{
                        {.name = "path", .value = path},
                        {.name = "count", .value = std::to_string(removed)},
                    }));
}

void AssetManager::discover() {
    if (!content_directory_exists(_root)) {
        KIN_LOG_WARN_F("asset",
                       "asset discovery root missing",
                       (LogFields{{.name = "root", .value = _root.string()}}));
        return;
    }

    std::size_t discovered_count = 0;
    for (const std::filesystem::path& file : list_content_files(_root, true)) {
        const std::filesystem::path relative = file.lexically_relative(_root);
        AssetMetadata discovered = make_metadata(normalized_relative_path(relative));
        if (auto found = _metadata.find(discovered.path); found != _metadata.end() && found->second.status == AssetStatus::Loaded) {
            discovered.status = AssetStatus::Loaded;
            discovered.dimensions = found->second.dimensions;
        }
        _metadata[discovered.path] = discovered;
        ++discovered_count;
    }
    KIN_LOG_DEBUG_F("asset",
                    "asset discovery complete",
                    (LogFields{
                        {.name = "root", .value = _root.string()},
                        {.name = "count", .value = std::to_string(discovered_count)},
                    }));
}

std::string AssetManager::localized_path(std::string_view relative_path) const {
    // Already a localized path (as load_handle passes on): left as it is.
    if (relative_path.starts_with("l10n/")) {
        return std::string{relative_path};
    }
    if (const Localization* l10n = active_localization()) {
        return l10n->localized_path(_root, relative_path);
    }
    return std::string{relative_path};
}

std::filesystem::path AssetManager::resolve(std::string_view relative_path) const {
    return _root / std::filesystem::path{localized_path(relative_path)};
}

AssetManager::Stats AssetManager::stats() const {
    const std::size_t loaded = static_cast<std::size_t>(std::ranges::count_if(_assets, [](const auto& entry) {
        return entry.second->loaded();
    }));
    return {
        .loaded_assets = loaded,
        .registered_loaders = _loaders.size(),
    };
}

std::vector<std::string> AssetManager::loaded_asset_paths() const {
    std::vector<std::string> paths;
    paths.reserve(_loaded_paths.size());
    for (const auto& [key, path] : _loaded_paths) {
        paths.push_back(path);
    }
    std::ranges::sort(paths);
    return paths;
}

std::vector<AssetMetadata> AssetManager::assets_metadata() const {
    std::vector<AssetMetadata> result;
    result.reserve(_metadata.size());
    for (const auto& [path, metadata] : _metadata) {
        result.push_back(metadata);
    }
    std::ranges::sort(result, {}, &AssetMetadata::path);
    return result;
}

const AssetMetadata* AssetManager::metadata(std::string_view relative_path) const {
    const auto found = _metadata.find(std::string(relative_path));
    return found == _metadata.end() ? nullptr : &found->second;
}

AssetMetadata AssetManager::make_metadata(std::string relative_path) const {
    const std::filesystem::path resolved = resolve(relative_path);
    AssetMetadata metadata{
        .name = std::filesystem::path{relative_path}.filename().string(),
        .path = normalized_relative_path(relative_path),
        .type = infer_asset_type(relative_path),
        .status = status_for_path(resolved),
    };
    if (const std::optional<u64> size = content_file_size(resolved)) {
        metadata.size_bytes = *size;
    }
    return metadata;
}

void register_default_asset_loaders(AssetManager& assets) {
    assets.register_loader<Image>([](const std::filesystem::path& path) {
        return load_image(path);
    });
    register_animation_asset_loaders(assets);
    assets.register_loader<GameInfo>([](const std::filesystem::path& path) {
        return load_game_info(path);
    });
    assets.register_loader<InputMap>([](const std::filesystem::path& path) {
        return load_input_map(path);
    });
    register_particle_catalog_loader(assets);
    register_audio_catalog_loader(assets);
    register_tilemap_loader(assets);
    assets.register_loader<DialogueDocument>([](const std::filesystem::path& path) {
        DialogueLoadResult result = load_dialogue(path);
        if (!result.ok()) {
            throw std::runtime_error(result.error.empty() ? "failed to load dialogue" : result.error);
        }
        return std::move(*result.document);
    });
    assets.register_loader<PrefabAsset>([](const std::filesystem::path& path) {
        EcsWorld validation_world;
        register_builtin_editor_components(validation_world);
        PrefabLoadResult result = load_prefab_asset(path, validation_world.components());
        if (!result.ok()) {
            std::ostringstream message;
            message << "failed to load prefab";
            for (const std::string& diagnostic : result.diagnostics) {
                message << ": " << diagnostic;
            }
            throw std::runtime_error(message.str());
        }
        return std::move(*result.prefab);
    });
}

} // namespace kin
