#include "lua_api_surface.hpp"
#include "lua_reload_internal.hpp"
#include "lua_state_copy.hpp"

#include <kin/core/profile.hpp>
#include <kin/ecs/animator.hpp>
#include <kin/ecs/camera.hpp>
#include <kin/ecs/dialogue.hpp>
#include <kin/ecs/lifecycle.hpp>
#include <kin/ecs/particles.hpp>
#include <kin/ecs/render.hpp>
#include <kin/ecs/saving.hpp>
#include <kin/core/timers.hpp>
#include <kin/ecs/scene_flow.hpp>
#include <kin/ecs/tween.hpp>
#include <kin/ecs/script.hpp>
#include <kin/ecs/ui_screens.hpp>
#include <kin/ecs/world.hpp>
#include <kin/l10n/localization.hpp>
#include <kin/platform/input.hpp>
#include <kin/platform/log.hpp>
#include <kin/prefab/prefab_asset.hpp>
#include <kin/scripting/entity_scripts.hpp>
#include <kin/scripting/lua_script.hpp>
#include <kin/scripting/workflows2d.hpp>
#if KIN_ENABLE_PHYSICS
#include <kin/ecs/physics.hpp>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kin {
namespace {

using FileTime = std::optional<std::filesystem::file_time_type>;

FileTime time_of(const std::filesystem::path& file) {
    std::error_code error;
    const auto time = std::filesystem::last_write_time(file, error);
    return error ? std::nullopt : std::optional(time);
}

// What a script holds of an entity: the world it is in, and it (gone: every
// method but alive() and id() fails).
struct ScriptEntity {
    EcsWorld* world = nullptr;
    flecs::entity entity;

    u64 lifetime = 0;
    ScriptEntity(EcsWorld* w, flecs::entity e) : world(w), entity(e) {
        if (entity && entity.is_alive())
            if (const auto* lease = entity.get<EntityLifetime>())
                lifetime = lease->generation;
    }
    bool alive() const {
        if (!entity || !entity.is_alive())
            return false;
        const auto* lease = entity.get<EntityLifetime>();
        return lifetime == (lease ? lease->generation : 0);
    }
    flecs::entity live() const {
        if (scripting_detail::reload_staging)
            throw sol::error("live entity access is unavailable while preparing reload");
        if (!alive()) {
            throw sol::error("the entity is gone");
        }
        return entity;
    }
};

// A field's value as Lua holds it, and back.
sol::object to_lua(sol::state_view lua, const ComponentFieldValue& value) {
    return std::visit(
        [&](const auto& v) -> sol::object {
            using V = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<V, bool> || std::is_same_v<V, std::string>) {
                return sol::make_object(lua, v);
            } else if constexpr (std::is_arithmetic_v<V>) {
                return sol::make_object(lua, static_cast<f64>(v));
            } else if constexpr (std::is_same_v<V, Vec2f> || std::is_same_v<V, Vec2i>) {
                return sol::make_object(lua, lua.create_table_with("x", v.x, "y", v.y));
            } else if constexpr (std::is_same_v<V, Color>) {
                return sol::make_object(lua, lua.create_table_with("r", v.r, "g", v.g, "b", v.b, "a", v.a));
            } else if constexpr (std::is_same_v<V, ComponentAssetRef>) {
                return sol::make_object(lua, v.id);
            } else if constexpr (std::is_same_v<V, ComponentEntityRef>) {
                return sol::make_object(lua, static_cast<f64>(v.id));
            } else if constexpr (std::is_same_v<V, std::vector<Vec2f>>) {
                sol::table points = lua.create_table(static_cast<int>(v.size()), 0);
                for (std::size_t i = 0; i < v.size(); ++i) {
                    points[i + 1] = lua.create_table_with("x", v[i].x, "y", v[i].y);
                }
                return sol::make_object(lua, points);
            } else {
                return sol::lua_nil; // 3D values: not for 2D scripts
            }
        },
        value);
}

std::optional<ComponentFieldValue> from_lua(ComponentFieldKind kind, const sol::object& value) {
    const auto number = [&]() -> std::optional<f64> {
        return value.get_type() == sol::type::number ? std::optional(value.as<f64>()) : std::nullopt;
    };
    const auto field = [&](const char* name, f64 fallback) {
        if (value.get_type() != sol::type::table) {
            return fallback;
        }
        const sol::object part = value.as<sol::table>()[name];
        return part.get_type() == sol::type::number ? part.as<f64>() : fallback;
    };
    switch (kind) {
    case ComponentFieldKind::Bool:
        if (value.get_type() == sol::type::boolean) return value.as<bool>();
        break;
    case ComponentFieldKind::I32:
        if (const auto n = number()) return static_cast<i32>(*n);
        break;
    case ComponentFieldKind::I64:
        if (const auto n = number()) return static_cast<i64>(*n);
        break;
    case ComponentFieldKind::U32:
        if (const auto n = number(); n && *n >= 0) return static_cast<u32>(*n);
        break;
    case ComponentFieldKind::U64:
        if (const auto n = number(); n && *n >= 0) return static_cast<u64>(*n);
        break;
    case ComponentFieldKind::F32:
        if (const auto n = number()) return static_cast<f32>(*n);
        break;
    case ComponentFieldKind::F64:
        if (const auto n = number()) return *n;
        break;
    case ComponentFieldKind::String:
        if (value.get_type() == sol::type::string) return value.as<std::string>();
        break;
    case ComponentFieldKind::AssetRef:
        if (value.get_type() == sol::type::string) return ComponentAssetRef{value.as<std::string>()};
        break;
    case ComponentFieldKind::Vec2f:
        if (value.get_type() == sol::type::table) return Vec2f{static_cast<f32>(field("x", 0.0)), static_cast<f32>(field("y", 0.0))};
        break;
    case ComponentFieldKind::Vec2i:
        if (value.get_type() == sol::type::table) return Vec2i{static_cast<i32>(field("x", 0.0)), static_cast<i32>(field("y", 0.0))};
        break;
    case ComponentFieldKind::Color:
        if (value.get_type() == sol::type::table) {
            const auto channel = [&](const char* name) { return static_cast<u8>(std::clamp(field(name, 255.0), 0.0, 255.0)); };
            return Color{channel("r"), channel("g"), channel("b"), channel("a")};
        }
        break;
    case ComponentFieldKind::Points:
        if (value.get_type() == sol::type::table) {
            const sol::table table = value.as<sol::table>();
            std::vector<Vec2f> points;
            for (std::size_t i = 1; i <= table.size(); ++i) {
                const sol::object item = table[i];
                if (item.get_type() != sol::type::table) {
                    return std::nullopt;
                }
                const sol::table point = item.as<sol::table>();
                points.push_back({point.get_or("x", 0.0f), point.get_or("y", 0.0f)});
            }
            return points;
        }
        break;
    default: break;
    }
    return std::nullopt;
}

