#pragma once

#include <kin/ecs/component.hpp>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace kin {

class ScriptScene;
struct PrefabAsset;

enum class PlaySessionState {
    Stopped,
    Running,
    Paused,
};

struct PlaySessionStartDescriptor {
    std::function<void(EcsWorld&)> configure_play_world;
};

struct PlaySessionResult {
    bool ok = false;
    std::vector<std::string> diagnostics;
};

class PlayWorldSession {
public:
    PlaySessionResult start(EcsWorld& edit_world, PlaySessionStartDescriptor descriptor);
    bool running() const { return _state == PlaySessionState::Running; }
    bool paused() const { return _state == PlaySessionState::Paused; }
    PlaySessionState state() const { return _state; }
    EcsWorld* play_world() { return _play_world.get(); }
    const EcsWorld* play_world() const { return _play_world.get(); }

    bool pause();
    bool resume();
    bool step(f32 dt);
    bool update(f32 dt);
    PlaySessionResult reset();
    SceneDocument apply_document() const;
    void discard();
    const std::string& last_error() const { return _last_error; }

private:
    PlaySessionResult instantiate_from_initial();

    EcsWorld* _edit_world = nullptr;
    std::unique_ptr<EcsWorld> _play_world;
    std::function<void(EcsWorld&)> _configure_play_world;
    SceneDocument _initial_document;
    PlaySessionState _state = PlaySessionState::Stopped;
    std::string _last_error;
};

struct ReloadResult {
    bool ok = true;
    std::vector<std::string> diagnostics;
};

class EcsReloadCoordinator {
public:
    ReloadResult migrate_data_schema(EcsWorld& world,
                                     std::string_view component,
                                     const std::vector<ComponentFieldSnapshot>& fields);
    ReloadResult reload_script_scene(ScriptScene& scene);
    ReloadResult validate_prefab(const PrefabAsset& prefab, const EcsComponentRegistry& components);
};

} // namespace kin
