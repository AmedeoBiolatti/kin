#include <kin/anim/anim_format.hpp>
#include <kin/anim/player.hpp>
#include <kin/platform/log.hpp>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

const char* sample_asset = R"(
animation hero.idle
  repeat 0
    clip 0.6
      sprite hero.idle.0 0.0
      sprite hero.idle.1 0.3 pivot 0.5 1
      track SpriteRenderer.tint relative
        key 0.0 color 255 255 255 255 linear
        key 0.6 color 255 220 220 255 easeout
      event 0.0 sound name=step value=hero.step target=arm offset 1 2

animation {X}.idle template
  clip 0.5
    sprite {X}.idle.0 0.0

animation hero.attack
  sequence
    ref hero.windup
    parallel end=primary:0
      clip 0.2
        track SpriteRenderer.rotation
          key 0.0 float 0 linear
          key 0.2 float 20 linear
      clip 0.4
        track SpriteRenderer.offset
          key 0.0 vec2 0 0 linear
          key 0.4 vec2 4 0 linear

statemachine actor
  initial idle
  state idle hero.idle
  state run hero.run
  state hurt hero.hurt
  transition idle -> run when bool:moving == true mode replace
  transition run -> idle when float:speed < 0.1 mode replace
  transition any -> hurt when trigger:hit mode push
)";

void test_parse_serialize_round_trip() {
    kin::AnimationRegistryFragment first;
    std::string error;
    assert(kin::parse_animation_registry_fragment(sample_asset, first, error));
    assert(first.animations.size() == 2);
    assert(first.animation_templates.size() == 1);
    assert(first.state_machines.size() == 1);

    const std::string serialized = kin::serialize_animation_registry_fragment(first);
    kin::AnimationRegistryFragment second;
    assert(kin::parse_animation_registry_fragment(serialized, second, error));
    const std::string serialized_again = kin::serialize_animation_registry_fragment(second);
    assert(serialized == serialized_again);
}

void test_fragment_merge_and_template_resolve() {
    std::vector<kin::LogEvent> events;
    kin::set_logger_config({
        .min_level = kin::LogLevel::Debug,
        .sdl_sink = false,
        .memory_events = &events,
    });

    kin::AnimationRegistryFragment fragment;
    std::string error;
    assert(kin::parse_animation_registry_fragment(sample_asset, fragment, error));
    fragment.source_path = "hero.kinanim";

    kin::AnimationRegistry registry;
    const kin::u64 before = registry.generation();
    registry.merge(fragment);
    assert(registry.generation() > before);

    const auto concrete = registry.resolve("hero.idle");
    assert(concrete);

    const auto templated = registry.resolve("orc.idle", {{"X", "orc"}});
    assert(templated);
    assert(templated->name == "orc.idle");

    bool saw_merge = false;
    bool saw_generation = false;
    bool saw_resolve = false;
    for (const kin::LogEvent& event : events) {
        saw_merge = saw_merge || (event.category == "animation" && event.message == "animation registry fragment merged");
        saw_generation = saw_generation || (event.category == "animation" && event.message == "animation registry generation bumped");
        saw_resolve = saw_resolve || (event.category == "animation" && event.message == "animation template resolved");
    }
    assert(saw_merge);
    assert(saw_generation);
    assert(saw_resolve);
    kin::set_logger_config({.sdl_sink = false});
}

void test_state_machine_conditions_parse() {
    kin::AnimationRegistryFragment fragment;
    std::string error;
    assert(kin::parse_animation_registry_fragment(sample_asset, fragment, error));
    const auto found = fragment.state_machines.find("actor");
    assert(found != fragment.state_machines.end());

    const kin::AnimationStateMachine& machine = found->second;
    assert(machine.initial == "idle");
    assert(machine.transitions.size() == 3);
    assert(machine.transitions[0].when[0].op == kin::CondOp::IsTrue);
    assert(machine.transitions[1].when[0].op == kin::CondOp::Less);
    assert(machine.transitions[2].from.empty());
    assert(machine.transitions[2].mode == kin::TransitionMode::PushOverride);
}