// A Lua value as a save keeps it: true/false, numbers, text, and tables of
// them (a table counting from 1 with nothing else, a list); none for the rest
// (functions, entities, a table nested too deep).
std::optional<JsonValue> json_of(const sol::object& value, int depth = 0) {
    switch (value.get_type()) {
    case sol::type::boolean: return JsonValue{value.as<bool>()};
    case sol::type::number: {
        const f64 number = value.as<f64>();
        return std::isfinite(number) ? std::optional(JsonValue{number}) : std::nullopt;
    }
    case sol::type::string: return JsonValue{value.as<std::string>()};
    case sol::type::table: {
        if (depth >= 16) {
            return std::nullopt;
        }
        const sol::table table = value.as<sol::table>();
        const std::size_t length = table.size();
        std::size_t count = 0;
        table.for_each([&](const sol::object&, const sol::object&) { ++count; });
        if (length > 0 && count == length) {
            JsonValue::Array items;
            for (std::size_t i = 1; i <= length; ++i) {
                std::optional<JsonValue> item = json_of(sol::object(table[i]), depth + 1);
                items.push_back(item ? std::move(*item) : JsonValue{});
            }
            return JsonValue{std::move(items)};
        }
        JsonValue::Object members;
        table.for_each([&](const sol::object& key, const sol::object& item) {
            std::string name;
            if (key.get_type() == sol::type::string) {
                name = key.as<std::string>();
            } else if (key.get_type() == sol::type::number) {
                char text[32];
                std::snprintf(text, sizeof(text), "%.17g", key.as<f64>());
                name = text;
            } else {
                return;
            }
            if (std::optional<JsonValue> kept = json_of(item, depth + 1)) {
                members.emplace(std::move(name), std::move(*kept));
            }
        });
        return JsonValue{std::move(members)};
    }
    default: return std::nullopt;
    }
}

// And back.
sol::object lua_of(sol::state_view lua, const JsonValue& value) {
    switch (value.type()) {
    case JsonValue::Type::Bool: return sol::make_object(lua, value.as_bool());
    case JsonValue::Type::Number: {
        // A whole number an integer again (a count shows "3", not "3.0").
        const f64 number = value.as_number();
        if (std::trunc(number) == number && std::abs(number) < 9.0e15) {
            return sol::make_object(lua, static_cast<lua_Integer>(number));
        }
        return sol::make_object(lua, number);
    }
    case JsonValue::Type::String: return sol::make_object(lua, value.as_string());
    case JsonValue::Type::Array: {
        sol::table table = lua.create_table(static_cast<int>(value.items().size()), 0);
        for (std::size_t i = 0; i < value.items().size(); ++i) {
            table[i + 1] = lua_of(lua, value.items()[i]);
        }
        return sol::make_object(lua, table);
    }
    case JsonValue::Type::Object: {
        sol::table table = lua.create_table(0, static_cast<int>(value.members().size()));
        for (const auto& [name, member] : value.members()) {
            table[name] = lua_of(lua, member);
        }
        return sol::make_object(lua, table);
    }
    default: return sol::lua_nil;
    }
}

// Makes a script's `self`: a table of its own, with the entity's methods.
constexpr const char* self_prelude = R"lua(
function __kin_self(entity)
    return setmetatable({entity = entity}, {__index = function(_, key)
        local method = entity[key]
        if type(method) == "function" then
            return function(_, ...) return method(entity, ...) end
        end
        return nil
    end})
end
)lua";

} // namespace

namespace {

// A number as text: 12, not 12.000000.
std::string format_number(double value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%g", value);
    return text;
}

// What a character found in its last step.
struct CharacterFound {
    bool floor = false;
    bool wall = false;
    bool ceiling = false;
};

// An entity a script moved: its body goes there at the next step.
void teleport(EcsEntity entity) {
#if KIN_ENABLE_PHYSICS
    if (entity.raw().has<PhysicsBodyComponent>()) {
        entity.raw().add<PhysicsTeleport>();
    }
#else
    (void)entity;
#endif
}

} // namespace

struct EntityScripts::Impl {
    EntityScriptsConfig config;
    std::shared_ptr<ScriptWorkflows2D> workflows;

    // A script file read, and the times of the files it was read from.
    struct File {
        std::unique_ptr<LuaScript> script;
        std::vector<std::pair<std::filesystem::path, FileTime>> times;
        u64 generation = 0; // bumped by each read: its selves belong to one
    };
    // An entity running a script: its self, made in the file's state then.
    struct Running {
        std::string path;
        u64 generation = 0;
        sol::table self;
        bool started = false;
        Timers timers; // its self:after and self:every
        std::unordered_map<u32, sol::protected_function> tween_done; // by tween id
        u64 lifetime = 0;
        u64 entity_id = 0;
    };
    std::unordered_map<std::string, File> files;
    std::unordered_map<u64, Running> running;
    std::map<std::string, std::string> problems;
    std::unordered_map<std::string, std::shared_ptr<const PrefabAsset>> prefabs;
    i32 frames = 0;

    // What the bindings reach, during an update.
    EcsWorld* world = nullptr;
    EcsWorld* bound_world = nullptr;
    Input* input = nullptr;
    SceneScreens* screens = nullptr;
    SceneDialogue* dialogue = nullptr;
    SceneFlow* flow = nullptr;
    SceneSaves* saves = nullptr;
    PhysicsWorld* physics = nullptr;
    // Values given back by the saves, for entities once they start (by id).
    std::unordered_map<u64, JsonValue> restored;
    std::string calling; // the script being called, for its problems

