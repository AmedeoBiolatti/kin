#pragma once

#include <kin/ecs/ecs_scene.hpp>

#include <functional>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <typeindex>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include <sol/sol.hpp>

namespace kin {

class ScriptEngine;
class ScriptScene;

struct ScriptOwned {
    i32 marker = 1;
};

struct ScriptDiagnostic {
    std::string message;
    std::string file;
    i32 line = 0;
    std::string function_name;
    std::string system_id;
    std::string phase;
    EcsId entity_id = 0;
    std::string component;
};

enum class ScriptReloadPolicy {
    ClearScriptEntities,
    KeepEntities,
};

// The Lua-binding boundary. Unlike ScriptEngine (whose sol2 state is pimpl'd
// away), this registry is intentionally sol-typed: it maps C++ components to
// Lua get/patch glue, so sol::object/sol::table appear in its signatures by
// design. This header therefore includes <sol/sol.hpp> on purpose; only code
// that defines script component bindings needs to include it.
class ScriptComponentRegistry {
public:
    struct ComponentEntry {
        std::string name;
        std::type_index type = typeid(void);
        std::function<bool(EcsEntity)> has;
        std::function<void(EcsEntity)> remove;
        std::function<sol::object(sol::this_state, EcsEntity)> get;
        std::function<void(EcsEntity, sol::table)> patch;
        std::function<void(EcsWorld&, std::vector<EcsEntity>&)> collect;
    };

    struct FieldEntry {
        std::string name;
        std::function<sol::object(sol::this_state, const void*)> get;
        std::function<void(void*, sol::object)> patch;
    };

    template <typename T>
    class ComponentBuilder;

    template <typename T>
    ComponentBuilder<T> component(std::string name);

    const ComponentEntry* find(std::string_view name) const;

private:
    template <typename T, typename Field>
    static sol::object field_to_lua(sol::this_state state, const Field& value);

    template <typename T, typename Field>
    static void field_from_lua(sol::object object, Field& value);

    std::unordered_map<std::string, std::shared_ptr<ComponentEntry>> _components;
};

struct ScriptSceneConfig {
    std::filesystem::path asset_root = ".";
    std::filesystem::path script_path;
    std::string name = "ScriptScene";
    const ScriptComponentRegistry* components = nullptr;
    std::function<void(ScriptScene&)> before_load;
    std::function<void(ScriptScene&, SceneContext&)> before_update;
    // Binds the host's own API into each fresh Lua state (every load and
    // reload), before the script runs: e.g. kin::bind_lua_audio.
    std::function<void(sol::state&)> bind;
    bool hot_reload =
#ifdef NDEBUG
        false;
#else
        true;
#endif
    ScriptReloadPolicy reload_policy = ScriptReloadPolicy::ClearScriptEntities;
    bool render_enabled = true;
    Color clear_color = Color::rgb(0, 0, 0);
    RenderProfile profile = top_down_2d_profile();
};

class ScriptScene : public EcsScene {
public:
    explicit ScriptScene(ScriptSceneConfig config, EcsWorld* shared_world = nullptr);
    ~ScriptScene() override;

    std::string_view name() const override { return _config.name; }

    void on_enter(SceneContext& ctx) override;
    void on_exit(SceneContext& ctx) override;
    void update(SceneContext& ctx) override;
    void collect_actions(InputActionContext& context) const override;
    void write_report(JsonWriter& json) const override;

    const ScriptSceneConfig& config() const { return _config; }
    const ScriptComponentRegistry* script_components() const { return _config.components; }
    const std::string& last_script_error() const { return _last_error; }
    const ScriptDiagnostic& last_script_diagnostic() const { return _last_diagnostic; }
    bool script_loaded() const { return _script_loaded; }

    std::filesystem::path resolved_script_path() const;
    void set_script_path(std::filesystem::path path);
    void clear_script_entities();
    bool reload_script();

private:
    bool load_script(bool clear_entities);
    void poll_hot_reload();
    void refresh_write_times();
    void set_script_error(std::string error, std::string function_name = {});
    void clear_script_error();

    ScriptSceneConfig _config;
    std::unique_ptr<ScriptEngine> _script;
    std::unordered_map<std::string, std::filesystem::file_time_type> _script_write_times;
    bool _script_loaded = false;
    std::string _last_error;
    ScriptDiagnostic _last_diagnostic;
};

template <typename T>
class ScriptComponentRegistry::ComponentBuilder {
public:
    ComponentBuilder(ScriptComponentRegistry& registry, std::shared_ptr<ComponentEntry> entry)
        : _registry(registry),
          _entry(std::move(entry)) {
    }

