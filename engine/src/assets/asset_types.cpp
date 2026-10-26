#include <kin/assets/asset_manager.hpp>

namespace kin {

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
    case AssetType::Scene: return "Scene";
    case AssetType::Audio: return "Audio";
    case AssetType::Font: return "Font";
    case AssetType::Model: return "Model";
    case AssetType::TileSet: return "TileSet";
    case AssetType::Shape: return "Shape";
    case AssetType::Shader: return "Shader";
    case AssetType::ColorLut: return "ColorLut";
    case AssetType::UiScreen: return "UiScreen";
    case AssetType::UiTheme: return "UiTheme";
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

} // namespace kin