    void bind(sol::state& lua) {
        bind_lua_workflows2d(lua, [this] { return workflows; });
        lua.registry()["kin.entity"] = [this](EcsEntity entity, std::function<bool()>) {
            if (!world || entity.raw().world().c_ptr() != world->raw().c_ptr())
                throw sol::error("foreign pool entity");
            return ScriptEntity{world, entity.raw()};
        };
        lua.registry()["kin.unwrap_entity"] = [this](const ScriptEntity& entity) {
            if (entity.world != world)
                throw sol::error("foreign tween entity");
            return EcsEntity{entity.live()};
        };
        sol::usertype<ScriptEntity> entity = lua.new_usertype<ScriptEntity>("Entity", sol::no_constructor);
        entity["id"] = [](const ScriptEntity& e) { return static_cast<f64>(e.entity.id()); };
        entity["alive"] = &ScriptEntity::alive;
        entity["name"] = [](const ScriptEntity& e) { return std::string(e.live().name().c_str()); };
        entity["parent"] = [](const ScriptEntity& e) -> std::optional<ScriptEntity> {
            const flecs::entity parent = e.live().parent();
            return parent ? std::optional(ScriptEntity{e.world, parent}) : std::nullopt;
        };
        entity["child"] = [](const ScriptEntity& e, const std::string& name) -> std::optional<ScriptEntity> {
            const flecs::entity child = e.live().lookup(name.c_str());
            return child ? std::optional(ScriptEntity{e.world, child}) : std::nullopt;
        };
        entity["has"] = [](const ScriptEntity& e, const std::string& component) {
            return e.world->components().has(EcsEntity{e.live()}, component);
        };
        entity["add"] = [](const ScriptEntity& e, const std::string& component) {
            return e.world->components().add(EcsEntity{e.live()}, component);
        };
        entity["remove"] = [](const ScriptEntity& e, const std::string& component) {
            return e.world->components().remove(EcsEntity{e.live()}, component);
        };
        entity["get"] = [](const ScriptEntity& e, const std::string& component, sol::this_state state) -> sol::object {
            sol::state_view lua(state);
            sol::table fields = lua.create_table();
            const bool found = e.world->components().visit_fields(
                EcsEntity{e.live()}, component,
                [&](std::string_view name, ComponentFieldKind, const ComponentFieldValue& value) { fields[std::string(name)] = to_lua(lua, value); });
            return found ? sol::object(fields) : sol::lua_nil;
        };
        entity["set"] = [](const ScriptEntity& e, const std::string& component, const sol::table& values) {
            EcsComponentRegistry& registry = e.world->components();
            const ComponentDescriptor* descriptor = registry.find(component);
            if (!descriptor) {
                throw sol::error("no component '" + component + "' is described");
            }
            EcsEntity target{e.live()};
            if (!registry.has(target, component)) {
                registry.add(target, component);
            }
            for (const auto& [key, value] : values) {
                const std::string name = key.as<std::string>();
                const auto field = std::ranges::find(descriptor->fields, name, &ComponentFieldDescriptor::name);
                if (field == descriptor->fields.end()) {
                    throw sol::error(component + " has no field '" + name + "'");
                }
                const std::optional<ComponentFieldValue> converted = from_lua(field->kind, value);
                if (!converted || !registry.patch_field(target, component, name, *converted)) {
                    throw sol::error(component + "." + name + " takes a " + component_field_kind_name(field->kind));
                }
            }
        };
        entity["position"] = [](const ScriptEntity& e) {
            const Transform2D* t = e.live().get<Transform2D>();
            return std::make_tuple(t ? t->pos.x : 0.0f, t ? t->pos.y : 0.0f);
        };
        entity["set_position"] = [](const ScriptEntity& e, f32 x, f32 y) {
            EcsEntity target{e.live()};
            Transform2D t = target.get<Transform2D>() ? *target.get<Transform2D>() : Transform2D{};
            t.pos = {x, y};
            target.set(t);
            teleport(target);
        };
        entity["move"] = [](const ScriptEntity& e, f32 dx, f32 dy) {
            EcsEntity target{e.live()};
            Transform2D t = target.get<Transform2D>() ? *target.get<Transform2D>() : Transform2D{};
            t.pos = {t.pos.x + dx, t.pos.y + dy};
            target.set(t);
            teleport(target);
        };
        // Its velocity: its character's (CharacterController2D), else its body's.
        entity["velocity"] = [this](const ScriptEntity& e) {
            Vec2f velocity{};
#if KIN_ENABLE_PHYSICS
            const flecs::entity live = e.live();
            if (const CharacterController2D* character = live.get<CharacterController2D>()) {
                velocity = character->velocity;
            } else if (const PhysicsVelocity* set = live.get<PhysicsVelocity>()) {
                velocity = set->linear;
            } else if (const PhysicsBodyComponent* body = live.get<PhysicsBodyComponent>(); body && physics) {
                velocity = physics->linear_velocity(body->body);
            }
#else
            (void)e;
            (void)this;
#endif
            return std::make_tuple(velocity.x, velocity.y);
        };
        entity["set_velocity"] = [](const ScriptEntity& e, f32 x, f32 y) {
#if KIN_ENABLE_PHYSICS
            flecs::entity live = e.live();
            if (CharacterController2D* character = live.get_mut<CharacterController2D>()) {
                character->velocity = {x, y};
            } else {
                live.set(PhysicsVelocity{.linear = {x, y}});
            }
#else
            (void)e, (void)x, (void)y;
            throw sol::error("set_velocity: the game has no physics");
#endif
        };
        // What its character found in the last step.
        const auto character_found = [](const ScriptEntity& e, bool CharacterFound::*what) {
#if KIN_ENABLE_PHYSICS
            const CharacterController2D* character = e.live().get<CharacterController2D>();
            if (!character) {
                return false;
            }
            const CharacterFound found{character->on_floor, character->on_wall, character->on_ceiling};
            return found.*what;
#else
            (void)e, (void)what;
            return false;
#endif
        };
        entity["on_floor"] = [character_found](const ScriptEntity& e) { return character_found(e, &CharacterFound::floor); };
        entity["on_wall"] = [character_found](const ScriptEntity& e) { return character_found(e, &CharacterFound::wall); };
        entity["on_ceiling"] = [character_found](const ScriptEntity& e) { return character_found(e, &CharacterFound::ceiling); };
        entity["rotation"] = [](const ScriptEntity& e) {
            const Transform2D* t = e.live().get<Transform2D>();
            return t ? t->rotation : 0.0f;
        };
        entity["set_rotation"] = [](const ScriptEntity& e, f32 degrees) {
            EcsEntity target{e.live()};
            Transform2D t = target.get<Transform2D>() ? *target.get<Transform2D>() : Transform2D{};
            t.rotation = degrees;
            target.set(t);
        };
        entity["destroy"] = [](const ScriptEntity& e) {
            const EcsEntity target{e.live()};
            if (e.world && e.world->entities().registered(target)) {
                e.world->entities().destroy(target); // its authored id (and its children's) let go, and told
            } else {
                target.raw().destruct();
            }
        };
        // Its ParticleSystemComponent's effect burst (or another of its catalog).
        entity["emit"] = [](const ScriptEntity& e, sol::optional<std::string> effect) {
            return emit_particles(EcsEntity{e.live()}, effect.value_or(std::string{}));
        };
        // Its tweens (kin/ecs/tween.hpp; the game runs update_tweens).
        entity["tween"] = [this](const ScriptEntity& e, const std::string& property, const sol::object& to, f32 seconds,
                                 sol::object easing_or_options, sol::object options_given) {
            TweenDef def{.property = property, .seconds = seconds};
            // `to` as the field's kind says (a number, {x, y}, {r, g, b, a}...).
            const std::size_t dot = property.find('.');
            const ComponentDescriptor* component = dot == std::string::npos ? nullptr : e.world->components().find(property.substr(0, dot));
            if (component) {
                const auto field = std::ranges::find(component->fields, property.substr(dot + 1), &ComponentFieldDescriptor::name);
                if (field != component->fields.end()) {
                    const std::optional<ComponentFieldValue> value = from_lua(field->kind, to);
                    if (!value) {
                        throw sol::error("tween: " + property + " takes a " + component_field_kind_name(field->kind));
                    }
                    std::visit(
                        [&](const auto& typed) {
                            using V = std::decay_t<decltype(typed)>;
                            if constexpr (std::is_constructible_v<AnimValue, V> &&
                                          (std::is_same_v<V, f32> || std::is_same_v<V, Vec2f> || std::is_same_v<V, Color> ||
                                           std::is_same_v<V, i32> || std::is_same_v<V, bool> || std::is_same_v<V, std::string>)) {
                                def.to = typed;
                            } else {
                                throw sol::error("tween: " + property + " is not a number, a pair, a colour, true/false or text");
                            }
                        },
                        *value);
                }
            }
            sol::object options = options_given;
            if (easing_or_options.get_type() == sol::type::string) {
                if (!parse_easing(easing_or_options.as<std::string>(), def.easing)) {
                    throw sol::error("tween: no easing is called '" + easing_or_options.as<std::string>() + "'");
                }
            } else if (easing_or_options.get_type() == sol::type::table) {
                options = easing_or_options;
            }
            sol::optional<sol::protected_function> done;
            if (options.get_type() == sol::type::table) {
                const sol::table given = options.as<sol::table>();
                def.delay = given.get_or("delay", 0.0f);
                def.repeats = given.get_or("repeats", 0);
                def.yoyo = given.get_or("yoyo", false);
                def.relative = given.get_or("relative", false);
                if (const sol::object easing = given["easing"]; easing.get_type() == sol::type::string &&
                                                                !parse_easing(easing.as<std::string>(), def.easing)) {
                    throw sol::error("tween: no easing is called '" + easing.as<std::string>() + "'");
                }
                done = given.get<sol::optional<sol::protected_function>>("done");
            }
            std::string error;
            const u32 id = kin::tween(*e.world, EcsEntity{e.live()}, std::move(def), &error);
            if (id == 0) {
                throw sol::error("tween: " + error);
            }
            if (done) {
                if (const auto run = running.find(e.entity.id()); run != running.end()) {
                    run->second.tween_done[id] = *done;
                }
            }
            return id;
        };
        entity["tweening"] = [](const ScriptEntity& e, sol::optional<std::string> property) {
            return tweening(EcsEntity{e.live()}, property.value_or(""));
        };
        entity["stop_tween"] = [](const ScriptEntity& e, u32 id) { return stop_tween(EcsEntity{e.live()}, id); };
        entity["stop_tweens"] = [](const ScriptEntity& e, sol::optional<std::string> property) {
            return stop_tweens(EcsEntity{e.live()}, property.value_or(""));
        };
        // Its timers: fn(self) after `seconds`, or every `seconds` (until cancelled, or it stops running its script).
        const auto timer = [this](const ScriptEntity& e, f32 seconds, const sol::protected_function& fn, bool repeat) -> u32 {
            const u64 id = e.live().id();
            const auto run = running.find(id);
            if (run == running.end()) {
                throw sol::error("timers are a running script's");
            }
            std::function<void()> action = [this, id, fn, repeat]() {
                const auto found = running.find(id);
                if (found == running.end() || !found->second.self.valid()) {
                    return;
                }
                const auto file = files.find(found->second.path);
                if (file != files.end() && world && world->raw().is_alive(id)) {
                    file->second.script->call_function(fn, repeat ? "every" : "after", found->second.self);
                }
            };
            return repeat ? run->second.timers.every(seconds, std::move(action)) : run->second.timers.after(seconds, std::move(action));
        };
        entity["after"] = [timer](const ScriptEntity& e, f32 seconds, const sol::protected_function& fn) { return timer(e, seconds, fn, false); };
        entity["every"] = [timer](const ScriptEntity& e, f32 seconds, const sol::protected_function& fn) { return timer(e, seconds, fn, true); };
        entity["cancel"] = [this](const ScriptEntity& e, u32 timer_id) {
            const auto run = running.find(e.entity.id());
            return run != running.end() && run->second.timers.cancel(timer_id);
        };
        // Its Animator's.
        const auto animator = [](const ScriptEntity& e) {
            Animator* found = EcsEntity{e.live()}.get_mut<Animator>();
            if (!found) {
                throw sol::error("the entity has no Animator");
            }
            return found;
        };
        entity["play"] = [animator](const ScriptEntity& e, const std::string& animation) { animator(e)->animation = animation; };
        entity["set_param"] = [animator](const ScriptEntity& e, const std::string& name, const sol::object& value) {
            Animator* found = animator(e);
            if (value.get_type() == sol::type::boolean) {
                found->params.set_bool(name, value.as<bool>());
            } else if (value.get_type() == sol::type::number) {
                found->params.set_float(name, value.as<f32>());
            } else {
                throw sol::error("set_param takes true, false or a number");
            }
        };
        entity["trigger"] = [animator](const ScriptEntity& e, const std::string& name) { animator(e)->params.set_trigger(name); };
        entity["state"] = [animator](const ScriptEntity& e) { return animator(e)->state; };

        sol::table input_table = lua.create_named_table("input");
        input_table["held"] = [this](const std::string& action) { return input && input->held(action); };
        input_table["pressed"] = [this](const std::string& action) { return input && input->pressed(action); };
        input_table["released"] = [this](const std::string& action) { return input && input->released(action); };
        input_table["axis"] = [this](const std::string& name) { return input ? input->axis(name) : 0.0f; };
        input_table["mouse"] = [this]() {
            const Vec2f at = input ? input->mouse_pos() : Vec2f{};
            return std::make_tuple(at.x, at.y);
        };

        // The language, which game scripts may change (a language menu's).
        if (sol::optional<sol::table> l10n = lua["l10n"]) {
            (*l10n)["set_locale"] = [](const std::string& tag) -> std::string {
                Localization* active = active_localization();
                return active ? active->set_locale(tag) : std::string{};
            };
        }

        sol::table ui_table = lua.create_named_table("ui");
        ui_table["clicked"] = [this](const std::string& id) { return screens && screens->clicked(id); };
        ui_table["changed"] = [this](const std::string& id) { return screens && screens->changed(id); };
        ui_table["value"] = [this](const std::string& id) { return screens ? screens->value(id) : 0.0f; };
        ui_table["checked"] = [this](const std::string& id) { return screens && screens->checked(id); };
        ui_table["text"] = [this](const std::string& id) { return screens ? screens->text(id) : std::string(); };
        ui_table["set_text"] = [this](const std::string& id, const sol::object& text) {
            if (screens) {
                screens->set_text(id, text.is<std::string>() ? text.as<std::string>()
                                                            : text.is<double>() ? format_number(text.as<double>()) : std::string());
            }
        };
        ui_table["set_value"] = [this](const std::string& id, f32 value) {
            if (screens) screens->set_value(id, value);
        };
        ui_table["set_checked"] = [this](const std::string& id, bool checked) {
            if (screens) screens->set_checked(id, checked);
        };
        ui_table["show"] = [this](const std::string& id, sol::optional<bool> shown) {
            if (screens) screens->set_visible(id, shown.value_or(true));
        };
        ui_table["hide"] = [this](const std::string& id) {
            if (screens) screens->set_visible(id, false);
        };
        ui_table["set_image"] = [this](const std::string& id, const std::string& path) {
            if (screens) screens->set_image(id, path);
        };
        ui_table["set_enabled"] = [this](const std::string& id, bool enabled) {
            if (screens) screens->set_enabled(id, enabled);
        };

        sol::table dialogue_table = lua.create_named_table("dialogue");
        dialogue_table["start"] = [this](const std::string& path, sol::optional<std::string> node) {
            return dialogue && dialogue->start(path, node.value_or(""));
        };
        dialogue_table["stop"] = [this]() {
            if (dialogue) dialogue->stop();
        };
        dialogue_table["active"] = [this]() { return dialogue && dialogue->active(); };
        dialogue_table["advance"] = [this]() {
            if (dialogue) dialogue->advance();
        };
        dialogue_table["choose"] = [this](const sol::object& which) {
            if (!dialogue) return false;
            if (which.is<std::string>()) return dialogue->choose(std::string_view(which.as<std::string>()));
            return which.is<double>() && dialogue->choose(static_cast<i32>(which.as<double>()) - 1); // Lua counts from 1
        };
        dialogue_table["node"] = [this]() { return dialogue && dialogue->active() ? dialogue->view().node_id : std::string(); };
        dialogue_table["get"] = [this](sol::this_state state, const std::string& name) -> sol::object {
            if (!dialogue) return sol::make_object(state, sol::lua_nil);
            const DialogueVariable value = dialogue->variable(name);
            if (const auto* b = std::get_if<bool>(&value.value)) return sol::make_object(state, *b);
            if (const auto* n = std::get_if<f64>(&value.value)) return sol::make_object(state, *n);
            return sol::make_object(state, std::get<std::string>(value.value));
        };
        dialogue_table["set"] = [this](const std::string& name, const sol::object& value) {
            if (!dialogue) return;
            if (value.is<bool>()) dialogue->set_variable(name, {.value = value.as<bool>()});
            else if (value.is<double>()) dialogue->set_variable(name, {.value = value.as<double>()});
            else if (value.is<std::string>()) dialogue->set_variable(name, {.value = value.as<std::string>()});
        };

        sol::table scene_table = lua.create_named_table("scene");
        scene_table["go"] = [this](const std::string& path, sol::optional<std::string> arrive, sol::optional<std::string> transition,
                                   sol::optional<f32> duration) {
            if (!flow) {
                return false;
            }
            SceneTravel travel{.scene = path, .arrive = arrive.value_or("")};
            if (transition) {
                if (*transition == "none") {
                    travel.transition.reset();
                } else if (const std::optional<TransitionKind> kind = transition_from_name(*transition)) {
                    travel.transition = kind;
                } else {
                    throw sol::error("no transition is called '" + *transition + "'");
                }
            }
            travel.duration = std::max(duration.value_or(travel.duration), 0.0f);
            flow->go(std::move(travel));
            return true;
        };
        scene_table["going"] = [this]() { return flow && flow->pending(); };

        sol::table world_table = lua.create_named_table("world");
        world_table["find"] = [this](const std::string& name) -> std::optional<ScriptEntity> {
            const EcsEntity found = world ? world->entities().find_by_name(name) : EcsEntity{};
            return found.valid() ? std::optional(ScriptEntity{world, found.raw()}) : std::nullopt;
        };
        world_table["spawn"] = [this](const std::string& prefab, sol::optional<f32> x, sol::optional<f32> y) -> std::optional<ScriptEntity> {
            const std::shared_ptr<const PrefabAsset> asset = prefab_of(prefab);
            if (!asset || !world) {
                throw sol::error("the prefab '" + prefab + "' could not be read");
            }
            const PrefabInstance made = instantiate_prefab(*world, *asset, PrefabSpawn{.pos = {x.value_or(0.0f), y.value_or(0.0f)}},
                                                           world->components());
            if (!made.root.valid()) {
                return std::nullopt;
            }
            made.root.raw().set<SpawnedPrefab>({.prefab = prefab}); // a save makes it again from it
            return ScriptEntity{world, made.root.raw()};
        };

        sol::table camera_table = lua.create_named_table("camera");
        camera_table["shake"] = [this](f32 trauma) {
            if (world) {
                shake_camera(*world, trauma);
            }
        };
        camera_table["trauma"] = [this]() {
            f32 most = 0.0f;
            if (world) {
                world->raw().each([&](const CameraComponent& camera) {
                    if (camera.current) {
                        most = std::max(most, camera.trauma);
                    }
                });
            }
            return most;
        };

#if KIN_ENABLE_PHYSICS
        bind_physics(lua);
#endif

        sol::table save_table = lua.create_named_table("save");
        const auto saves_or_throw = [this]() -> SceneSaves& {
            if (!saves) {
                throw sol::error("the game keeps no saves");
            }
            return *saves;
        };
        save_table["write"] = [this, saves_or_throw](const std::string& slot, sol::optional<std::string> title,
                                                     sol::optional<std::string> summary, sol::this_state state) {
            SceneSaves& kept = saves_or_throw();
            if (!world) {
                throw sol::error("save.write: no world");
            }
            const SaveResult written = kept.write(*world, slot, title.value_or(slot), summary.value_or(""));
            sol::state_view lua(state);
            if (written.ok) {
                return std::make_tuple(sol::make_object(lua, true), sol::make_object(lua, sol::lua_nil));
            }
            return std::make_tuple(sol::make_object(lua, sol::lua_nil), sol::make_object(lua, written.message));
        };
        save_table["read"] = [saves_or_throw](const std::string& slot) {
            SceneSaves& kept = saves_or_throw();
            if (!valid_save_id(slot)) {
                return std::make_tuple(false, std::string("'" + slot + "' is not a slot's name"));
            }
            if (!kept.store().slot_exists(slot)) {
                return std::make_tuple(false, std::string("no save is called '" + slot + "'"));
            }
            kept.ask_read(slot);
            return std::make_tuple(true, std::string());
        };
        save_table["exists"] = [saves_or_throw](const std::string& slot) {
            return valid_save_id(slot) && saves_or_throw().store().slot_exists(slot);
        };
        save_table["delete"] = [saves_or_throw](const std::string& slot) {
            return valid_save_id(slot) && saves_or_throw().store().delete_slot(slot).ok;
        };
        save_table["slots"] = [saves_or_throw](sol::this_state state) {
            sol::state_view lua(state);
            std::vector<SaveSlotInfo> slots = saves_or_throw().store().list_slots();
            std::ranges::stable_sort(slots, [](const SaveSlotInfo& a, const SaveSlotInfo& b) { return a.updated_at_unix > b.updated_at_unix; });
            sol::table listed = lua.create_table(static_cast<int>(slots.size()), 0);
            for (std::size_t i = 0; i < slots.size(); ++i) {
                listed[i + 1] = lua.create_table_with("id", slots[i].slot_id, "title", slots[i].title, "summary", slots[i].summary,
                                                      "updated", static_cast<f64>(slots[i].updated_at_unix));
            }
            return listed;
        };
        save_table["get"] = [saves_or_throw](const std::string& name, sol::object fallback, sol::this_state state) -> sol::object {
            const JsonValue* value = saves_or_throw().value(name);
            return value ? lua_of(state, *value) : fallback;
        };
        save_table["set"] = [saves_or_throw](const std::string& name, sol::object value) {
            SceneSaves& kept = saves_or_throw();
            if (value.get_type() == sol::type::lua_nil || value.get_type() == sol::type::none) {
                kept.set_value(name, JsonValue{});
                return;
            }
            std::optional<JsonValue> json = json_of(value);
            if (!json) {
                throw sol::error("save.set: '" + name + "' is not kept (true/false, numbers, text and tables of them are)");
            }
            kept.set_value(name, std::move(*json));
        };
        lua.safe_script(self_prelude);
        scripting_detail::record_lua_type<ScriptEntity>(lua, "Entity");
    }

