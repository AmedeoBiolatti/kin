#include <kin/scripting/entity_scripts.hpp>

#include "lua_api_surface.hpp"
#include "lua_reload_internal.hpp"
#include "lua_state_copy.hpp"
#include "lua_vm.hpp"
#include "lua_workflows2d_internal.hpp"

#include <kin/ecs/animator.hpp>
#include <kin/ecs/camera.hpp>
#include <kin/ecs/dialogue.hpp>
#include <kin/ecs/lifecycle.hpp>
#include <kin/ecs/particles.hpp>
#include <kin/ecs/render.hpp>
#include <kin/ecs/saving.hpp>
#include <kin/ecs/scene_flow.hpp>
#include <kin/ecs/script.hpp>
#include <kin/ecs/tween.hpp>
#include <kin/ecs/ui_screens.hpp>
#include <kin/ecs/world.hpp>
#include <kin/l10n/localization.hpp>
#include <kin/platform/input.hpp>
#include <kin/platform/log.hpp>
#include <kin/prefab/prefab_asset.hpp>
#include <kin/scripting/lua_script.hpp>
#include <kin/scripting/lua_values.hpp>
#include <kin/scripting/workflows2d.hpp>
#if KIN_ENABLE_PHYSICS
#include <kin/ecs/physics.hpp>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
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

    u64 lifetime = 0; // its incarnation (a pooled entity is used again)

    ScriptEntity(EcsWorld* w, flecs::entity e) : world(w), entity(e) {
        if (entity && entity.is_alive()) {
            lifetime = entity_lifetime(EcsEntity{entity});
        }
    }
    bool alive() const { return entity && same_lifetime(EcsEntity{entity}, lifetime); }
    flecs::entity live() const {
        if (scripting_detail::reload_staging)
            throw sol::error("live entity access is unavailable while preparing reload");
        if (!alive()) {
            throw sol::error("the entity is gone");
        }
        return entity;
    }
};

// Makes a script's `self`: a table of its own, with the entity's methods and
// the script's own: timers, and tweens with a done callback, run as the
// script's tasks (cancelled when it stops, dropped by a reload).
constexpr const char* self_prelude = R"lua(
__kin_self_methods = {}
local own = __kin_self_methods
function own.after(self, seconds, fn)
    assert(type(seconds) == "number" and seconds >= 0, "after takes seconds")
    assert(type(fn) == "function", "after takes a function")
    return task.run(function()
        task.wait(seconds)
        fn(self)
    end)
end
function own.every(self, seconds, fn)
    assert(type(seconds) == "number" and seconds > 0, "every takes a positive interval")
    assert(type(fn) == "function", "every takes a function")
    return task.run(function()
        while true do
            task.wait(seconds)
            fn(self)
        end
    end)
end
function own.cancel(self, timer)
    local state = timer:state()
    timer:cancel()
    return state == "pending" or state == "running" or state == "waiting"
end
function own.tween(self, property, to, seconds, easing, options)
    local id = self.entity:tween(property, to, seconds, easing, options)
    local given = type(easing) == "table" and easing or options
    local done = type(given) == "table" and given.done or nil
    if done ~= nil then
        assert(type(done) == "function", "tween: done takes a function")
        local tween = tweens.of(self.entity, id)
        task.run(function()
            task.until_(function() return tween:state() ~= "running" end)
            if tween:state() == "completed" then
                done(self)
            end
        end)
    end
    return id
