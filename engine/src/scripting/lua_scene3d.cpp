#include "lua_paths.hpp"
#include "lua_scene3d_detail.hpp"
#include "lua_scene_context.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <sol/sol.hpp>
#include <stdexcept>

namespace kin::scripting_detail {
namespace lua_scene3d_detail {
struct LuaAction3D {
    SceneEntity3D actor;
    ActionToken3D token;
};
void fields(sol::table options, std::initializer_list<std::string_view> names) {
    for (const auto& [key, value] : options) {
        if (!key.is<std::string>())
            throw std::runtime_error("3D option keys must be strings");
        const auto name = key.as<std::string>();
        if (std::ranges::find(names, name) == names.end())
            throw std::runtime_error("unknown 3D option: " + name);
    }
}
Vec3f vector(sol::table value) {
    Vec3f out{value.get<f32>("x"), value.get<f32>("y"), value.get<f32>("z")};
    if (!std::isfinite(out.x) || !std::isfinite(out.y) || !std::isfinite(out.z))
        throw std::runtime_error("3D vectors require finite x, y, z");
    return out;
}
sol::table vector(sol::state_view lua, Vec3f value) {
    return lua.create_table_with("x", value.x, "y", value.y, "z", value.z);
}
Quatf rotation(sol::table value) {
    const auto xyz = vector(value);
    const auto w = value.get<f32>("w");
    const f64 length = f64(xyz.x) * xyz.x + f64(xyz.y) * xyz.y + f64(xyz.z) * xyz.z + f64(w) * w;
    if (!std::isfinite(w) || std::fabs(length - 1) > 1e-4)
        throw std::runtime_error("3D rotation requires a unit quaternion");
    return {xyz.x, xyz.y, xyz.z, w};
}
f32 number(sol::table options, const char* name, f32 fallback, f32 minimum, f32 maximum) {
    const auto value = options.get_or(name, fallback);
    if (!std::isfinite(value) || value < minimum || value > maximum)
        throw std::runtime_error(std::string("3D number out of range: ") + name);
    return value;
}
EcsCommandBuffer& command_buffer(const LuaScene3DBindings& api) {
    if (!api.active())
        throw std::runtime_error("3D gameplay mutations require an active scene");
    auto* buffer = current_script_commands();
    if (!buffer)
        buffer = api.callback_commands();
    if (!buffer)
        throw std::runtime_error("3D gameplay mutations require a script callback or system");
    return *buffer;
}
EcsEntity resolve(const LuaScene3DBindings& api, SceneEntity3D handle) {
    return api.active() ? handle.resolve(*api.world) : EcsEntity{};
}
Character3D native_character(const LuaScene3DBindings& api, EcsEntity entity) {
    if (!entity || !api.queries)
        return {};
    const auto character = api.bodies->character_of(entity.raw());
    return api.physics->contains(character) ? character : Character3D{};
}
void physics(const LuaScene3DBindings& api) {
    if (!api.queries)
        throw std::runtime_error("3D physics queries require an active physics scene");
}
SceneEntity3D body_entity(const LuaScene3DBindings& api, Body3D body) {
    const auto entity = api.bodies->entity_of_body(body);
    return entity ? api.queries.handle(EcsEntity{entity}) : SceneEntity3D{};
}
void entity_field(sol::table row, const char* key, SceneEntity3D entity) {
    if (entity)
        row[key] = entity;
}
const char* ground(CharacterGround3D value) {
    switch (value) {
    case CharacterGround3D::OnGround:
        return "on_ground";
    case CharacterGround3D::OnSteepSlope:
        return "on_steep_slope";
    case CharacterGround3D::Sliding:
        return "sliding";
    case CharacterGround3D::InAir:
        return "in_air";
    }
    return "in_air";
}
const char* contact_kind(ContactKind3D value) {
    switch (value) {
    case ContactKind3D::Begin:
        return "begin";
    case ContactKind3D::Persist:
        return "persist";
    case ContactKind3D::End:
        return "end";
    }
    return "end";
}
const char* character_kind(CharacterEventKind3D value) {
    switch (value) {
    case CharacterEventKind3D::Landed:
        return "landed";
    case CharacterEventKind3D::LeftGround:
        return "left_ground";
    case CharacterEventKind3D::HitHead:
        return "hit_head";
    case CharacterEventKind3D::ContactBegin:
        return "contact_begin";
    case CharacterEventKind3D::ContactEnd:
        return "contact_end";
    case CharacterEventKind3D::Gripped:
        return "gripped";
    case CharacterEventKind3D::Slipped:
        return "slipped";
    case CharacterEventKind3D::GripReleased:
        return "grip_released";
    case CharacterEventKind3D::Exhausted:
        return "exhausted";
    case CharacterEventKind3D::MantleStarted:
        return "mantle_started";
    case CharacterEventKind3D::MantleFinished:
        return "mantle_finished";
    case CharacterEventKind3D::MantleBlocked:
        return "mantle_blocked";
    case CharacterEventKind3D::Fell:
        return "fell";
    }
    return "contact_end";
}
const char* action_status(ActionStatus3D value) {
    switch (value) {
    case ActionStatus3D::None:
        return "none";
    case ActionStatus3D::Playing:
        return "playing";
    case ActionStatus3D::Finishing:
        return "finishing";
    case ActionStatus3D::Completed:
        return "completed";
    case ActionStatus3D::Cancelled:
        return "cancelled";
    case ActionStatus3D::Superseded:
        return "superseded";
    }
    return "none";
}
const AnimClip3D* clip(EcsEntity actor, std::string_view name) {
    if (const auto* skin = actor.get<SkinnedMeshRenderer3D>();
        skin && skin->model && !skin->model->skeletons.empty() && skin->model->node_skeleton != 0)
        return skin->model->find_clip(name);
    if (const auto* rigid = actor.get<ModelRenderer3D>(); rigid && rigid->model && rigid->model->node_skeleton >= 0)
        return rigid->model->find_clip(name, static_cast<u32>(rigid->model->node_skeleton));
    return nullptr;
}
const char* status(SceneRequestStatus3D value) {
    switch (value) {
    case SceneRequestStatus3D::Invalid:
        return "invalid";
    case SceneRequestStatus3D::Pending:
        return "pending";
    case SceneRequestStatus3D::Succeeded:
        return "succeeded";
    case SceneRequestStatus3D::Failed:
        return "failed";
    case SceneRequestStatus3D::Cancelled:
        return "cancelled";
    }
    return "invalid";
}
SceneSpawn3D spawn_options(const sol::optional<sol::table>& options) {
    SceneSpawn3D out;
    if (!options)
        return out;
    fields(*options, {"position", "scale", "rotation", "parent"});
    const auto read_vector = [&](const char* name, std::optional<Vec3f>& destination) {
        sol::object value = (*options)[name];
        if (value != sol::nil)
            destination = vector(value.as<sol::table>());
    };
    read_vector("position", out.position);
    read_vector("scale", out.scale);
    sol::object rotation = (*options)["rotation"];
    if (rotation != sol::nil) {
        out.rotation = lua_scene3d_detail::rotation(rotation.as<sol::table>());
    }
    sol::object parent = (*options)["parent"];
    if (parent != sol::nil)
        out.parent = parent.as<SceneEntity3D>();
    return out;
}
} // namespace lua_scene3d_detail

void bind_lua_scene3d(sol::state& lua, LuaScene3DBindings bindings) {
    using namespace lua_scene3d_detail;
    const auto api = std::make_shared<LuaScene3DBindings>(std::move(bindings));
    lua.new_usertype<LuaAction3D>("KinAction3D", sol::no_constructor);
    lua.new_usertype<SceneEntity3D>("KinSceneEntity3D", sol::no_constructor, "id",
                                    sol::property([](SceneEntity3D h) { return h.entity.id; }), "alive",
                                    [api](SceneEntity3D h) { return bool(resolve(*api, h)); });
    lua.new_usertype<ScenePrefab3D>("KinScenePrefab3D", sol::no_constructor, "root",
                                    sol::property([](const ScenePrefab3D& p) { return p.root; }));
    lua.new_usertype<ScenePreparedPrefab3D>("KinScenePreparedPrefab3D", sol::no_constructor);
    lua.new_usertype<ScenePrefabPool3D>("KinScenePrefabPool3D", sol::no_constructor);
    lua.new_usertype<SceneRequest3D>(
        "KinSceneRequest3D", sol::no_constructor, "status",
        sol::property([](const SceneRequest3D& r) { return status(r.status()); }), "error",
        sol::property(&SceneRequest3D::error), "prefab", sol::property(&SceneRequest3D::prefab), "prepared",
        sol::property([](const SceneRequest3D& r) -> sol::optional<ScenePreparedPrefab3D> {
            const auto h = r.prepared();
            if (h)
                return h;
            return sol::nullopt;
        }),
        "pool", sol::property([](const SceneRequest3D& r) -> sol::optional<ScenePrefabPool3D> {
            const auto h = r.pool();
            if (h)
                return h;
            return sol::nullopt;
        }));
    auto scene = lua.create_named_table("scene3d");
    scene["available"] = [api] { return api->active(); };
    scene["physics_available"] = [api] { return bool(api->queries); };
    scene["entity"] = [api](std::string_view id) { return api->active() ? api->entity(id) : SceneEntity3D{}; };
    scene["character_input"] = [api](SceneEntity3D handle, sol::table options) {
        command_buffer(*api);
        fields(options, {"move", "forward", "jump", "crouch", "sprint"});
        CharacterInput3D input;
        if (sol::object move = options["move"]; move != sol::nil) {
            const auto value = move.as<sol::table>();
            input.move = {value.get<f32>("x"), value.get<f32>("y")};
            if (!std::isfinite(input.move.x) || !std::isfinite(input.move.y))
                throw std::runtime_error("character movement requires finite x, y");
        }
        if (sol::object forward = options["forward"]; forward != sol::nil)
            input.forward = vector(forward.as<sol::table>());
        input.jump = options.get_or("jump", false);
        input.crouch = options.get_or("crouch", false);
        input.sprint = options.get_or("sprint", false);
        script_component_access("CharacterController3D", true);
        auto entity = resolve(*api, handle);
        if (!native_character(*api, entity))
            return false;
        auto* controller = entity.get_mut<CharacterController3D>();
        if (!controller)
            return false;
        controller->input = input;
        return true;
    };
    scene["character_state"] = [api](sol::this_state state, SceneEntity3D handle) -> sol::object {
        script_component_access("CharacterController3D", false);
        auto entity = resolve(*api, handle);
        if (!native_character(*api, entity))
            return sol::nil;
        const auto* controller = entity.get<CharacterController3D>();
        if (!controller)
            return sol::nil;
        const auto& value = controller->state;
        sol::state_view lua{state};
        auto row = lua.create_table_with(
            "position", vector(lua, value.position), "velocity", vector(lua, value.velocity), "up",
            vector(lua, value.up), "ground", ground(value.ground), "ground_normal", vector(lua, value.ground_normal),
            "ground_velocity", vector(lua, value.ground_velocity), "ground_speed", value.ground_speed, "vertical_speed",
            value.vertical_speed, "crouching", value.crouching, "stand_blocked", value.stand_blocked, "air_time",
            value.air_time, "height", value.height, "jumped", value.jumped);
        entity_field(row, "ground_entity", body_entity(*api, value.ground_body));
        return sol::make_object(state, row);
    };
    scene["contact_events"] = [api](sol::this_state state, sol::optional<SceneEntity3D> filter) {
        physics(*api);
        if (filter && !resolve(*api, *filter))
            throw std::runtime_error("3D contact filter entity is stale");
        sol::state_view lua{state};
        auto rows = lua.create_table();
        int index = 0;
        for (const auto& event : api->bodies->events()) {
            const auto a = body_entity(*api, event.a), b = body_entity(*api, event.b);
            if (filter && a != *filter && b != *filter)
                continue;
            auto row = lua.create_table_with("kind", contact_kind(event.kind), "sensor", event.sensor, "position",
                                             vector(lua, event.point), "normal", vector(lua, event.normal),
                                             "closing_speed", event.closing_speed, "point_count", event.point_count);
            entity_field(row, "a", a);
            entity_field(row, "b", b);
            rows[++index] = row;
        }
        return rows;
    };
    scene["character_events"] = [api](sol::this_state state, sol::optional<SceneEntity3D> filter) {
        physics(*api);
        if (filter && !resolve(*api, *filter))
            throw std::runtime_error("3D character-event filter entity is stale");
        sol::state_view lua{state};
        auto rows = lua.create_table();
        int index = 0;
        for (const auto& event : api->bodies->character_events()) {
            // Native character identity, not a recycled user id, selects the actor.
            const auto entity = api->bodies->entity_of(event.user_id);
            const auto actor = entity && api->bodies->character_of(entity) == event.character
                                   ? api->queries.handle(EcsEntity{entity})
                                   : SceneEntity3D{};
            if (filter && actor != *filter)
                continue;
            auto row =
                lua.create_table_with("kind", character_kind(event.kind), "sensor", event.sensor, "position",
                                      vector(lua, event.point), "normal", vector(lua, event.normal), "speed",
                                      event.speed, "jumped", event.jumped, "hand", event.hand, "reason", event.reason);
            entity_field(row, "character", actor);
            entity_field(row, "other", body_entity(*api, event.body));
            rows[++index] = row;
        }
        return rows;
    };
    scene["set_animation"] = [api](SceneEntity3D handle, sol::table options) {
        command_buffer(*api);
        fields(options, {"clip", "speed", "loop", "playing", "fade_seconds"});
        script_component_access("Animator3D", true);
        auto actor = resolve(*api, handle);
        const auto* animator = actor ? actor.get<Animator3D>() : nullptr;
        if (!animator)
            return false;
        const auto name = options.get_or("clip", animator->clip);
        const f32 speed = options.get_or("speed", animator->speed);
        const f32 fade = options.get_or("fade_seconds", animator->fade_seconds);
        const bool loop = options.get_or("loop", animator->loop),
                   playing = options.get_or("playing", animator->playing);
        if (!std::isfinite(speed) || !std::isfinite(fade) || fade < 0)
            throw std::runtime_error("animation speed must be finite and fade_seconds finite and nonnegative");
        if (sol::object selection = options["clip"]; selection != sol::nil) {
            if (animator->machine)
                throw std::runtime_error("select authored animation states through parameters, not a base clip");
            if (!name.empty() && !clip(actor, name))
                return false;
        }
        auto* own = actor.get_mut<Animator3D>();
        own->clip = name;
        own->speed = speed;
        own->fade_seconds = fade;
        own->loop = loop;
        own->playing = playing;
        return true;
    };
    scene["set_animation_parameter"] = [api](SceneEntity3D handle, std::string name, sol::object value) {
        command_buffer(*api);
        if (name.empty() || (!value.is<bool>() && !value.is<f32>()))
            throw std::runtime_error("animation parameters require a name and a boolean or finite number");
        if (value.is<f32>() && !std::isfinite(value.as<f32>()))
            throw std::runtime_error("animation parameters require finite numbers");
        script_component_access("Animator3D", true);
        auto actor = resolve(*api, handle);
        auto* animator = actor ? actor.get_mut<Animator3D>() : nullptr;
        if (!animator)
            return false;
        if (value.is<bool>())
            animator->params.set_bool(name, value.as<bool>());
        else
            animator->params.set_float(name, value.as<f32>());
        return true;
    };
    scene["trigger_animation"] = [api](SceneEntity3D handle, std::string name) {
        command_buffer(*api);
        if (name.empty())
            throw std::runtime_error("animation triggers require a name");
        script_component_access("Animator3D", true);
        auto actor = resolve(*api, handle);
        auto* animator = actor ? actor.get_mut<Animator3D>() : nullptr;
        if (!animator)
            return false;
        animator->params.set_trigger(name);
        return true;
    };
    scene["animation_state"] = [api](sol::this_state state, SceneEntity3D handle) -> sol::object {
        script_component_access("Animator3D", false);
        const auto actor = resolve(*api, handle);
        const auto* animator = actor ? actor.get<Animator3D>() : nullptr;
        if (!animator)
            return sol::nil;
        sol::state_view lua{state};
        return sol::make_object(state, lua.create_table_with("state", animator->state, "clip", animator->clip, "time",
                                                             animator->player.time(), "playing", animator->playing,
                                                             "speed", animator->speed, "loop", animator->loop,
                                                             "finished", animator->player.finished()));
    };
    scene["play_action"] = [api](sol::this_state state, SceneEntity3D handle, std::string name,
                                 sol::optional<std::string> mode) -> sol::object {
        command_buffer(*api);
        if (mode && *mode != "if_idle" && *mode != "replace")
            throw std::runtime_error("action mode must be if_idle or replace");
        script_component_access("Animator3D", true);
        const auto token = play_action3d(resolve(*api, handle), name,
                                         mode && *mode == "replace" ? ActionStart3D::Replace : ActionStart3D::IfIdle);
        return token ? sol::make_object(state, LuaAction3D{handle, token}) : sol::object{sol::nil};
    };
    scene["cancel_action"] = [api](LuaAction3D action, sol::optional<f32> fade) {
        command_buffer(*api);
        const f32 seconds = fade.value_or(-1);
        if (!std::isfinite(seconds) || (seconds < 0 && seconds != -1))
            throw std::runtime_error("action fade must be -1 or finite and nonnegative");
        script_component_access("Animator3D", true);
        return cancel_action3d(resolve(*api, action.actor), action.token, seconds);
    };
    scene["action_status"] = [api](LuaAction3D action) {
        script_component_access("Animator3D", false);
        return action_status(action_status3d(resolve(*api, action.actor), action.token));
    };
    scene["action_event_valid"] = [api](LuaAction3D action) {
        script_component_access("Animator3D", false);
        const auto actor = resolve(*api, action.actor);
        const auto* animator = actor ? actor.get<Animator3D>() : nullptr;
        return animator && animator->player.action_event_valid(action.token);
    };
    scene["animation_events"] = [api](sol::this_state state, SceneEntity3D handle) {
        script_component_access("Animator3D", false);
        sol::state_view lua{state};
        auto rows = lua.create_table();
        const auto actor = resolve(*api, handle);
        const auto* animator = actor ? actor.get<Animator3D>() : nullptr;
        u32 dropped = 0;
        if (animator) {
            const auto& batch = animator->events;
            dropped = batch.dropped;
            int index = 0;
            for (u32 i = 0; i < batch.count; ++i) {
                const auto action = batch.actions[i];
                if (action && !animator->player.action_event_valid(action))
                    continue;
                const auto& event = *batch.entries[i];
                auto row = lua.create_table_with("channel", event.channel, "name", event.name, "value", event.value);
                if (action)
                    row["action"] = LuaAction3D{handle, action};
                rows[++index] = row;
            }
        }
        return std::make_tuple(rows, dropped);
    };
    scene["set_ik"] = [api](SceneEntity3D handle, std::string name, sol::table options) {
        command_buffer(*api);
        fields(options, {"target", "offset", "weight", "fade_seconds", "pole_position", "pole_direction",
                         "match_rotation", "target_rotation", "rotation_weight"});
        const auto weight = number(options, "weight", 1, 0, 1),
                   fade = number(options, "fade_seconds", 0, 0, std::numeric_limits<f32>::max());
        const auto rotation_weight = number(options, "rotation_weight", 1, 0, 1);
        sol::object target = options["target"], offset = options["offset"], pole = options["pole_position"],
                    direction = options["pole_direction"], turn = options["target_rotation"];
        IKTarget3D destination;
        if (target.is<SceneEntity3D>()) {
            destination.kind = IKTarget3D::Kind::SceneEntity;
            destination.scene_entity = target.as<SceneEntity3D>();
            if (!resolve(*api, destination.scene_entity))
                throw std::runtime_error("IK target entity is stale");
            if (offset != sol::nil)
                destination.position = vector(offset.as<sol::table>());
        } else if (target.is<sol::table>()) {
            destination.kind = IKTarget3D::Kind::World;
            destination.position = vector(target.as<sol::table>());
        } else if (target != sol::nil && !(target.is<bool>() && !target.as<bool>())) {
            throw std::runtime_error("IK target requires a world point, scene entity, or false to clear");
        }
        if (offset != sol::nil && !target.is<SceneEntity3D>())
            throw std::runtime_error("IK offset requires an entity target");
        if (pole != sol::nil && direction != sol::nil)
            throw std::runtime_error("give one IK pole position or direction");
        std::optional<Vec3f> pole_value;
        if (pole != sol::nil)
            pole_value = vector(pole.as<sol::table>());
        if (direction != sol::nil)
            pole_value = vector(direction.as<sol::table>());
        std::optional<Quatf> orientation;
        if (turn != sol::nil)
            orientation = rotation(turn.as<sol::table>());
        const auto match = options.get_or("match_rotation", false);
        script_component_access("Animator3D", true);
        auto entity = resolve(*api, handle);
        auto* animator = entity ? entity.get_mut<Animator3D>() : nullptr;
        auto* constraint = animator ? animator->find_ik(name) : nullptr;
        if (!constraint)
            return false;
        if (target != sol::nil)
            constraint->target = destination;
        if (options["weight"] != sol::nil)
            constraint->weight = weight;
        if (options["fade_seconds"] != sol::nil)
            constraint->fade_seconds = fade;
        if (pole_value) {
            constraint->pole =
                pole != sol::nil ? IKConstraint3D::Pole::WorldPosition : IKConstraint3D::Pole::ModelDirection;
            constraint->pole_vector = *pole_value;
        }
        if (options["match_rotation"] != sol::nil)
            constraint->match_rotation = match;
        if (orientation)
            constraint->target_rotation = *orientation;
        if (options["rotation_weight"] != sol::nil)
            constraint->rotation_weight = rotation_weight;
        return true;
    };
    scene["ik_state"] = [api](sol::this_state state, SceneEntity3D handle, std::string name) -> sol::object {
        script_component_access("Animator3D", false);
        auto entity = resolve(*api, handle);
        const auto* animator = entity ? entity.get<Animator3D>() : nullptr;
        const auto* constraint = animator ? animator->find_ik(name) : nullptr;
        if (!constraint)
            return sol::nil;
        sol::state_view lua{state};
        auto row =
            lua.create_table_with("weight", constraint->weight, "current_weight", constraint->current, "target_valid",
                                  constraint->target_valid, "has_last_target", constraint->has_last_target);
        if (constraint->has_last_target)
            row["last_target"] = vector(lua, constraint->last_target);
        return sol::make_object(state, row);
    };
    scene["raycast"] = [api](sol::this_state state, sol::table from, sol::table to,
                             sol::optional<sol::table> options) -> sol::object {
        if (!api->queries)
            throw std::runtime_error("3D physics queries require an active physics scene");
        SceneQueryFilter3D filter;
        if (options) {
            filter.mask = options->get_or("mask", all_layers3d);
            filter.sensors = options->get_or("sensors", false);
            sol::object ignore = (*options)["ignore"];
            if (ignore != sol::nil)
                filter.ignore = ignore.as<SceneEntity3D>();
            if (filter.ignore.session && !resolve(*api, filter.ignore))
                throw std::runtime_error("3D query ignore entity is stale");
        }
        const auto hit = api->queries.raycast(vector(from), vector(to), filter);
        if (!hit)
            return sol::nil;
        sol::state_view lua{state};
        auto result =
            lua.create_table_with("fraction", hit->physics.fraction, "sub_shape", hit->physics.sub_shape, "position",
                                  vector(lua, hit->physics.point), "normal", vector(lua, hit->physics.normal));
        if (hit->entity)
            result["entity"] = SceneEntity3D(hit->entity);
        return sol::make_object(state, result);
    };
    scene["impulse"] = [api](SceneEntity3D handle, sol::table value) {
        command_buffer(*api); // mutations belong to an update/enter callback or an ECS system
        const auto impulse = vector(value);
        auto entity = resolve(*api, handle);
        if (!entity || !api->queries)
            return false;
        const auto body = api->bodies->body_of(entity.raw());
        if (!api->physics->contains(body) || api->physics->owner_of(body) != BodyOwner3D::None ||
            api->physics->motion(body) != Body3DMotion::Dynamic)
            return false;
        api->physics->add_impulse(body, impulse);
        return true;
    };
    const auto source_path = [api](const std::string& path) {
        const std::filesystem::path source{path};
        if (source.empty() || source.native().size() > 4096 || source.is_absolute() ||
            !path_within_root(api->asset_root / source, api->asset_root))
            throw std::runtime_error("3D prefab paths must stay relative to the content root (at most 4096 bytes)");
        return source;
    };
    scene["spawn"] = [api, source_path](sol::object target, sol::optional<sol::table> options) {
        auto& buffer = command_buffer(*api);
        auto placement = spawn_options(options);
        if (target.is<std::string>())
            return api->commands.spawn(buffer, source_path(target.as<std::string>()), std::move(placement));
        if (target.is<ScenePreparedPrefab3D>())
            return api->commands.spawn(buffer, target.as<ScenePreparedPrefab3D>(), std::move(placement));
        if (target.is<ScenePrefabPool3D>())
            return api->commands.spawn(buffer, target.as<ScenePrefabPool3D>(), std::move(placement));
        throw std::runtime_error("spawn requires a prefab path, prepared prefab or pool");
    };
    scene["preload"] = [api, source_path](std::string path) {
        return api->commands.preload(command_buffer(*api), source_path(path));
    };
    scene["create_pool"] = [api](ScenePreparedPrefab3D prepared, sol::optional<sol::table> options) {
        ScenePrefabPoolOptions3D config;
        if (options) {
            fields(*options, {"capacity"});
            const sol::object value = (*options)["capacity"];
            if (value != sol::nil) {
                const auto count = number(*options, "capacity", 16, 1, 10000);
                if (std::trunc(count) != count)
                    throw std::runtime_error("pool capacity must be an integer in 1..10000");
                config.capacity = static_cast<size_t>(count);
            }
        }
        return api->commands.create_pool(command_buffer(*api), prepared, std::move(config));
    };
    scene["pool_stats"] = [api](sol::this_state state, ScenePrefabPool3D pool) -> sol::object {
        if (!api->active())
            return sol::nil;
        const auto value = api->pool_stats(pool);
        if (!value)
            return sol::nil;
        sol::state_view lua{state};
        return sol::make_object(state, lua.create_table_with("capacity", value->capacity, "active", value->active,
                                                             "available", value->available, "acquired", value->acquired,
                                                             "reused", value->reused, "exhausted", value->exhausted));
    };
    scene["release"] = [api](sol::object resource) {
        auto& buffer = command_buffer(*api);
        if (resource.is<ScenePreparedPrefab3D>())
            return api->commands.release(buffer, resource.as<ScenePreparedPrefab3D>());
        if (resource.is<ScenePrefabPool3D>())
            return api->commands.release(buffer, resource.as<ScenePrefabPool3D>());
        throw std::runtime_error("release requires a prepared prefab or pool");
    };
    scene["despawn"] = [api](sol::object target) {
        auto& buffer = command_buffer(*api);
        if (target.is<SceneRequest3D>())
            return api->commands.despawn(buffer, target.as<SceneRequest3D>());
        if (target.is<ScenePrefab3D>())
            return api->commands.despawn(buffer, target.as<ScenePrefab3D>());
        throw std::runtime_error("despawn requires a 3D prefab or spawn request");
    };
    bind_physics(lua, scene, api);
}
} // namespace kin::scripting_detail
