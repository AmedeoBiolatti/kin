#pragma once

#include <kin/scene/scene.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace kin {

class JsonWriter;
class ScriptScene;

class ScriptEngine {
public:
    ScriptEngine();
    ~ScriptEngine();

    ScriptEngine(const ScriptEngine&) = delete;
    ScriptEngine& operator=(const ScriptEngine&) = delete;

    bool load_file(const std::filesystem::path& path);
    bool load_file(const std::filesystem::path& asset_root, const std::filesystem::path& script_path);

    bool call_on_load(ScriptScene& scene);
    bool call_on_enter(SceneContext& ctx);
    bool call_on_exit(SceneContext& ctx);
    bool call_update(SceneContext& ctx);
    bool call_collect_actions(InputActionContext& actions) const;
    bool call_write_report(JsonWriter& json) const;

    const std::string& last_error() const { return _last_error; }
    const std::vector<std::filesystem::path>& script_dependencies() const;

private:
    template <typename... Args>
    bool call_optional(std::string_view name, Args&&... args);

    template <typename... Args>
    bool call_optional_const(std::string_view name, Args&&... args) const;

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