    std::shared_ptr<const PrefabAsset> prefab_of(const std::string& path) {
        if (const auto found = prefabs.find(path); found != prefabs.end()) {
            return found->second;
        }
        const PrefabLoadResult loaded = load_prefab_asset(config.content / path, world->components());
        std::shared_ptr<const PrefabAsset> asset = loaded.prefab ? std::make_shared<const PrefabAsset>(*loaded.prefab) : nullptr;
        prefabs.emplace(path, asset);
        return asset;
    }

#if KIN_ENABLE_PHYSICS
    // The entity a body is: its body's, or its tilemap's solid cells'.
    flecs::entity entity_of(PhysicsBody body) const {
        if (!physics || !world || !body) {
            return {};
        }
        const u64 id = physics->user_id(body);
        if (id == 0 || !world->raw().is_alive(id)) {
            return {};
        }
        const flecs::entity found = world->raw().entity(id);
        if (const PhysicsBodyComponent* owned = found.get<PhysicsBodyComponent>(); owned && owned->body == body) {
            return found;
        }
        if (const TilemapCollider* cells = found.get<TilemapCollider>(); cells && cells->body == body) {
            return found;
        }
        return {};
    }

    // A query's options: {mask = bits, sensors = true, ignore = an entity (or self)}.
    static PhysicsFilter filter_of(const sol::optional<sol::table>& options) {
        PhysicsFilter filter{.include_sensors = false};
        if (!options) {
            return filter;
        }
        if (const sol::optional<double> mask = (*options)["mask"]) {
            if (*mask < 0.0 || *mask > 65535.0) {
                throw sol::error("physics: mask takes bits, 0 to 65535");
            }
            filter.mask_bits = static_cast<u16>(*mask);
        }
        if (const sol::optional<bool> sensors = (*options)["sensors"]) {
            filter.include_sensors = *sensors;
        }
        sol::object ignore = (*options)["ignore"];
        if (ignore.is<sol::table>()) {
            ignore = ignore.as<sol::table>()["entity"]; // a script's self
        }
        if (ignore.is<ScriptEntity>()) {
            const ScriptEntity& other = ignore.as<const ScriptEntity&>();
            if (other.alive()) {
                if (const PhysicsBodyComponent* body = other.entity.get<PhysicsBodyComponent>()) {
                    filter.ignore = body->body;
                }
            }
        }
        return filter;
    }

