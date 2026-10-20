#pragma once

#include <kin/scene/scene_session3d.hpp>

namespace sol {
class state;
}
namespace kin::scripting_detail {
struct LuaScene3DBindings {
    EcsWorld* world = nullptr;
    std::filesystem::path asset_root;
    World3D* physics = nullptr;
    WorldPhysicsState3D* bodies = nullptr;
    SceneCommands3D commands;
    SceneQueries3D queries;
    std::function<EcsCommandBuffer*()> callback_commands;
    std::function<bool()> active;
    std::function<std::optional<ScenePrefabPoolStats3D>(ScenePrefabPool3D)> pool_stats;
    std::function<SceneEntity3D(std::string_view)> entity;
};
void bind_lua_scene3d(sol::state&, LuaScene3DBindings);
} // namespace kin::scripting_detail
