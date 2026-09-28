#pragma once

#include <kin/anim/registry.hpp>
#include <kin/anim/state_machine.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kin {

class AssetManager;

struct AnimationRegistryFragment {
    std::vector<Animation> animations;
    std::vector<Animation> animation_templates;
    std::unordered_map<std::string, AnimationStateMachine> state_machines;
    std::unordered_map<std::string, AnimationStateMachine> state_machine_templates;
    std::filesystem::path source_path;
    u64 generation = 0;
};

enum class AnimationDiagnosticSeverity : u8 {
    Warning,
    Error,
};

struct AnimationDiagnostic {
    AnimationDiagnosticSeverity severity = AnimationDiagnosticSeverity::Error;
    std::string path;
    std::string message;
};

class AnimationAssetLibrary {
public:
    AnimationRegistry registry;
    std::unordered_map<std::string, std::shared_ptr<const AnimationStateMachine>> state_machines;
    std::unordered_map<std::string, std::shared_ptr<const AnimationStateMachine>> state_machine_templates;

    bool merge(const AnimationRegistryFragment& fragment, SpriteValidator sprite_validator = {});
    bool reload_if_changed(const std::filesystem::path& path, std::string& error, SpriteValidator sprite_validator = {});

    const std::vector<AnimationDiagnostic>& diagnostics() const { return _diagnostics; }
    std::vector<AnimationDiagnostic> diagnostics_for(const std::filesystem::path& source_path) const;

private:
    std::unordered_map<std::string, u64> _generations;
    std::vector<AnimationDiagnostic> _diagnostics;
    std::unordered_map<std::string, std::vector<AnimationDiagnostic>> _diagnostics_by_source;
};

bool parse_animation_registry_fragment(std::string_view text, AnimationRegistryFragment& out, std::string& error);
std::string serialize_animation_registry_fragment(const AnimationRegistryFragment& fragment);
std::vector<AnimationDiagnostic> validate_animation_registry_fragment(const AnimationRegistryFragment& fragment,
                                                                      SpriteValidator sprite_validator = {});

AnimationRegistryFragment load_animation_registry_fragment(const std::filesystem::path& path);
bool save_animation_registry_fragment(const AnimationRegistryFragment& fragment, const std::filesystem::path& path);
AnimationStateMachine load_animation_state_machine_asset(const std::filesystem::path& path);
bool save_animation_state_machine_asset(const AnimationStateMachine& machine, const std::filesystem::path& path);
void register_animation_asset_loaders(AssetManager& assets);

} // namespace kin
