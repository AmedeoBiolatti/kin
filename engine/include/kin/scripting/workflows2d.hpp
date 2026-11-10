#pragma once

#include <kin/core/types.hpp>

#include <filesystem>
#include <functional>
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

// A world's main-thread 2D services for Lua (`tweens`, `input_contexts`,
// `rebind`, `pools`, `preload`, `controllers`, `asset_memory`), one per world
// and shared by every Lua host bound to it (shared()). The world, renderer and
// borrowed services must outlive this object and its Lua states. No Lua callback
// is retained here, so a VM can reload while native jobs are still completing.
class ScriptWorkflows2D : public std::enable_shared_from_this<ScriptWorkflows2D> {
public:
    ScriptWorkflows2D(EcsWorld& world, std::filesystem::path content);
    ~ScriptWorkflows2D();
    ScriptWorkflows2D(const ScriptWorkflows2D&) = delete;
    ScriptWorkflows2D& operator=(const ScriptWorkflows2D&) = delete;

    // The world's services: `given` (which becomes the world's when it has
    // none), else the world's, else new ones reading `content`. Throws
    // std::invalid_argument when `given` is another world's or the world
    // already shares other services.
    static std::shared_ptr<ScriptWorkflows2D> shared(EcsWorld& world, const std::filesystem::path& content,
                                                     std::shared_ptr<ScriptWorkflows2D> given = {});

    void configure(Renderer2D* renderer, Gamepads* gamepads = nullptr, Assets2D* assets = nullptr,
                   JobSystem* jobs = nullptr);
    // One simulation step: input contexts and rebinding, the world's tweens
    // (update_tweens), jobs, preloads and memory budgets. Every host sharing
    // the services calls it from its own step with its own `host` key; only
    // the first call of a step does the work.
    void step(const void* host, const Input& input, f32 dt);
    // A host that no longer calls step() (it is going away).
    void leave(const void* host);
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
