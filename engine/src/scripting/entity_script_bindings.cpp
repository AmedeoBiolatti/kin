#include "entity_scripts_internal.hpp"

#include "lua_api_surface.hpp"
#include "lua_workflows2d_internal.hpp"

#include <kin/ecs/animator.hpp>
#include <kin/ecs/particles.hpp>
#include <kin/ecs/render.hpp>
#include <kin/ecs/tween.hpp>
#include <kin/scripting/lua_values.hpp>
#include <kin/scripting/workflows2d.hpp>
#if KIN_ENABLE_PHYSICS
#include <kin/ecs/physics.hpp>
#endif

#include <algorithm>
#include <functional>
#include <tuple>
#include <utility>

namespace kin {
namespace {

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

void EntityScripts::Impl::bind_entity(sol::state& lua) {
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
    // Function pointers and std::function give equal callback signatures
    // equal types, so sol2 can reuse its invocation adapters. Captures stay
    // owned by the Lua closure, including their original host pointer.
    sol::usertype<ScriptEntity> entity =
        scripting_detail::new_api_type<ScriptEntity>(lua, "Entity", sol::no_constructor);
    entity["id"] = +[](const ScriptEntity& e) { return static_cast<f64>(e.entity.id()); };
    entity["alive"] = &ScriptEntity::alive;
    entity["name"] = +[](const ScriptEntity& e) { return std::string(e.live().name().c_str()); };
    entity["parent"] = +[](const ScriptEntity& e) -> std::optional<ScriptEntity> {
        const flecs::entity parent = e.live().parent();
        return parent ? std::optional(ScriptEntity{e.world, parent}) : std::nullopt;
    };
    entity["child"] = +[](const ScriptEntity& e, const std::string& name) -> std::optional<ScriptEntity> {
        const flecs::entity child = e.live().lookup(name.c_str());
        return child ? std::optional(ScriptEntity{e.world, child}) : std::nullopt;
    };
    entity["has"] = +[](const ScriptEntity& e, const std::string& component) {
        return e.world->components().has(EcsEntity{e.live()}, component);
    };
    entity["add"] = +[](const ScriptEntity& e, const std::string& component) {
        return e.world->components().add(EcsEntity{e.live()}, component);
    };
    entity["remove"] = +[](const ScriptEntity& e, const std::string& component) {
        return e.world->components().remove(EcsEntity{e.live()}, component);
    };
    entity["get"] = +[](const ScriptEntity& e, const std::string& component, sol::this_state state) -> sol::object {
        sol::state_view lua(state);
        sol::table fields = lua.create_table();
        const bool found = e.world->components().visit_fields(
            EcsEntity{e.live()}, component,
            [&](std::string_view name, ComponentFieldKind, const ComponentFieldValue& value) {
                fields[std::string(name)] = component_value_to_lua(lua, value);
            });
        return found ? sol::object(fields) : sol::lua_nil;
    };
    entity["set"] = +[](const ScriptEntity& e, const std::string& component, const sol::table& values) {
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
    entity["position"] = +[](const ScriptEntity& e) {
        const Transform2D* t = e.live().get<Transform2D>();
        return std::make_tuple(t ? t->pos.x : 0.0f, t ? t->pos.y : 0.0f);
    };
    entity["set_position"] = +[](const ScriptEntity& e, f32 x, f32 y) {
        EcsEntity target{e.live()};
        Transform2D t = target.get<Transform2D>() ? *target.get<Transform2D>() : Transform2D{};
        t.pos = {x, y};
        target.set(t);
        teleport(target);
    };
    entity["move"] = +[](const ScriptEntity& e, f32 dx, f32 dy) {
        EcsEntity target{e.live()};
        Transform2D t = target.get<Transform2D>() ? *target.get<Transform2D>() : Transform2D{};
        t.pos = {t.pos.x + dx, t.pos.y + dy};
        target.set(t);
        teleport(target);
    };
    // Its velocity: its character's (CharacterController2D), else its body's.
    entity["velocity"] = std::function{[this](const ScriptEntity& e) {
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
    }};
    entity["set_velocity"] = +[](const ScriptEntity& e, f32 x, f32 y) {
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
    entity["on_floor"] = std::function{[character_found](const ScriptEntity& e) { return character_found(e, &CharacterFound::floor); }};
    entity["on_wall"] = std::function{[character_found](const ScriptEntity& e) { return character_found(e, &CharacterFound::wall); }};
    entity["on_ceiling"] = std::function{[character_found](const ScriptEntity& e) { return character_found(e, &CharacterFound::ceiling); }};
    entity["rotation"] = +[](const ScriptEntity& e) {
        const Transform2D* t = e.live().get<Transform2D>();
        return t ? t->rotation : 0.0f;
    };
    entity["set_rotation"] = +[](const ScriptEntity& e, f32 degrees) {
        EcsEntity target{e.live()};
        Transform2D t = target.get<Transform2D>() ? *target.get<Transform2D>() : Transform2D{};
        t.rotation = degrees;
        target.set(t);
    };
    entity["destroy"] = +[](const ScriptEntity& e) {
        const EcsEntity target{e.live()};
        if (e.world && e.world->entities().registered(target)) {
            e.world->entities().destroy(target); // its authored id (and its children's) let go, and told
        } else {
            target.raw().destruct();
        }
    };
    // Its ParticleSystemComponent's effect burst (or another of its catalog).
    entity["emit"] = +[](const ScriptEntity& e, sol::optional<std::string> effect) {
        return emit_particles(EcsEntity{e.live()}, effect.value_or(std::string{}));
    };
    // Its tweens (kin/ecs/tween.hpp, stepped with the world's script
    // services). self:tween calls this, then runs its `done` as a task.
    entity["tween"] = +[](const ScriptEntity& e, const std::string& property, const sol::object& to, f32 seconds,
                         const sol::object& easing_or_options, const sol::object& options) {
        TweenDef def = scripting_detail::lua_tween_def(*e.world, property, to, seconds, easing_or_options, options);
        std::string error;
        const u32 id = kin::tween(*e.world, EcsEntity{e.live()}, std::move(def), &error);
        if (id == 0) {
            throw sol::error("tween: " + error);
        }
        return id;
    };
    entity["tweening"] = +[](const ScriptEntity& e, sol::optional<std::string> property) {
        return tweening(EcsEntity{e.live()}, property.value_or(""));
    };
    entity["stop_tween"] = +[](const ScriptEntity& e, u32 id) { return stop_tween(EcsEntity{e.live()}, id); };
    entity["stop_tweens"] = +[](const ScriptEntity& e, sol::optional<std::string> property) {
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
    entity["play"] = std::function{[animator](const ScriptEntity& e, const std::string& animation) { animator(e)->animation = animation; }};
    entity["set_param"] = std::function{[animator](const ScriptEntity& e, const std::string& name, const sol::object& value) {
        Animator* found = animator(e);
        if (value.get_type() == sol::type::boolean) {
            found->params.set_bool(name, value.as<bool>());
        } else if (value.get_type() == sol::type::number) {
            found->params.set_float(name, value.as<f32>());
        } else {
            throw sol::error("set_param takes true, false or a number");
        }
    }};
    entity["trigger"] = std::function{[animator](const ScriptEntity& e, const std::string& name) { animator(e)->params.set_trigger(name); }};
    entity["state"] = std::function{[animator](const ScriptEntity& e) { return animator(e)->state; }};
}

} // namespace kin
