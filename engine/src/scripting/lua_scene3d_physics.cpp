#include "lua_scene3d_detail.hpp"
#include "lua_scene_context.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace kin::scripting_detail::lua_scene3d_detail {
namespace {
constexpr f32 infinity = std::numeric_limits<f32>::max();
SceneQueryFilter3D filter(const LuaScene3DBindings& api, const sol::optional<sol::table>& options) {
    SceneQueryFilter3D out;
    if (options) {
        out.mask = option(*options, "mask", all_layers3d);
        out.sensors = option(*options, "sensors", false);
        out.initial_overlap = option(*options, "initial_overlap", false);
        if (sol::object ignore = (*options)["ignore"]; ignore != sol::nil) {
            out.ignore = checked<SceneEntity3D>(ignore, "ignore");
            (void)resolve(api, out.ignore);
        }
    }
    return out;
}
size_t capacity(const sol::optional<sol::table>& options) {
    const auto value = options ? number(*options, "capacity", 32, 1, 256) : 32;
    if (std::trunc(value) != value)
        throw std::runtime_error("query capacity must be an integer in 1..256");
    return static_cast<size_t>(value);
}
Quatf orientation(const sol::optional<sol::table>& options) {
    if (options)
        if (sol::object value = (*options)["rotation"]; value != sol::nil)
            return rotation(checked<sol::table>(value, "rotation"));
    return {};
}
template <class Hit> sol::table hit_table(sol::state_view lua, const SceneQueryHit3D<Hit>& hit) {
    auto out = lua.create_table_with("position", vector(lua, hit.physics.point), "normal",
                                     vector(lua, hit.physics.normal), "sub_shape", hit.physics.sub_shape);
    entity_field(out, "entity", hit.entity);
    if constexpr (requires { hit.physics.fraction; })
        out["fraction"] = hit.physics.fraction;
    if constexpr (requires { hit.physics.penetration; })
        out["penetration"] = hit.physics.penetration;
    if constexpr (requires { hit.physics.position; })
        out["shape_position"] = vector(lua, hit.physics.position);
    return out;
}
RagdollHandle3D ragdoll(const LuaScene3DBindings& api, SceneEntity3D handle) {
    const auto entity = resolve(api, handle);
    if (!entity || !api.queries)
        return {};
    const auto native = api.bodies->ragdoll_of(entity.raw());
    return api.physics->contains(native) ? native : RagdollHandle3D{};
}
RopeHandle3D rope(const LuaScene3DBindings& api, SceneEntity3D handle) {
    const auto entity = resolve(api, handle);
    if (!entity || !api.queries)
        return {};
    const auto native = api.bodies->rope_of(entity.raw());
    return api.physics->contains(native) ? native : RopeHandle3D{};
}
const char* mode(RagdollMode3D value) {
    switch (value) {
    case RagdollMode3D::Animated:
        return "animated";
    case RagdollMode3D::Ragdoll:
        return "ragdoll";
    case RagdollMode3D::Blend:
        return "blend";
    case RagdollMode3D::GetUp:
        return "get_up";
    }
    return "animated";
}
const char* grip_phase(GripPhase3D value) {
    switch (value) {
    case GripPhase3D::Free:
        return "free";
    case GripPhase3D::Reaching:
        return "reaching";
    case GripPhase3D::Held:
        return "held";
    case GripPhase3D::Slipping:
        return "slipping";
    case GripPhase3D::Released:
        return "released";
    }
    return "free";
}
const char* grip_kind(GripKind3D value) {
    switch (value) {
    case GripKind3D::Surface:
        return "surface";
    case GripKind3D::Ledge:
        return "ledge";
    case GripKind3D::Hold:
        return "hold";
    case GripKind3D::Rope:
        return "rope";
    }
    return "surface";
}
sol::table place(sol::state_view lua, const RagdollPlace3D& value) {
    auto q = vector(lua, {value.rotation.x, value.rotation.y, value.rotation.z});
    q["w"] = value.rotation.w;
    return lua.create_table_with("found", value.found, "position", vector(lua, value.position), "rotation", q,
                                 "face_down", value.face_down, "lying", value.lying, "height", value.height);
}
RagdollStart3D start_options(const sol::optional<sol::table>& options) {
    RagdollStart3D out;
    if (!options)
        return out;
    out.blend_seconds = number(*options, "blend_seconds", 0, 0, infinity);
    if (sol::object value = (*options)["velocity"]; value != sol::nil) {
        out.set_velocity = true;
        out.velocity = vector(checked<sol::table>(value, "velocity"));
    }
    const auto bone = number(*options, "impulse_bone", -1, -1, 65535);
    if (std::trunc(bone) != bone)
        throw std::runtime_error("impulse_bone must be a zero-based integer");
    out.impulse_bone = static_cast<i32>(bone);
    if (sol::object value = (*options)["impulse"]; value != sol::nil)
        out.impulse = vector(checked<sol::table>(value, "impulse"));
    if (sol::object value = (*options)["impulse_point"]; value != sol::nil) {
        out.point_given = true;
        out.impulse_point = vector(checked<sol::table>(value, "impulse_point"));
    }
    return out;
}
void validate_impulse(const LuaScene3DBindings& api, RagdollHandle3D native, const RagdollStart3D& start) {
    if (start.impulse_bone >= 0 &&
        static_cast<size_t>(start.impulse_bone) >= api.physics->ragdoll_bodies(native).size())
        throw std::runtime_error("ragdoll impulse bone is outside this skeleton");
    if (start.impulse_bone < 0 && (start.point_given || length_squared(start.impulse) > 0))
        throw std::runtime_error("ragdoll impulse requires an impulse_bone");
}
} // namespace

