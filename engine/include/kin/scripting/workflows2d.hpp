#pragma once

#include <filesystem>
#include <functional>
#include <kin/core/types.hpp>
#include <memory>

namespace sol {
class state;
}
namespace kin {
class EcsWorld;
class Input;
class InputRouter;
class Renderer2D;
class Gamepads;
class Assets2D;
class JobSystem;
class MemoryContent;
class PreloadedScene2D;

// Main-thread scene services shared by both Lua hosts. The world, renderer and
// borrowed services must outlive this object and its Lua states. No Lua callback
// is retained here, so a VM can reload while native jobs are still completing.
class ScriptWorkflows2D : public std::enable_shared_from_this<ScriptWorkflows2D> {
public:
    ScriptWorkflows2D(EcsWorld& world, std::filesystem::path content);
    ~ScriptWorkflows2D();
    ScriptWorkflows2D(const ScriptWorkflows2D&) = delete;
    ScriptWorkflows2D& operator=(const ScriptWorkflows2D&) = delete;
    void configure(Renderer2D* renderer, Gamepads* gamepads = nullptr, Assets2D* assets = nullptr,
                   JobSystem* jobs = nullptr);
    void advance(const Input& input, f32 dt);
    InputRouter& input_router();
    EcsWorld& world();
    void use_content(std::shared_ptr<const MemoryContent> content);
    // Lua preload:commit() queues a ready scene here. Native hosts consume it
    // after the script returns; ScriptEngine's SceneContext overload does so.
    std::unique_ptr<PreloadedScene2D> take_scene();

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
    friend void bind_lua_workflows2d(sol::state&, std::function<std::shared_ptr<ScriptWorkflows2D>()>);
};

// Also usable with LuaScript. Entity names work in every host; userdata uses
// the host's checked kin.unwrap_entity / kin.entity registry bridges.
void bind_lua_workflows2d(sol::state& lua, std::function<std::shared_ptr<ScriptWorkflows2D>()> services);
} // namespace kin