    PhysicsWorld& physics_or_throw() const {
        if (!physics) {
            throw sol::error("the game gives its scripts no physics");
        }
        return *physics;
    }

    // A hit as scripts see it.
    sol::table hit_table(sol::state_view lua, PhysicsBody body, Vec2f point, Vec2f normal, f32 fraction) const {
        sol::table hit = lua.create_table();
        if (const flecs::entity entity = entity_of(body)) {
            hit["entity"] = ScriptEntity{world, entity};
        }
        hit["x"] = point.x;
        hit["y"] = point.y;
        hit["normal"] = lua.create_table_with("x", normal.x, "y", normal.y);
        hit["fraction"] = fraction;
        return hit;
    }

    // The entities of `fixtures`, each once, as a list.
    sol::table entities_table(sol::state_view lua, const std::vector<PhysicsFixture>& fixtures) const {
        sol::table list = lua.create_table();
        std::vector<u64> seen;
        int n = 0;
        for (const PhysicsFixture fixture : fixtures) {
            const flecs::entity entity = entity_of(physics->body_of(fixture));
            if (entity && std::ranges::find(seen, entity.id()) == seen.end()) {
                seen.push_back(entity.id());
                list[++n] = ScriptEntity{world, entity};
            }
        }
        return list;
    }

    void bind_physics(sol::state& lua) {
        sol::table table = lua.create_named_table("physics");
        table["raycast"] = [this](f32 x1, f32 y1, f32 x2, f32 y2, sol::optional<sol::table> options, sol::this_state state) -> sol::object {
            const std::vector<PhysicsRaycastHit> hits = physics_or_throw().raycast({x1, y1}, {x2, y2}, filter_of(options));
            if (hits.empty()) {
                return sol::lua_nil;
            }
            const PhysicsRaycastHit& hit = hits.front();
            return hit_table(state, hit.body, hit.point, hit.normal, hit.fraction);
        };
        const auto cast = [this](const PhysicsShape& shape, Vec2f from, Vec2f to, const sol::optional<sol::table>& options,
                                 sol::this_state state) -> sol::object {
            const std::optional<PhysicsShapeHit> hit = physics_or_throw().cast_shape(shape, from, to, filter_of(options));
            if (!hit) {
                return sol::lua_nil;
            }
            return hit_table(state, hit->body, hit->point, hit->normal, hit->fraction);
        };
        table["cast_circle"] = [cast](f32 x1, f32 y1, f32 x2, f32 y2, f32 radius, sol::optional<sol::table> options, sol::this_state state) {
            return cast(PhysicsShape::circle(radius), {x1, y1}, {x2, y2}, options, state);
        };
        table["cast_box"] = [cast](f32 x1, f32 y1, f32 x2, f32 y2, f32 width, f32 height, sol::optional<sol::table> options,
                                   sol::this_state state) {
            return cast(PhysicsShape::box({0.5f * width, 0.5f * height}), {x1, y1}, {x2, y2}, options, state);
        };
        table["overlap_point"] = [this](f32 x, f32 y, sol::optional<sol::table> options, sol::this_state state) {
            return entities_table(state, physics_or_throw().query_point({x, y}, filter_of(options)));
        };
        table["overlap_circle"] = [this](f32 x, f32 y, f32 radius, sol::optional<sol::table> options, sol::this_state state) {
            return entities_table(state, physics_or_throw().query_shape(PhysicsShape::circle(radius), {x, y}, filter_of(options)));
        };
        table["overlap_box"] = [this](f32 x, f32 y, f32 width, f32 height, sol::optional<sol::table> options, sol::this_state state) {
            return entities_table(state,
                                  physics_or_throw().query_shape(PhysicsShape::box({0.5f * width, 0.5f * height}), {x, y}, filter_of(options)));
        };
    }
#endif