void test_fragment_diagnostics_report_refs_sprites_and_state_targets() {
    kin::AnimationRegistryFragment fragment;
    std::string error;
    assert(kin::parse_animation_registry_fragment(R"(
animation hero.idle
  sequence
    ref hero.missing
    clip 0.2
      sprite hero.idle.0 0
      sprite hero.missing.sprite 0.1

statemachine actor
  initial missing
  state idle hero.idle
  state run hero.run
  transition idle -> missing when trigger:go mode replace
)", fragment, error));

    const std::vector<kin::AnimationDiagnostic> diagnostics = kin::validate_animation_registry_fragment(
        fragment,
        [](std::string_view sprite) {
            return sprite == "hero.idle.0";
        });
    assert(diagnostics.size() == 5);

    bool saw_ref = false;
    bool saw_sprite = false;
    bool saw_initial = false;
    bool saw_state_animation = false;
    bool saw_transition = false;
    for (const kin::AnimationDiagnostic& diagnostic : diagnostics) {
        saw_ref = saw_ref || diagnostic.message.find("unresolved ref") != std::string::npos;
        saw_sprite = saw_sprite || diagnostic.message.find("missing sprite") != std::string::npos;
        saw_initial = saw_initial || diagnostic.message.find("initial state") != std::string::npos;
        saw_state_animation = saw_state_animation || diagnostic.message.find("missing state animation") != std::string::npos;
        saw_transition = saw_transition || diagnostic.message.find("transition target state") != std::string::npos;
    }
    assert(saw_ref);
    assert(saw_sprite);
    assert(saw_initial);
    assert(saw_state_animation);
    assert(saw_transition);
}

void test_rejects_malformed_assets() {
    kin::AnimationRegistryFragment fragment;
    std::string error;
    assert(!kin::parse_animation_registry_fragment("animation bad\n  clip 1\n    track SpriteRenderer.offset\n      key 0 bogus 1\n", fragment, error));
    assert(error.find("unknown value kind") != std::string::npos);

    assert(!kin::parse_animation_registry_fragment("animation empty\n", fragment, error));
    assert(error.find("missing body") != std::string::npos);

    assert(!kin::parse_animation_registry_fragment("statemachine bad\n  transition idle run when trigger:x\n", fragment, error));
    assert(error.find("malformed transition") != std::string::npos);

    assert(!kin::parse_animation_registry_fragment(R"(
statemachine bad
  initial idle
  state idle hero.idle
  transition idle -> idle when trigger:go mode bogus
)", fragment, error));
    assert(error.find("unknown transition mode") != std::string::npos);

    assert(!kin::parse_animation_registry_fragment(R"(
statemachine bad
  initial idle
  state idle hero.idle
  transition idle -> idle when float:speed >= 1
)", fragment, error));
    assert(error.find("unknown float condition operator") != std::string::npos);

    assert(!kin::parse_animation_registry_fragment(R"(
statemachine bad
  initial idle
  state idle hero.idle
  transition idle -> idle when bool:moving == maybe
)", fragment, error));
    assert(error.find("malformed bool condition") != std::string::npos);

    assert(!kin::parse_animation_registry_fragment(R"(
statemachine bad
  initial idle
  state idle hero.idle
  transition idle -> idle when mode push
)", fragment, error));
    assert(error.find("transition requires condition") != std::string::npos);

    assert(!kin::parse_animation_registry_fragment(R"(
statemachine bad
  initial idle
  state idle hero.idle
  transition idle -> idle when trigger:go mode push junk
)", fragment, error));
    assert(error.find("unexpected transition token") != std::string::npos);
}

void test_transition_omitted_mode_defaults_to_replace() {
    kin::AnimationRegistryFragment fragment;
    std::string error;
    assert(kin::parse_animation_registry_fragment(R"(
statemachine actor
  initial idle
  state idle hero.idle
  state walk hero.walk
  transition idle -> walk when bool:moving == true
)", fragment, error));
    const kin::AnimationStateMachine& machine = fragment.state_machines.at("actor");
    assert(machine.transitions.size() == 1);
    assert(machine.transitions[0].mode == kin::TransitionMode::ReplaceBase);
}

void test_reload_merge_preserves_previous_on_failure() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "kin-anim-format-tests";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::filesystem::path path = dir / "hero.kinanim";

    std::ofstream{path} << "animation hero.idle\n  clip 1\n    track SpriteRenderer.rotation\n      key 0 float 1\n      key 1 float 2\n";

    kin::AnimationAssetLibrary library;
    std::string error;
    assert(library.reload_if_changed(path, error));
    const kin::u64 first_generation = library.registry.generation();
    assert(library.registry.resolve("hero.idle"));

    std::ofstream{path} << "animation hero.idle\n  clip 1\n    track SpriteRenderer.rotation\n      key 0 float 3\n      key 1 float 4\n";
    std::filesystem::last_write_time(path, std::filesystem::last_write_time(path) + std::chrono::seconds(2));
    assert(library.reload_if_changed(path, error));
    assert(library.registry.generation() > first_generation);
    const kin::u64 second_generation = library.registry.generation();

    std::ofstream{path} << "animation hero.idle\n  clip 1\n    track SpriteRenderer.rotation\n      key 0 nope 1\n";
    for (int attempts = 0; attempts < 4; ++attempts) {
        std::filesystem::last_write_time(path, std::filesystem::last_write_time(path) + std::chrono::seconds(2));
        error.clear();
        if (!library.reload_if_changed(path, error) && !error.empty()) {
            break;
        }
    }
    assert(!error.empty());
    assert(library.registry.generation() == second_generation);
    assert(library.registry.resolve("hero.idle"));
}

void test_library_stores_and_updates_diagnostics() {
    kin::AnimationRegistryFragment first;
    first.animations.push_back(kin::Animation{
        .name = "hero.idle",
        .root = kin::clip_node(kin::Clip{
            .duration = 1.0f,
            .sprites = {{{.keys = {{.time = 0.0f, .sprite_id = "hero.missing"}}}}},
        }),
    });
    first.source_path = "hero.kinanim";

    kin::AnimationAssetLibrary library;
    assert(library.merge(first, [](std::string_view) {
        return false;
    }));
    assert(library.registry.resolve("hero.idle"));
    assert(library.diagnostics().size() == 1);
    assert(library.diagnostics()[0].message.find("missing sprite") != std::string::npos);
    assert(library.diagnostics_for("hero.kinanim").size() == 1);

    kin::AnimationRegistryFragment second;
    second.animations.push_back(kin::Animation{
        .name = "hero.idle",
        .root = kin::clip_node(kin::Clip{.duration = 1.0f}),
    });
    second.source_path = "hero.kinanim";
    assert(library.merge(second));
    assert(library.diagnostics().empty());
    assert(library.diagnostics_for("hero.kinanim").empty());
}

} // namespace

int main() {
    test_parse_serialize_round_trip();
    test_fragment_merge_and_template_resolve();
    test_state_machine_conditions_parse();
    test_fragment_diagnostics_report_refs_sprites_and_state_targets();
    test_rejects_malformed_assets();
    test_transition_omitted_mode_defaults_to_replace();
    test_reload_merge_preserves_previous_on_failure();
    test_library_stores_and_updates_diagnostics();
    return 0;
}