end
function __kin_self(entity)
    return setmetatable({entity = entity}, {__index = function(_, key)
        local mine = own[key]
        if mine then
            return mine
        end
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

namespace {

// A live host service the bindings reach. While a reload candidate is being
// prepared every use throws, so a rejected candidate cannot have touched the
// game; get() is for the host's own code.
template <typename T> class LiveService {
public:
    LiveService& operator=(T* service) {
        _service = service;
        return *this;
    }
    T* get() const { return _service; }
    operator T*() const { return checked(); }
    T* operator->() const { return checked(); }
    T& operator*() const { return *checked(); }

private:
    T* checked() const {
        if (scripting_detail::reload_staging) {
            throw sol::error("live services are unavailable while preparing reload");
        }
        return _service;
    }

    T* _service = nullptr;
};

} // namespace

struct EntityScripts::Impl {
    EntityScriptsConfig config;
    std::shared_ptr<ScriptWorkflows2D> workflows; // the world's (ScriptWorkflows2D::shared)

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
        u64 lifetime = 0;  // its entity's incarnation
        u64 entity_id = 0; // its tasks' owner
    };
    std::map<std::string, File> files; // in order: reloads and tasks run reproducibly
    std::unordered_map<u64, Running> running;
    std::map<std::string, std::string> problems;
    std::unordered_map<std::string, std::shared_ptr<const PrefabAsset>> prefabs;
    i32 frames = 0;

    // What the bindings reach, during an update.
    LiveService<EcsWorld> world;
    EcsWorld* bound_world = nullptr; // the world of the last update (for reloads and stops between them)
    LiveService<Input> input;
    LiveService<SceneScreens> screens;
    LiveService<SceneDialogue> dialogue;
    LiveService<SceneFlow> flow;
    LiveService<SceneSaves> saves;
    LiveService<PhysicsWorld> physics;
    // Values given back by the saves, for entities once they start (by id).
    std::unordered_map<u64, JsonValue> restored;
    std::string calling; // the script being called, for its problems
    i32 player = -1;     // whose input the entity being run reads (Input's numbers: -1 anyone)

    // A player as a script names it (1 the first, 0 anyone; none: its entity's), as Input numbers them.
    i32 player_of(const sol::optional<i32>& named) const {
        return named ? (*named > 0 ? *named - 1 : -1) : player;
    }

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
        sol::usertype<ScriptEntity> entity =
            scripting_detail::new_api_type<ScriptEntity>(lua, "Entity", sol::no_constructor);
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
                [&](std::string_view name, ComponentFieldKind, const ComponentFieldValue& value) {
                    fields[std::string(name)] = component_value_to_lua(lua, value);
                });
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
                const std::optional<ComponentFieldValue> converted = component_value_from_lua(field->kind, value);
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
        // Its tweens (kin/ecs/tween.hpp, stepped with the world's script
        // services). self:tween calls this, then runs its `done` as a task.
        entity["tween"] = [](const ScriptEntity& e, const std::string& property, const sol::object& to, f32 seconds,
                             const sol::object& easing_or_options, const sol::object& options) {
            TweenDef def = scripting_detail::lua_tween_def(*e.world, property, to, seconds, easing_or_options, options);
            std::string error;
            const u32 id = kin::tween(*e.world, EcsEntity{e.live()}, std::move(def), &error);
            if (id == 0) {
                throw sol::error("tween: " + error);
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
        // Each takes a player last (1 the first, 0 anyone), or reads the
        // player its entity's ScriptComponent names.
        input_table["held"] = [this](const std::string& action, sol::optional<i32> who) {
            return input && input->held(player_of(who), action);
        };
        input_table["pressed"] = [this](const std::string& action, sol::optional<i32> who) {
            return input && input->pressed(player_of(who), action);
        };
        input_table["released"] = [this](const std::string& action, sol::optional<i32> who) {
            return input && input->released(player_of(who), action);
        };
        input_table["axis"] = [this](const std::string& name, sol::optional<i32> who) {
            return input ? input->axis(player_of(who), name) : 0.0f;
        };
        // Whose input this script's entity reads (0: anyone).
        input_table["player"] = [this]() { return player + 1; };
        // The fingers down, {id, x, y, start_x, start_y} each; two fingers'
        // pinch (1: none) and pan since the last step. Gestures are actions
        // an input file binds ("touch jump tap").
        input_table["touches"] = [this](sol::this_state state) {
            sol::state_view lua{state};
            sol::table list = lua.create_table();
            if (input) {
                i32 at = 1;
                for (const TouchPoint& t : input->touches()) {
                    list[at++] = lua.create_table_with("id", static_cast<f64>(t.id), "x", t.pos.x, "y", t.pos.y, "start_x", t.start.x,
                                                       "start_y", t.start.y);
                }
            }
            return list;
        };
        input_table["pinch"] = [this]() { return input ? input->touch_pinch() : 1.0f; };
        input_table["pan"] = [this]() {
            const Vec2f moved = input ? input->touch_pan() : Vec2f{};
            return std::make_tuple(moved.x, moved.y);
        };
        input_table["mouse"] = [this]() {
            const Vec2f at = input ? input->mouse_pos() : Vec2f{};
            return std::make_tuple(at.x, at.y);
        };

        // The language, which game scripts may change (a language menu's).
        if (sol::optional<sol::table> l10n = lua["l10n"]) {
            (*l10n)["set_locale"] = [](const std::string& tag) -> std::string {
                if (scripting_detail::reload_staging) {
                    throw sol::error("live services are unavailable while preparing reload");
                }
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
                world->each([&](const CameraComponent& camera) {
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
            return value ? json_to_lua(state, *value) : fallback;
        };
        save_table["set"] = [saves_or_throw](const std::string& name, sol::object value) {
            SceneSaves& kept = saves_or_throw();
            if (value.get_type() == sol::type::lua_nil || value.get_type() == sol::type::none) {
                kept.set_value(name, JsonValue{});
                return;
            }
            std::optional<JsonValue> json = lua_to_json(value);
            if (!json) {
                throw sol::error("save.set: '" + name + "' is not kept (true/false, numbers, text and tables of them are)");
            }
            kept.set_value(name, std::move(*json));
        };
        lua.safe_script(self_prelude);
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

    std::unique_ptr<LuaScript> make_script(const std::string& path) {
        return std::make_unique<LuaScript>(LuaScriptOptions{
            .setup = [this](sol::state& lua) { bind(lua); },
            .module_root = config.content,
            .limits = config.runtime_limits,
            .on_error =
                [this, path](std::string_view message) {
                    KIN_LOG_WARN_F("script", "script error",
                                   (LogFields{{.name = "script", .value = path},
                                              {.name = "error", .value = std::string(message)}}));
                    problems[path] = std::string(message);
                },
        });
    }

    void remember_times(const std::string& path, File& made) {
        made.times.clear();
        for (const auto& dependency : made.script->dependencies()) {
            made.times.emplace_back(dependency, time_of(dependency));
        }
        const auto main = config.content / path;
        if (std::ranges::none_of(made.times, [&](const auto& entry) { return entry.first == main; })) {
            made.times.emplace_back(main, time_of(main));
        }
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

    // The bindings reach the world of the last update while the host reloads
    // or stops scripts between updates.
    struct WorldScope {
        LiveService<EcsWorld>& world;
        EcsWorld* previous;
        ~WorldScope() { world = previous; }
    };
    WorldScope bind_world() {
        EcsWorld* previous = world.get();
        world = bound_world;
        return {world, previous};
    }

    bool reload(const std::string& path, File& made) {
        if (!made.script->loaded()) {
            const bool loaded = made.script->load_file(config.content / path);
            if (loaded) {
                problems.erase(path);
                ++made.generation;
            }
            remember_times(path, made);
            return loaded;
        }
        const WorldScope scope = bind_world();
        auto next = make_script(path);
        const scripting_detail::ReloadStaging staging;
        std::map<u64, sol::table> selves; // destroyed before the candidate on rejection
        try {
            {
                const LuaInstructionBudget budget{lua_maintenance_budget};
                if (!next->load_file(config.content / path)) {
                    throw std::runtime_error(next->error());
                }
            }
            std::vector<u64> ids;
            for (const auto& [id, run] : running) {
                if (run.path == path) {
                    ids.push_back(id);
                }
            }
            std::ranges::sort(ids);
            for (u64 id : ids) {
                // Each instance migrates under its own budget: copy its data,
                // make its self in the candidate, then on_reload(self).
                const LuaInstructionBudget budget{lua_maintenance_budget};
                auto& run = running.at(id);
                const auto copy_entity = [&](const sol::object& value) -> std::optional<sol::object> {
                    if (!value.is<ScriptEntity>()) {
                        return {};
                    }
                    auto entity = value.as<ScriptEntity>();
                    if (!entity.alive()) {
                        throw std::runtime_error("reload state contains a stale entity");
                    }
                    return sol::make_object(next->lua(), entity);
                };
                const auto copied = scripting_detail::copy_lua_data(sol::make_object(run.self.lua_state(), run.self),
                                                                    next->lua(), copy_entity, true)
                                        .as<sol::table>();
                auto result =
                    next->call_for<sol::table>("__kin_self", ScriptEntity{world.get(), world.get()->raw().entity(id)});
                if (!result) {
                    throw std::runtime_error(next->error().empty() ? "script self factory is missing" : next->error());
                }
                sol::table self = *result;
                for (const auto& [key, value] : copied) {
                    self[key] = value;
                }
                if (next->runtime().memory_exceeded()) {
                    throw std::runtime_error("entity reload exceeds Lua memory limit");
                }
                selves.emplace(id, self);
                next->set_task_owner(id);
                if (next->has("on_reload") && !next->call("on_reload", self)) {
                    throw std::runtime_error(next->error());
                }
            }
            // No fallible Lua code after this point. Old selves and tasks are
            // released with the old VM, after every migration has succeeded.
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

    // A running script stops: on_stop(self, reason), then its tasks end.
    void stop(const Running& run, std::string_view reason) {
        const WorldScope scope = bind_world();
        auto found = files.find(run.path);
        if (found == files.end()) {
            return;
        }
        auto& script = *found->second.script;
        script.set_task_owner(run.entity_id);
        if (run.started && script.has("on_stop")) {
            script.call("on_stop", run.self, std::string(reason));
        }
        script.cancel_tasks(run.entity_id);
    }

    void reload_changed() {
        for (auto& [path, made] : files) {
            if (std::ranges::any_of(made.times, [](const auto& file) { return time_of(file.first) != file.second; })) {
                reload(path, made);
            }
        }
        prefabs.clear();
    }

    // Whether a task's owner (an entity's id) still runs the script it started in.
    bool task_owner_alive(EcsWorld& world, u64 id) const {
        const auto found = running.find(id);
        if (found == running.end()) {
            return false;
        }
        const EcsEntity entity{world.raw().entity(id)};
        const ScriptComponent* component = entity ? entity.get<ScriptComponent>() : nullptr;
        return entity && entity.raw().enabled() && component && component->enabled &&
               component->asset.id == found->second.path && same_lifetime(entity, found->second.lifetime);
    }
};

EntityScripts::EntityScripts(EntityScriptsConfig config) : _impl(std::make_unique<Impl>()) {
    _impl->config = std::move(config);
}

EntityScripts::~EntityScripts() {
    if (_impl->saves) {
        _impl->saves->set_scripts({}); // its hooks go with it
    }
    if (_impl->workflows) {
        _impl->workflows->leave(this);
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
    for (auto& [id, run] : _impl->running) {
        _impl->stop(run, "restart");
    }
    _impl->running.clear();
}

bool EntityScripts::reload(std::string_view asset) {
    const std::string path(asset);
    auto found = _impl->files.find(path);
    if (found == _impl->files.end()) {
        return false;
    }
    return _impl->reload(path, found->second);
}

LuaApiSurface EntityScripts::api_surface() const {
    auto script = _impl->make_script("<api>");
    LuaApiSurface surface = script->api_surface();
    // self's own methods (the self prelude's), beside its entity's.
    auto& entity = surface.tables["Entity"];
    const sol::table own = script->lua()["__kin_self_methods"];
    for (const auto& [key, value] : own) {
        entity.push_back(key.as<std::string>());
    }
    std::ranges::sort(entity);
    entity.erase(std::unique(entity.begin(), entity.end()), entity.end());
    for (const char* callback : {"on_start", "on_load", "on_reload"}) {
        surface.callbacks[callback] = {"Entity"};
    }
    surface.callbacks["on_update"] = {"Entity", "number"};
    surface.callbacks["on_stop"] = {"Entity", "string"};
    surface.callbacks["on_contact"] = {"Entity", "Entity"};
    surface.callbacks["on_contact_end"] = {"Entity", "Entity"};
    surface.callbacks["on_dialogue_event"] = {"Entity", "string", "string"};
    return surface;
}

std::map<std::string, LuaRuntimeStats> EntityScripts::runtime_stats() const {
    std::map<std::string, LuaRuntimeStats> stats;
    for (const auto& [path, file] : _impl->files) {
        stats.emplace(path, file.script->runtime().stats());
    }
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
                if (std::optional<JsonValue> kept = lua_to_json(value)) {
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

void EntityScripts::update(EcsWorld& world, Input& input, f32 dt) {
    Impl& impl = *_impl;
    impl.bound_world = &world;
    impl.world = &world;
    impl.input = &input;
    if (!impl.workflows) {
        impl.workflows = ScriptWorkflows2D::shared(world, impl.config.content);
    }
    if (&impl.workflows->world() != &world) {
        throw std::invalid_argument("workflow services belong to another world");
    }
    impl.workflows->step(this, input, dt);
    if (impl.config.check_frames > 0 && ++impl.frames >= impl.config.check_frames) {
        impl.frames = 0;
        impl.reload_changed();
    }
    // The scripted entities now (a script may make or destroy some as it runs).
    std::vector<std::pair<flecs::entity, std::string>> scripted;
    world.each([&](flecs::entity entity, const ScriptComponent& script) {
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
        if (remove) {
            impl.stop(entry.second, !world.raw().is_alive(entry.first) ? "destroyed" : "disabled_or_removed");
        }
        return remove;
    });
    std::ranges::sort(scripted, [](const auto& a, const auto& b) { return a.first.id() < b.first.id(); });
    for (const auto& [entity, path] : scripted) {
        if (!entity.is_alive()) {
            continue;
        }
        const ScriptComponent* component = entity.get<ScriptComponent>();
        impl.player = component && component->player > 0 ? component->player - 1 : -1;
        Impl::File& file = impl.file(path);
        if (!file.script->loaded()) {
            continue; // it does not read: its problem is told
        }
        auto [found, fresh] = impl.running.try_emplace(entity.id());
        Impl::Running& run = found->second;
        const u64 lifetime = entity_lifetime(EcsEntity{entity});
        if (fresh || run.generation != file.generation || run.lifetime != lifetime) {
            if (!fresh && run.self.valid()) {
                impl.stop(run, "replaced");
            }
            // Its self, in the file's state as read now.
            run = {.path = path, .generation = file.generation, .lifetime = lifetime, .entity_id = entity.id()};
            auto made = file.script->call_for<sol::table>("__kin_self", ScriptEntity{&world, entity});
            if (!made) {
                impl.problems[path] =
                    file.script->error().empty() ? "script self factory is missing" : file.script->error();
                continue;
            }
            run.self = *made;
        }
        file.script->set_task_owner(entity.id());
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
                        run.self[name] = json_to_lua(lua, value);
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
                world.each([&](flecs::entity candidate, const PhysicsBodyComponent& component) {
                    if (!other && component.body == body) {
                        other = candidate;
                    }
                });
                if (!other) { // a tilemap's solid cells
                    world.each([&](flecs::entity candidate, const TilemapCollider& component) {
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
    for (auto& [path, file] : impl.files) {
        file.script->advance_tasks(dt, [&](u64 owner) { return impl.task_owner_alive(world, owner); });
    }
    impl.world = nullptr;
    impl.input = nullptr;
    impl.player = -1;
}

} // namespace kin