    std::unique_ptr<LuaScript> make_script(const std::string& path, std::shared_ptr<bool> staging = {}) {
        return std::make_unique<LuaScript>(LuaScriptOptions{
            .setup =
                [this, staging](sol::state& lua) {
                    bind(lua);
                    if (!staging)
                        return;
                    // Proxies keep service table identity stable while withholding
                    // live host access throughout candidate load and migration.
                    for (const char* name :
                         {"world", "input", "camera", "ui", "dialogue", "save", "scene", "physics", "tweens",
                          "input_contexts", "rebind", "pools", "preload", "controllers", "asset_memory", "l10n"}) {
                        sol::object original = lua[name];
                        if (original.get_type() != sol::type::table)
                            continue;
                        sol::table proxy = lua.create_table();
                        sol::table meta = lua.create_table();
                        meta["__index"] = [staging, original](const sol::object&,
                                                              const sol::object& key) -> sol::object {
                            if (*staging)
                                throw sol::error("live services are unavailable while preparing reload");
                            return original.as<sol::table>()[key];
                        };
                        meta["__newindex"] = [](const sol::object&, const sol::object&, const sol::object&) {
                            throw sol::error("engine service tables are read-only");
                        };
                        meta["__metatable"] = false;
                        proxy[sol::metatable_key] = meta;
                        lua[name] = proxy;
                    }
                },
            .module_root = config.content,
            .instruction_limit = config.runtime_limits.instruction_limit,
            .memory_limit_bytes = config.runtime_limits.memory_limit_bytes,
            .on_error =
                [this, path](std::string_view message) {
                    KIN_LOG_WARN_F("script", "script error",
                                   (LogFields{{.name = "script", .value = path},
                                              {.name = "error", .value = std::string(message)}}));
                    problems[path] = std::string(message);
                },
            .on_profile =
                [](std::string_view name, u64 ns) {
                    if (auto* session = current_profile_session())
                        session->record(name, "lua", ns);
                },
        });
    }
    void remember_times(const std::string& path, File& made) {
        made.times.clear();
        for (const auto& dependency : made.script->dependencies())
            made.times.emplace_back(dependency, time_of(dependency));
        const auto main = config.content / path;
        if (std::ranges::none_of(made.times, [&](const auto& entry) { return entry.first == main; }))
            made.times.emplace_back(main, time_of(main));
    }
    File& file(const std::string& path) {
        auto [found, fresh] = files.try_emplace(path);
        File& made = found->second;
        if (fresh) {
            made.script = make_script(path);
            if (made.script->load_file(config.content / path)) {
                problems.erase(path);
                ++made.generation;
            }
            remember_times(path, made);
        }
        return made;
    }
    bool reload(const std::string& path, File& made) {
        if (!made.script->loaded()) {
            if (made.script->load_file(config.content / path)) {
                problems.erase(path);
                ++made.generation;
                remember_times(path, made);
                return true;
            }
            remember_times(path, made);
            return false;
        }
        auto* previous_world = world;
        world = bound_world;
        struct WorldScope {
            EcsWorld*& world;
            EcsWorld* previous;
            ~WorldScope() { world = previous; }
        } world_scope{world, previous_world};
        auto staging = std::make_shared<bool>(true);
        auto next = make_script(path, staging);
        struct Guard {
            bool previous = scripting_detail::reload_staging;
            Guard() { scripting_detail::reload_staging = true; }
            ~Guard() { scripting_detail::reload_staging = previous; }
        } guard;
        std::map<u64, sol::table> selves; // destroyed before candidate on rejection
        try {
            auto limits = config.runtime_limits;
            limits.instruction_limit =
                std::min<u64>(limits.instruction_limit ? limits.instruction_limit : 1'000'000, 1'000'000);
            next->runtime().set_limits(limits);
            if (!next->load_file(config.content / path))
                throw std::runtime_error(next->error());
            std::vector<u64> ids;
            for (const auto& [id, run] : running)
                if (run.path == path)
                    ids.push_back(id);
            std::ranges::sort(ids);
            for (u64 id : ids) {
                auto& run = running.at(id);
                const auto copied = scripting_detail::copy_lua_data(
                                        sol::make_object(run.self.lua_state(), run.self), next->lua(),
                                        [&](const sol::object& value) -> std::optional<sol::object> {
                                            if (!value.is<ScriptEntity>())
                                                return {};
                                            auto entity = value.as<ScriptEntity>();
                                            if (!entity.alive())
                                                throw std::runtime_error("reload state contains a stale entity");
                                            return sol::make_object(next->lua(), entity);
                                        },
                                        true)
                                        .as<sol::table>();
                auto result = next->call_for<sol::table>("__kin_self", ScriptEntity{world, world->raw().entity(id)});
                if (!result)
                    throw std::runtime_error(next->error().empty() ? "script self factory is missing" : next->error());
                sol::table self = *result;
                for (const auto& [key, value] : copied)
                    self[key] = value;
                if (config.runtime_limits.memory_limit_bytes &&
                    next->runtime().stats().memory_bytes > config.runtime_limits.memory_limit_bytes)
                    throw std::runtime_error("entity reload exceeds Lua memory limit");
                selves.emplace(id, self);
                next->set_task_owner(std::to_string(id) + ":" + std::to_string(run.lifetime));
                if (next->has("on_reload") && !next->call("on_reload", self))
                    throw std::runtime_error(next->error());
            }
            next->runtime().set_limits(config.runtime_limits);
            *staging = false;
            // No fallible Lua code after this point. Old selves and tasks are
            // released before the old VM, after every migration has succeeded.
            ++made.generation;
            for (auto& [id, self] : selves) {
                auto& run = running.at(id);
                run.self = std::move(self);
                run.generation = made.generation;
            }
            made.script.swap(next);
            problems.erase(path);
            remember_times(path, made);
            return true;
        } catch (const std::exception& e) {
            problems[path] = "reload: " + std::string(e.what());
            remember_times(path, made); // suppress unchanged bad edits
            return false;
        }
    }
    void stop(const Running& run, std::string_view reason) {
        auto* previous_world = world;
        world = bound_world;
        struct WorldScope {
            EcsWorld*& world;
            EcsWorld* previous;
            ~WorldScope() { world = previous; }
        } world_scope{world, previous_world};
        auto found = files.find(run.path);
        if (found == files.end())
            return;
        auto& script = *found->second.script;
        const auto owner = std::to_string(run.entity_id) + ":" + std::to_string(run.lifetime);
        script.set_task_owner(owner);
        if (run.started && script.has("on_stop"))
            script.call("on_stop", run.self, std::string(reason));
        script.cancel_tasks(owner);
    }
    void reload_changed() {
        // Stable order makes migration and task registration reproducible.
        std::vector<std::string> paths;
        for (auto& [path, made] : files)
            if (std::ranges::any_of(made.times, [](const auto& file) { return time_of(file.first) != file.second; }))
                paths.push_back(path);
        std::ranges::sort(paths);
        for (const auto& path : paths)
            reload(path, files.at(path));
        prefabs.clear();
    }
};

