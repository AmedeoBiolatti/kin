#include <kin/anim/anim_format.hpp>
#include <kin/assets/asset_manager.hpp>

#include <cassert>
#include <filesystem>
#include <fstream>

void run_anim_asset_loader_tests() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "kin-anim-assets-tests";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    const auto anim_path = dir / "hero.kinanim2";
    const auto state_machine_path = dir / "hero.kinanimsm";
    std::ofstream{anim_path} << "animation hero.idle\n"
                             << "  clip 0.5\n"
                             << "    sprite hero.idle.0 0\n"
                             << "animation {X}.idle template\n"
                             << "  clip 0.5\n"
                             << "    sprite {X}.idle.0 0\n";
    std::ofstream{state_machine_path} << "statemachine actor\n"
                                      << "  initial idle\n"
                                      << "  state idle hero.idle\n"
                                      << "  state hurt hero.hurt\n"
                                      << "  transition any -> hurt when trigger:hit mode push\n";

    kin::AssetManager assets{dir};
    assets.discover();

    const kin::AssetMetadata* anim_meta = assets.metadata("hero.kinanim2");
    assert(anim_meta != nullptr);
    assert(anim_meta->type == kin::AssetType::Animation);
    assert(anim_meta->status == kin::AssetStatus::Discovered);

    const kin::AssetMetadata* state_machine_meta = assets.metadata("hero.kinanimsm");
    assert(state_machine_meta != nullptr);
    assert(state_machine_meta->type == kin::AssetType::AnimationStateMachine);
    assert(state_machine_meta->status == kin::AssetStatus::Discovered);

    const auto fragment = assets.load<kin::AnimationRegistryFragment>("hero.kinanim2");
    assert(fragment);
    assert(fragment->animations.size() == 1);
    assert(fragment->animation_templates.size() == 1);
    anim_meta = assets.metadata("hero.kinanim2");
    assert(anim_meta != nullptr);
    assert(anim_meta->status == kin::AssetStatus::Loaded);

    const auto machine = assets.load<kin::AnimationStateMachine>("hero.kinanimsm");
    assert(machine);
    assert(machine->initial == "idle");
    assert(machine->transitions.size() == 1);
    assert(machine->transitions[0].mode == kin::TransitionMode::PushOverride);
    state_machine_meta = assets.metadata("hero.kinanimsm");
    assert(state_machine_meta != nullptr);
    assert(state_machine_meta->status == kin::AssetStatus::Loaded);
}