void bind_physics(sol::state& lua, sol::table table, std::shared_ptr<LuaScene3DBindings> api) {
    CheckedBindings scene{table};
    lua.new_usertype<Shape3D>("KinQueryShape3D", sol::no_constructor);
    scene["shape"] = [](sol::object options_argument) {
        const auto options = checked<sol::table>(options_argument, "options");

        const auto kind = checked<std::string>(options["kind"], "kind");
        Shape3D out;
        if (kind == "box") {
            fields(options, {"kind", "half_extents"});
            out = Shape3D::box(vector(checked<sol::table>(options["half_extents"], "half_extents")));
        } else if (kind == "sphere") {
            fields(options, {"kind", "radius"});
            out = Shape3D::sphere(number(options, "radius", 0, 0, infinity));
        } else if (kind == "capsule" || kind == "cylinder") {
            fields(options, {"kind", "radius", "half_height"});
            const auto radius = number(options, "radius", 0, 0, infinity),
                       height = number(options, "half_height", 0, 0, infinity);
            out = kind == "capsule" ? Shape3D::capsule(height, radius) : Shape3D::cylinder(height, radius);
        } else if (kind == "convex_hull") {
            fields(options, {"kind", "points"});
            const auto points = checked<sol::table>(options["points"], "points");
            if (points.size() < 4 || points.size() > 256)
                throw std::runtime_error("convex query shapes require 4..256 points");
            std::vector<Vec3f> vertices;
            vertices.reserve(points.size());
            for (size_t i = 1; i <= points.size(); ++i)
                vertices.push_back(vector(checked<sol::table>(points[i], "point")));
            out = Shape3D::convex_hull(vertices);
        } else
            throw std::runtime_error("unsupported 3D query shape kind");
        if (!out)
            throw std::runtime_error("invalid 3D query shape dimensions or geometry");
        return out;
    };
    scene["raycast_all"] = [api](sol::this_state state, sol::object from_argument, sol::object to_argument,
                                 sol::object options_argument) {
        const auto from = checked<sol::table>(from_argument, "from");
        const auto to = checked<sol::table>(to_argument, "to");

        const auto options = optional_argument<sol::table>(options_argument, "options");
        physics(*api);
        if (options)
            fields(*options, {"mask", "sensors", "ignore", "capacity"});
        std::vector<SceneRayHit3D> hits(capacity(options));
        const auto result = api->queries.raycast_all(vector(from), vector(to), hits, filter(*api, options));
        if (!result.valid)
            throw std::runtime_error("invalid 3D ray query");
        sol::state_view lua{state};
        auto rows = lua.create_table();
        for (size_t i = 0; i < result.count; ++i)
            rows[i + 1] = hit_table(lua, hits[i]);
        return std::make_tuple(rows, result.overflow);
    };
    scene["shape_cast"] = [api](sol::this_state state, const Shape3D& shape, sol::object position_argument,
                                sol::object displacement_argument, sol::object options_argument) -> sol::object {
        const auto position = checked<sol::table>(position_argument, "position");
        const auto displacement = checked<sol::table>(displacement_argument, "displacement");

        const auto options = optional_argument<sol::table>(options_argument, "options");
        physics(*api);
        if (options)
            fields(*options, {"mask", "sensors", "ignore", "initial_overlap", "rotation"});
        const auto hit = api->queries.shape_cast(shape, vector(position), orientation(options), vector(displacement),
                                                 filter(*api, options));
        if (!hit)
            return sol::nil;
        return sol::make_object(state, hit_table(sol::state_view{state}, *hit));
    };
    scene["overlap"] = [api](sol::this_state state, const Shape3D& shape, sol::object position_argument,
                             sol::object options_argument) {
        const auto position = checked<sol::table>(position_argument, "position");

        const auto options = optional_argument<sol::table>(options_argument, "options");
        physics(*api);
        if (options)
            fields(*options, {"mask", "sensors", "ignore", "rotation", "capacity"});
        std::vector<SceneOverlapHit3D> hits(capacity(options));
        const auto result =
            api->queries.overlap(shape, vector(position), orientation(options), hits, filter(*api, options));
        if (!result.valid)
            throw std::runtime_error("invalid 3D overlap query");
        sol::state_view lua{state};
        auto rows = lua.create_table();
        for (size_t i = 0; i < result.count; ++i)
            rows[i + 1] = hit_table(lua, hits[i]);
        return std::make_tuple(rows, result.overflow);
    };
    scene["traversal_input"] = [api](SceneEntity3D handle, sol::object options_argument) {
        const auto options = checked<sol::table>(options_argument, "options");

        command_buffer(*api);
        fields(options, {"grab", "reach", "pull_up", "jump_off", "drop"});
        TraversalInput3D input;
        if (sol::object value = options["grab"]; value != sol::nil) {
            const auto limbs = checked<sol::table>(value, "grab");
            for (const auto& [key, held] : limbs) {
                if (!key.is<int>() || checked<int>(key, "grab index") < 1 ||
                    checked<int>(key, "grab index") > static_cast<int>(max_grip_limbs3d) || !held.is<bool>())
                    throw std::runtime_error("grab requires boolean limb entries indexed 1..4");
                input.grab[static_cast<size_t>(checked<int>(key, "grab index") - 1)] = checked<bool>(held, "held");
            }
        }
        if (sol::object value = options["reach"]; value != sol::nil)
            input.reach = vector(checked<sol::table>(value, "reach"));
        input.pull_up = option(options, "pull_up", false);
        input.jump_off = option(options, "jump_off", false);
        input.drop = option(options, "drop", false);
        script_component_access("CharacterController3D", true);
        auto entity = resolve(*api, handle);
        if (!native_character(*api, entity))
            return false;
        auto* controller = entity.get_mut<CharacterController3D>();
        if (!controller || !controller->desc.traversal.enabled)
            return false;
        controller->traversal = input;
        return true;
    };
    scene["traversal_state"] = [api](sol::this_state state, SceneEntity3D handle) -> sol::object {
        script_component_access("CharacterController3D", false);
        const auto character = native_character(*api, resolve(*api, handle));
        if (!character)
            return sol::nil;
        const auto value = api->physics->traversal(character);
        sol::state_view lua{state};
        auto grips = lua.create_table();
        for (u32 i = 0; i < value.limb_count; ++i) {
            const auto& g = value.grips[i];
            auto row = lua.create_table_with("phase", grip_phase(g.phase), "kind", grip_kind(g.kind), "position",
                                             vector(lua, g.point), "normal", vector(lua, g.normal), "right",
                                             vector(lua, g.right), "surface_up", vector(lua, g.surface_up), "strength",
                                             g.strength, "reach_progress", g.reach_progress, "slip_speed", g.slip_speed,
                                             "slip_time", g.slip_time, "surface_tag", g.surface_tag, "drain", g.drain,
                                             "fixed", g.fixed, "ledge", g.ledge, "quality", g.quality);
            entity_field(row, "entity", body_entity(*api, g.body));
            grips[i + 1] = row;
        }
        return sol::make_object(
            state, lua.create_table_with("enabled", value.enabled, "mode",
                                         value.mode == TraversalMode3D::Walking   ? "walking"
                                         : value.mode == TraversalMode3D::Hanging ? "hanging"
                                                                                  : "mantling",
                                         "limb_count", value.limb_count, "held", value.held, "grips", grips,
                                         "mantle_progress", value.mantle_progress, "swing", vector(lua, value.swing),
                                         "hang_place", vector(lua, value.hang_place), "hang_time", value.hang_time,
                                         "anchor_centre", vector(lua, value.anchor_centre), "surface_normal",
                                         vector(lua, value.surface_normal), "resource",
                                         lua.create_table_with("value", value.resource.value, "max", value.resource.max,
                                                               "delay", value.resource.delay, "fraction",
                                                               value.resource.fraction(), "exhausted",
                                                               value.resource.exhausted)));
    };
    scene["set_traversal_resource"] = [api](SceneEntity3D handle, f32 value) {
        command_buffer(*api);
        script_component_access("CharacterController3D", true);
        if (!std::isfinite(value) || value < 0)
            throw std::runtime_error("traversal resource must be finite and nonnegative");
        const auto character = native_character(*api, resolve(*api, handle));
        if (!character || !api->physics->traversal(character).enabled)
            return false;
        api->physics->set_traversal_resource(character, value);
        return true;
    };
    scene["ragdoll_state"] = [api](sol::this_state state, SceneEntity3D handle) -> sol::object {
        script_component_access("Ragdoll3D", false);
        const auto native = ragdoll(*api, handle);
        if (!native)
            return sol::nil;
        const auto value = api->physics->ragdoll_state(native);
        sol::state_view lua{state};
        return sol::make_object(state,
                                lua.create_table_with("mode", mode(value.mode), "alpha", value.alpha, "target_alpha",
                                                      value.target_alpha, "dynamic_bodies", value.dynamic_bodies,
                                                      "awake_bodies", value.awake_bodies, "max_speed", value.max_speed,
                                                      "settled", value.settled, "frozen", value.frozen));
    };
    scene["start_ragdoll"] = [api](SceneEntity3D handle, sol::object options_argument) {
        const auto options = optional_argument<sol::table>(options_argument, "options");
        command_buffer(*api);
        script_component_access("Ragdoll3D", true);
        if (options)
            fields(*options, {"blend_seconds", "velocity", "impulse_bone", "impulse", "impulse_point"});
        const auto start = start_options(options);
        const auto native = ragdoll(*api, handle);
        if (!native)
            return false;
        validate_impulse(*api, native, start);
        return api->bodies->start_ragdoll(resolve(*api, handle).raw(), start);
    };
    scene["hand_over_to_ragdoll"] = [api](SceneEntity3D handle, sol::object options_argument) {
        const auto options = optional_argument<sol::table>(options_argument, "options");
        command_buffer(*api);
        script_component_access("Ragdoll3D", true);
        script_component_access("CharacterController3D", true);
        if (options)
            fields(*options, {"blend_seconds", "inherit_velocity", "impulse_bone", "impulse", "impulse_point"});
        const auto start = start_options(options);
        RagdollHandOver3D out;
        out.blend_seconds = options ? number(*options, "blend_seconds", .1f, 0, infinity) : .1f;
        out.inherit_velocity = options ? option(*options, "inherit_velocity", true) : true;
        out.impulse_bone = start.impulse_bone;
        out.impulse = start.impulse;
        out.impulse_point = start.impulse_point;
        out.point_given = start.point_given;
        const auto native = ragdoll(*api, handle);
        if (!native || !native_character(*api, resolve(*api, handle)))
            return false;
        validate_impulse(*api, native, start);
        return api->bodies->hand_over_to_ragdoll(resolve(*api, handle).raw(), out);
    };
    scene["blend_ragdoll"] = [api](SceneEntity3D handle, f32 alpha, f32 seconds) {
        command_buffer(*api);
        script_component_access("Ragdoll3D", true);
        if (!std::isfinite(alpha) || alpha < 0 || alpha > 1 || !std::isfinite(seconds) || seconds < 0)
            throw std::runtime_error("ragdoll blend requires alpha 0..1 and nonnegative seconds");
        return ragdoll(*api, handle) && api->bodies->blend_ragdoll(resolve(*api, handle).raw(), alpha, seconds);
    };
    scene["ragdoll_get_up"] = [api](SceneEntity3D handle, f32 seconds) {
        command_buffer(*api);
        script_component_access("Ragdoll3D", true);
        if (!std::isfinite(seconds) || seconds < 0)
            throw std::runtime_error("get-up seconds must be finite and nonnegative");
        return ragdoll(*api, handle) && api->bodies->ragdoll_get_up(resolve(*api, handle).raw(), seconds);
    };
    scene["animate_ragdoll"] = [api](SceneEntity3D handle) {
        command_buffer(*api);
        script_component_access("Ragdoll3D", true);
        return ragdoll(*api, handle) && api->bodies->animate_ragdoll(resolve(*api, handle).raw());
    };
    scene["set_ragdoll_subtree"] = [api](SceneEntity3D handle, std::string joint, f32 weight) {
        command_buffer(*api);
        script_component_access("Ragdoll3D", true);
        if (!std::isfinite(weight) || weight < 0 || weight > 1)
            throw std::runtime_error("ragdoll weight must be in 0..1");
        return ragdoll(*api, handle) && api->bodies->set_ragdoll_subtree(resolve(*api, handle).raw(), joint, weight);
    };
    scene["recover_from_ragdoll"] = [api](sol::this_state state, SceneEntity3D handle,
                                          sol::object options_argument) -> sol::object {
        const auto options = optional_argument<sol::table>(options_argument, "options");
        command_buffer(*api);
        script_component_access("Ragdoll3D", true);
        script_component_access("CharacterController3D", true);
        RagdollRecover3D config;
        if (options) {
            fields(*options, {"get_up_seconds", "max_ground_distance", "mask", "allowed_penetration"});
            config.get_up_seconds = number(*options, "get_up_seconds", config.get_up_seconds, 0, infinity);
            config.max_ground_distance =
                number(*options, "max_ground_distance", config.max_ground_distance, 0, infinity);
            config.allowed_penetration =
                number(*options, "allowed_penetration", config.allowed_penetration, 0, infinity);
            config.mask = option(*options, "mask", config.mask);
        }
        if (!ragdoll(*api, handle))
            return sol::nil;
        const auto value = api->bodies->recover_from_ragdoll(resolve(*api, handle).raw(), config);
        sol::state_view lua{state};
        return sol::make_object(state, lua.create_table_with("recovered", value.recovered, "blocked", value.blocked,
                                                             "place", place(lua, value.place)));
    };
    scene["ragdoll_events"] = [api](sol::this_state state, sol::object filter_argument) {
        const auto filter = optional_argument<SceneEntity3D>(filter_argument, "filter");
        physics(*api);
        if (filter)
            (void)resolve(*api, *filter);
        sol::state_view lua{state};
        auto rows = lua.create_table();
        int index = 0;
        for (const auto& e : api->bodies->ragdoll_events()) {
            const auto entity = api->bodies->entity_of(e.user_id);
            const auto actor = entity && api->bodies->ragdoll_of(entity) == e.ragdoll
                                   ? api->queries.handle(EcsEntity{entity})
                                   : SceneEntity3D{};
            if (filter && actor != *filter)
                continue;
            const char* kind = e.kind == RagdollEventKind3D::Started     ? "started"
                               : e.kind == RagdollEventKind3D::Settled   ? "settled"
                               : e.kind == RagdollEventKind3D::Recovered ? "recovered"
                                                                         : "blocked";
            auto row = lua.create_table_with("kind", kind);
            entity_field(row, "entity", actor);
            rows[++index] = row;
        }
        return rows;
    };
    scene["rope_state"] = [api](sol::this_state state, SceneEntity3D handle) -> sol::object {
        script_component_access("Rope3D", false);
        const auto native = rope(*api, handle);
        if (!native)
            return sol::nil;
        const auto value = api->physics->rope_state(native);
        sol::state_view lua{state};
        return sol::make_object(
            state,
            lua.create_table_with("segments", value.segments, "rest_length", value.rest_length, "target_length",
                                  value.target_length, "current_length", value.current_length, "broken", value.broken,
                                  "start_force", vector(lua, api->physics->rope_anchor_force(native, RopeEnd3D::Start)),
                                  "end_force", vector(lua, api->physics->rope_anchor_force(native, RopeEnd3D::End)),
                                  "start_tension", api->physics->rope_tension(native, RopeEnd3D::Start), "end_tension",
                                  api->physics->rope_tension(native, RopeEnd3D::End)));
    };
    scene["rope_points"] = [api](sol::this_state state, SceneEntity3D handle) -> sol::object {
        script_component_access("Rope3D", false);
        const auto native = rope(*api, handle);
        if (!native)
            return sol::nil;
        sol::state_view lua{state};
        auto rows = lua.create_table();
        int index = 0;
        for (const auto& point : api->physics->rope_points(native))
            rows[++index] =
                lua.create_table_with("position", vector(lua, point.position), "tangent", vector(lua, point.tangent));
        return sol::make_object(state, rows);
    };
    scene["rope_closest_point"] = [api](sol::this_state state, SceneEntity3D handle,
                                        sol::object position_argument) -> sol::object {
        const auto position = checked<sol::table>(position_argument, "position");

        script_component_access("Rope3D", false);
        const auto point = vector(position);
        const auto native = rope(*api, handle);
        if (!native)
            return sol::nil;
        const auto value = api->physics->rope_closest_point(native, point);
        if (!value)
            return sol::nil;
        sol::state_view lua{state};
        return sol::make_object(state,
                                lua.create_table_with("segment", value->segment + 1, "t", value->t, "along",
                                                      value->along, "position", vector(lua, value->point), "tangent",
                                                      vector(lua, value->tangent), "distance", value->distance));
    };
    scene["rope_point_at"] = [api](sol::this_state state, SceneEntity3D handle, sol::object segment_argument,
                                   f32 fraction) -> sol::object {
        const auto segment = checked<u32>(segment_argument, "segment");
        script_component_access("Rope3D", false);
        if (!std::isfinite(fraction) || fraction < 0 || fraction > 1 || !segment)
            throw std::runtime_error("rope point requires a one-based segment and fraction in 0..1");
        const auto native = rope(*api, handle);
        if (!native)
            return sol::nil;
        if (segment > api->physics->rope_state(native).segments)
            throw std::runtime_error("rope segment out of range");
        Vec3f point, tangent;
        if (!api->physics->rope_point_at(native, segment - 1, fraction - .5f, point, tangent))
            return sol::nil;
        sol::state_view lua{state};
        return sol::make_object(state,
                                lua.create_table_with("position", vector(lua, point), "tangent", vector(lua, tangent)));
    };
    const auto length_command = [api](SceneEntity3D handle, f32 target, f32 speed) {
        command_buffer(*api);
        script_component_access("Rope3D", true);
        if (!std::isfinite(target) || target <= 0 || !std::isfinite(speed) || speed < 0)
            throw std::runtime_error("rope target must be positive and speed nonnegative");
        if (!rope(*api, handle))
            return false;
        auto* component = resolve(*api, handle).get_mut<Rope3D>();
        if (!component)
            return false;
        component->target_length = target;
        component->reel_speed = speed;
        return true;
    };
    scene["set_rope_length"] = [length_command](SceneEntity3D handle, f32 target) {
        return length_command(handle, target, 0);
    };
    scene["reel_rope"] = length_command;
    scene["rope_events"] = [api](sol::this_state state, sol::object filter_argument) {
        const auto filter = optional_argument<SceneEntity3D>(filter_argument, "filter");
        physics(*api);
        if (filter)
            (void)resolve(*api, *filter);
        sol::state_view lua{state};
        auto rows = lua.create_table();
        int index = 0;
        for (const auto& e : api->bodies->rope_events()) {
            const auto entity = api->bodies->entity_of(e.user_id);
            const auto actor = entity && api->bodies->rope_of(entity) == e.rope ? api->queries.handle(EcsEntity{entity})
                                                                                : SceneEntity3D{};
            if (filter && actor != *filter)
                continue;
            auto row = lua.create_table_with("kind", "broke", "joint", e.joint, "force", e.force);
            entity_field(row, "entity", actor);
            rows[++index] = row;
        }
        return rows;
    };
}
} // namespace kin::scripting_detail::lua_scene3d_detail