EntityScripts::EntityScripts(EntityScriptsConfig config) : _impl(std::make_unique<Impl>()) {
    _impl->config = std::move(config);
}

EntityScripts::~EntityScripts() {
    if (_impl->saves) {
        _impl->saves->set_scripts({}); // its hooks go with it
    }
    _impl->running.clear(); // the selves before the states they live in
}

const std::map<std::string, std::string>& EntityScripts::problems() const {
    return _impl->problems;
}

std::size_t EntityScripts::running() const {
    return _impl->running.size();
}

void EntityScripts::restart() {
    for (auto& [id, run] : _impl->running)
        _impl->stop(run, "restart");
    _impl->running.clear();
}

bool EntityScripts::reload(std::string_view asset) {
    const std::string path(asset);
    auto found = _impl->files.find(path);
    if (found == _impl->files.end())
        return false;
    return _impl->reload(path, found->second);
}
std::map<std::string, std::vector<std::string>> EntityScripts::api_surface() const {
    auto script = _impl->make_script("<api>");
    return scripting_detail::lua_api_surface(script->lua());
}
std::map<std::string, LuaRuntimeStats> EntityScripts::runtime_stats() const {
    std::map<std::string, LuaRuntimeStats> stats;
    for (const auto& [path, file] : _impl->files)
        stats.emplace(path, file.script->runtime().stats());
    return stats;
}
void EntityScripts::set_screens(SceneScreens* screens) {
    _impl->screens = screens;
}

void EntityScripts::set_dialogue(SceneDialogue* dialogue) {
    _impl->dialogue = dialogue;
}

void EntityScripts::set_flow(SceneFlow* flow) {
    _impl->flow = flow;
}

void EntityScripts::set_physics(PhysicsWorld* physics) {
    _impl->physics = physics;
}

void EntityScripts::set_saves(SceneSaves* saves) {
    Impl& impl = *_impl;
    impl.saves = saves;
    impl.restored.clear();
    if (!saves) {
        return;
    }
    saves->set_scripts({
        .write = [&impl](EcsEntity entity) -> std::optional<JsonValue> {
            const auto found = impl.running.find(entity.id());
            if (found == impl.running.end() || !found->second.self.valid()) {
                return std::nullopt;
            }
            JsonValue::Object values;
            found->second.self.for_each([&](const sol::object& key, const sol::object& value) {
                if (key.get_type() != sol::type::string) {
                    return;
                }
                std::string name = key.as<std::string>();
                if (name == "entity") {
                    return; // its entity (methods), not a value
                }
                if (std::optional<JsonValue> kept = json_of(value)) {
                    values.emplace(std::move(name), std::move(*kept));
                }
            });
            return JsonValue{std::move(values)};
        },
        .read = [&impl](EcsEntity entity, const JsonValue& values) {
            if (values.is_object()) {
                impl.restored[entity.id()] = values;
            }
        },
    });
}