    template <typename Field>
    ComponentBuilder& field(std::string name, Field T::* member) {
        auto field_entry = std::make_shared<FieldEntry>();
        field_entry->name = std::move(name);
        field_entry->get = [member](sol::this_state state, const void* raw) {
            const T& component = *static_cast<const T*>(raw);
            return ScriptComponentRegistry::field_to_lua<T>(state, component.*member);
        };
        field_entry->patch = [member](void* raw, sol::object value) {
            T& component = *static_cast<T*>(raw);
            ScriptComponentRegistry::field_from_lua<T>(value, component.*member);
        };
        _fields.push_back(field_entry);

        _entry->get = [fields = _fields](sol::this_state state, EcsEntity entity) {
            const T* component = entity.get<T>();
            if (!component) {
                return sol::object{sol::nil};
            }

            sol::state_view lua{state};
            sol::table result = lua.create_table();
            for (const std::shared_ptr<FieldEntry>& field : fields) {
                result[field->name] = field->get(state, component);
            }
            return sol::object{result};
        };

        _entry->patch = [fields = _fields](EcsEntity entity, sol::table values) {
            T* component = entity.get_mut<T>();
            if (!component) {
                return;
            }

            for (const std::shared_ptr<FieldEntry>& field : fields) {
                sol::object value = values[field->name];
                if (value.get_type() == sol::type::nil) {
                    continue;
                }
                field->patch(component, value);
            }
        };

        return *this;
    }

private:
    ScriptComponentRegistry& _registry;
    std::shared_ptr<ComponentEntry> _entry;
    std::vector<std::shared_ptr<FieldEntry>> _fields;
};

template <typename T>
ScriptComponentRegistry::ComponentBuilder<T> ScriptComponentRegistry::component(std::string name) {
    auto entry = std::make_shared<ComponentEntry>();
    entry->name = name;
    entry->type = typeid(T);
    entry->has = [](EcsEntity entity) {
        return entity.raw().owns<T>();
    };
    entry->remove = [](EcsEntity entity) {
        entity.remove<T>();
    };
    entry->get = [](sol::this_state, EcsEntity) {
        return sol::object{sol::nil};
    };
    entry->patch = [](EcsEntity, sol::table) {};
    entry->collect = [](EcsWorld& world, std::vector<EcsEntity>& entities) {
        const EcsId component_id = static_cast<EcsId>(world.raw().component<T>().id());
        world.query<T>().each_entity([&](EcsEntity entity, const T&) {
            if (entity.id() == component_id) {
                return;
            }
            entities.push_back(entity);
        });
    };

    _components[entry->name] = entry;
    return ComponentBuilder<T>{*this, entry};
}

template <typename T, typename Field>
sol::object ScriptComponentRegistry::field_to_lua(sol::this_state state, const Field& value) {
    sol::state_view lua{state};
    if constexpr (std::is_same_v<Field, Vec2f>) {
        sol::table result = lua.create_table();
        result["x"] = value.x;
        result["y"] = value.y;
        return result;
    } else if constexpr (std::is_same_v<Field, Vec2i>) {
        sol::table result = lua.create_table();
        result["x"] = value.x;
        result["y"] = value.y;
        return result;
    } else if constexpr (std::is_same_v<Field, Color>) {
        sol::table result = lua.create_table();
        result["r"] = value.r;
        result["g"] = value.g;
        result["b"] = value.b;
        result["a"] = value.a;
        return result;
    } else {
        return sol::make_object(state, value);
    }
}

template <typename T, typename Field>
void ScriptComponentRegistry::field_from_lua(sol::object object, Field& value) {
    if constexpr (std::is_same_v<Field, Vec2f>) {
        sol::table table = object.as<sol::table>();
        value.x = table.get_or("x", value.x);
        value.y = table.get_or("y", value.y);
    } else if constexpr (std::is_same_v<Field, Vec2i>) {
        sol::table table = object.as<sol::table>();
        value.x = table.get_or("x", value.x);
        value.y = table.get_or("y", value.y);
    } else if constexpr (std::is_same_v<Field, Color>) {
        sol::table table = object.as<sol::table>();
        value.r = static_cast<u8>(table.get_or("r", static_cast<i32>(value.r)));
        value.g = static_cast<u8>(table.get_or("g", static_cast<i32>(value.g)));
        value.b = static_cast<u8>(table.get_or("b", static_cast<i32>(value.b)));
        value.a = static_cast<u8>(table.get_or("a", static_cast<i32>(value.a)));
    } else {
        value = object.as<Field>();
    }
}

} // namespace kin
