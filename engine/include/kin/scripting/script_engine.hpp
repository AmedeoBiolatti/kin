#pragma once

#include <kin/scene/scene.hpp>
#include <kin/scripting/lua_runtime.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sol {
class state;
} // namespace sol

namespace kin {

class JsonWriter;
class ScriptScene;
class ScriptComponentRegistry;
class ScriptWorkflows2D;
class MemoryContent;

struct ScriptOwned {
    i32 marker = 1;
};

// Borrowed scene services for native hosts sharing the Lua ECS API. The world
// and callback captures outlive the engine; registered Lua systems are removed
// before its VM closes. ScriptScene supplies these services automatically.
struct ScriptSceneBindings {
    EcsWorld* world = nullptr;
    const ScriptComponentRegistry* components = nullptr;
    std::filesystem::path script_path;
    std::function<void()> clear_entities;
    std::function<void(Color)> set_background;
    std::function<void()> mark_render_dirty;
    // The world's 2D services, if the host made them (ScriptWorkflows2D::shared
    // makes them otherwise; every Lua host bound to the world shares them).
    std::shared_ptr<ScriptWorkflows2D> workflows;
};

struct ScriptFrameContext {
    f32 dt = 0;
    Input* input = nullptr; // absent input reads false (CPU-only/native hosts)
    App* app = nullptr;     // quit() reports an error when no app was supplied
};

class ScriptEngine {
public:
    // `bind` adds the host's own API (e.g. bind_lua_audio) to the fresh state,
    // after kin's bindings and before any script runs.
    explicit ScriptEngine(const std::function<void(sol::state&)>& bind = {}, LuaRuntimeLimits limits = {});
    ~ScriptEngine();

    ScriptEngine(const ScriptEngine&) = delete;
    ScriptEngine& operator=(const ScriptEngine&) = delete;

    bool load_file(const std::filesystem::path& path);
    bool load_file(const std::filesystem::path& asset_root, const std::filesystem::path& script_path);
    // Safe live reload: copy reload_state and run on_reload(state, scene) in a
    // bounded candidate VM. Native mutations are unavailable during migration;
    // scene:register_system declarations are staged until activate_systems().
    std::unique_ptr<ScriptEngine> prepare_reload(const std::filesystem::path& asset_root,
                                                 const std::filesystem::path& script_path, std::string& error) const;
    void suspend_systems();
    bool activate_systems();
    // Stops the script for good (its systems removed): every later call fails
    // with `reason` as last_error(). A failed reload rollback ends here.
    void halt(std::string reason);
    // Pin future module and workflow loads to the active content generation.
    void use_content(std::shared_ptr<const MemoryContent> content);

    bool call_on_load(ScriptScene& scene);
    bool call_on_load(ScriptSceneBindings bindings);
    bool call_on_enter(SceneContext& ctx);
    bool call_on_exit(SceneContext& ctx);
    bool call_update(SceneContext& ctx);
    bool call_on_enter(ScriptFrameContext ctx);
    bool call_on_exit(ScriptFrameContext ctx);
    bool call_update(ScriptFrameContext ctx);
    bool call_collect_actions(InputActionContext& actions) const;
    bool call_write_report(JsonWriter& json) const;

    // What the state binds and which callbacks it calls, for kin_lua api.
    LuaApiSurface api_surface() const;
    LuaRuntimeStats runtime_stats() const;
    void set_runtime_limits(LuaRuntimeLimits limits);

    const std::string& last_error() const { return _last_error; }
    const std::vector<std::filesystem::path>& script_dependencies() const;
    // The world's 2D services, once on_load bound the engine to a world.
    std::shared_ptr<ScriptWorkflows2D> workflows() const;

private:
    template <typename... Args>
    bool call_optional(std::string_view name, Args&&... args) const;

    void register_bindings();
    void record_dependency(const std::filesystem::path& path);

    // sol2 (the Lua VM state) is fully encapsulated here so that
    // kin/scripting/script_engine.hpp does not pull <sol/sol.hpp> into its
    // consumers. The scripting binding layer that *is* intentionally sol-typed
    // lives in kin/scripting/script_scene.hpp (ScriptComponentRegistry).
    struct Impl;

    mutable std::string _last_error;
    std::unique_ptr<Impl> _impl;
};

} // namespace kin