void EntityScripts::set_workflows(std::shared_ptr<ScriptWorkflows2D> workflows) {
    _impl->workflows = std::move(workflows);
}
std::shared_ptr<ScriptWorkflows2D> EntityScripts::workflows() const {
    return _impl->workflows;
}
void EntityScripts::update(EcsWorld& world, Input& input, f32 dt) {
    Impl& impl = *_impl;
    impl.bound_world = &world;
    impl.world = &world;
    impl.input = &input;
    if (!impl.workflows)
        impl.workflows = std::make_shared<ScriptWorkflows2D>(world, impl.config.content);
    if (&impl.workflows->world() != &world)
        throw std::invalid_argument("workflow services belong to another world");
    impl.workflows->advance(input, dt);
    if (impl.config.check_frames > 0 && ++impl.frames >= impl.config.check_frames) {
        impl.frames = 0;
        impl.reload_changed();
    }
    // The scripted entities now (a script may make or destroy some as it runs).
    std::vector<std::pair<flecs::entity, std::string>> scripted;
    world.raw().each([&](flecs::entity entity, const ScriptComponent& script) {
        if (script.enabled && !script.asset.id.empty() && entity.enabled()) {
            scripted.emplace_back(entity, script.asset.id);
        }
    });
    // Values given back to entities gone since (or running no script): let go.
    std::erase_if(impl.restored, [&](const auto& entry) { return !world.raw().is_alive(entry.first); });
    // Those that no longer run theirs: gone.
    std::erase_if(impl.running, [&](const auto& entry) {
        const auto found = std::ranges::find_if(scripted, [&](const auto& item) { return item.first.id() == entry.first; });
        const bool remove = found == scripted.end() || found->second != entry.second.path;
        if (remove)
            impl.stop(entry.second, !world.raw().is_alive(entry.first) ? "destroyed" : "disabled_or_removed");
        return remove;
    });
    std::ranges::sort(scripted, [](const auto& a, const auto& b) { return a.first.id() < b.first.id(); });
    for (const auto& [entity, path] : scripted) {
        if (!entity.is_alive()) {
            continue;
        }
        Impl::File& file = impl.file(path);
        if (!file.script->loaded()) {
            continue; // it does not read: its problem is told
        }
        auto [found, fresh] = impl.running.try_emplace(entity.id());
        Impl::Running& run = found->second;
        const auto* lease = entity.get<EntityLifetime>();
        const u64 lifetime = lease ? lease->generation : 0;
        if (fresh || run.generation != file.generation || run.lifetime != lifetime) {
            if (!fresh && run.self.valid())
                impl.stop(run, "replaced");
            // Its self, in the file's state as read now.
            run = {.path = path, .generation = file.generation, .self = {}, .started = false};
            run.lifetime = lifetime;
            run.entity_id = entity.id();
            auto made = file.script->call_for<sol::table>("__kin_self", ScriptEntity{&world, entity});
            if (!made) {
                impl.problems[path] =
                    file.script->error().empty() ? "script self factory is missing" : file.script->error();
                continue;
            }
            run.self = *made;
        }
        file.script->set_task_owner(std::to_string(entity.id()) + ":" + std::to_string(lifetime));
        if (!run.started) {
            run.started = true;
            file.script->call("on_start", run.self);
        }
        // What a save gave back: its self's again, then on_load.
        if (const auto given = impl.restored.find(entity.id()); given != impl.restored.end()) {
            const JsonValue values = std::move(given->second);
            impl.restored.erase(given);
            if (entity.is_alive() && run.self.valid()) {
                sol::state_view lua(run.self.lua_state());
                for (const auto& [name, value] : values.members()) {
                    if (name != "entity") {
                        run.self[name] = lua_of(lua, value);
                    }
                }
                if (file.script->has("on_load")) {
                    file.script->call("on_load", run.self);
                }
            }
        }
        if (entity.is_alive()) {
            file.script->call("on_update", run.self, dt);
        }
        // Its tweens that got to their end, and its timers due.
        if (entity.is_alive()) {
            const Tweens* tweens = entity.get<Tweens>();
            std::vector<std::pair<u32, sol::protected_function>> finished;
            for (auto it = run.tween_done.begin(); it != run.tween_done.end();) {
                const bool ended = tweens && std::ranges::find(tweens->ended, it->first) != tweens->ended.end();
                const bool going = tweens && std::ranges::any_of(tweens->running, [&](const Tweens::Running& r) { return r.id == it->first; });
                if (ended) {
                    finished.emplace_back(it->first, std::move(it->second));
                }
                it = ended || !going ? run.tween_done.erase(it) : std::next(it);
            }
            for (const auto& [id, done] : finished) {
                if (entity.is_alive()) {
                    file.script->call_function(done, "tween done", run.self);
                }
            }
        }
        if (entity.is_alive()) {
            if (const auto found = impl.running.find(entity.id()); found != impl.running.end()) {
                found->second.timers.update(dt);
            }
        }
        // What the dialogue told this step (the game clears them once the step has seen them).
        if (impl.dialogue && entity.is_alive() && file.script->has("on_dialogue_event")) {
            const std::vector<DialogueEvent> told = impl.dialogue->events();
            for (const DialogueEvent& event : told) {
                if (!entity.is_alive()) break;
                file.script->call("on_dialogue_event", run.self, event.id, event.node_id);
            }
        }
#if KIN_ENABLE_PHYSICS
        // What its body began touching this step, and stopped touching.
        for (const auto& [type, callback] : {std::pair{PhysicsContactEventType::Begin, "on_contact"},
                                             std::pair{PhysicsContactEventType::End, "on_contact_end"}}) {
            if (!entity.is_alive() || !file.script->has(callback)) {
                continue;
            }
            const PhysicsContactQueue* queue = entity.get<PhysicsContactQueue>();
            if (!queue || queue->events.empty()) {
                continue;
            }
            const PhysicsBodyComponent* mine = entity.get<PhysicsBodyComponent>();
            std::vector<PhysicsBody> touched;
            for (const PhysicsContactEvent& event : queue->events) {
                if (event.type == type) {
                    touched.push_back(mine && event.body_a == mine->body ? event.body_b : event.body_a);
                }
            }
            for (const PhysicsBody body : touched) {
                flecs::entity other;
                world.raw().each([&](flecs::entity candidate, const PhysicsBodyComponent& component) {
                    if (!other && component.body == body) {
                        other = candidate;
                    }
                });
                if (!other) { // a tilemap's solid cells
                    world.raw().each([&](flecs::entity candidate, const TilemapCollider& component) {
                        if (!other && component.body == body) {
                            other = candidate;
                        }
                    });
                }
                if (other && entity.is_alive()) {
                    file.script->call(callback, run.self, ScriptEntity{&world, other});
                }
            }
        }
#endif
    }
    std::vector<std::string> paths;
    for (const auto& [path, file] : impl.files)
        paths.push_back(path);
    std::ranges::sort(paths);
    for (const auto& path : paths)
        impl.files.at(path).script->advance_tasks(dt, [&](std::string_view owner) {
            for (const auto& [id, run] : impl.running)
                if (owner == std::to_string(id) + ":" + std::to_string(run.lifetime)) {
                    const auto entity = world.raw().entity(id);
                    const auto* lease = entity.is_alive() ? entity.get<EntityLifetime>() : nullptr;
                    const auto* component = entity.is_alive() ? entity.get<ScriptComponent>() : nullptr;
                    return entity.is_alive() && entity.enabled() && component && component->enabled &&
                           component->asset.id == run.path && run.lifetime == (lease ? lease->generation : 0);
                }
            return false;
        });
    impl.world = nullptr;
    impl.input = nullptr;
}

} // namespace kin
