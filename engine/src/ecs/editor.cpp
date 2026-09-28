#include <kin/ecs/editor.hpp>

#include <kin/ecs/system.hpp>
#include <kin/prefab/prefab_asset.hpp>
#include <kin/scripting/script_scene.hpp>

namespace kin {

PlaySessionResult PlayWorldSession::start(EcsWorld& edit_world, PlaySessionStartDescriptor descriptor) {
    _edit_world = &edit_world;
    _configure_play_world = std::move(descriptor.configure_play_world);
    _initial_document = scene_document_from_world(edit_world);
    return instantiate_from_initial();
}

bool PlayWorldSession::pause() {
    if (!_play_world || _state == PlaySessionState::Stopped) {
        _last_error = "play session is not running";
        return false;
    }
    _state = PlaySessionState::Paused;
    _last_error.clear();
    return true;
}

bool PlayWorldSession::resume() {
    if (!_play_world || _state == PlaySessionState::Stopped) {
        _last_error = "play session is not running";
        return false;
    }
    _state = PlaySessionState::Running;
    _last_error.clear();
    return true;
}

bool PlayWorldSession::step(f32 dt) {
    if (!_play_world || _state == PlaySessionState::Stopped) {
        _last_error = "play session is not running";
        return false;
    }
    const bool ok = _play_world->run_frame(dt);
    _last_error = ok ? std::string{} : _play_world->systems().last_error();
    return ok;
}

bool PlayWorldSession::update(f32 dt) {
    if (_state != PlaySessionState::Running) {
        return true;
    }
    return step(dt);
}

PlaySessionResult PlayWorldSession::reset() {
    if (!_edit_world) {
        _last_error = "play session has no edit world";
        return {.diagnostics = {_last_error}};
    }
    return instantiate_from_initial();
}

SceneDocument PlayWorldSession::apply_document() const {
    return _play_world ? scene_document_from_world(*_play_world) : SceneDocument{};
}

void PlayWorldSession::discard() {
    _play_world.reset();
    _state = PlaySessionState::Stopped;
    _last_error.clear();
}

PlaySessionResult PlayWorldSession::instantiate_from_initial() {
    _play_world = std::make_unique<EcsWorld>();
    if (_configure_play_world) {
        _configure_play_world(*_play_world);
    }
    std::string error;
    if (!instantiate_scene(*_play_world, _initial_document, error)) {
        _state = PlaySessionState::Stopped;
        _last_error = error;
        _play_world.reset();
        return {.diagnostics = {error}};
    }
    _state = PlaySessionState::Running;
    _last_error.clear();
    return {.ok = true};
}

ReloadResult EcsReloadCoordinator::migrate_data_schema(EcsWorld& world,
                                                       std::string_view component,
                                                       const std::vector<ComponentFieldSnapshot>& fields) {
    if (!world.components().migrate_data_schema(component, fields)) {
        return {.ok = false, .diagnostics = {world.components().last_error()}};
    }
    return {};
}

ReloadResult EcsReloadCoordinator::reload_script_scene(ScriptScene& scene) {
    if (!scene.reload_script()) {
        return {.ok = false, .diagnostics = {scene.last_script_error()}};
    }
    return {};
}

ReloadResult EcsReloadCoordinator::validate_prefab(const PrefabAsset& prefab, const EcsComponentRegistry& components) {
    std::vector<std::string> diagnostics = validate_prefab_asset(prefab, components);
    return {.ok = diagnostics.empty(), .diagnostics = std::move(diagnostics)};
}

} // namespace kin
