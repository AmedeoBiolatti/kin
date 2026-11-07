#include "script_engine_internal.hpp"
#include "script_scene_api.hpp"

#include "lua_api_surface.hpp"
#include "lua_l10n.hpp"
#include "lua_services.hpp"
#include "lua_paths.hpp"
#include "lua_reload_internal.hpp"
#include "lua_scene_context.hpp"
#include "lua_state_copy.hpp"
#include "lua_vm.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <kin/assets/content.hpp>
#include <kin/core/json.hpp>
#include <kin/dialogue/dialogue.hpp>
#include <kin/ecs/render.hpp>
#include <kin/ecs/system.hpp>
#include <kin/ecs/time.hpp>
#include <kin/platform/app.hpp>
#include <kin/platform/input.hpp>
#include <kin/platform/log.hpp>
#include <kin/scene/scene_manager.hpp>
#include <kin/scene/scene_preload2d.hpp>
#include <kin/scripting/script_engine.hpp>
#include <kin/scripting/script_scene.hpp>
#include <kin/scripting/workflows2d.hpp>
#include <optional>
#include <sol/sol.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace kin::scripting_detail {

thread_local bool reload_staging = false;
thread_local EcsCommandBuffer* script_commands = nullptr;
EcsCommandBuffer* current_script_commands() {
    return script_commands;
}

} // namespace kin::scripting_detail

namespace kin::s {

enum class ScriptComponentKind {
    Unknown,
    Transform,
    Rect,
    Line,
    ScriptOwned,
};

struct ScriptComponentFilter {
    ScriptComponentKind kind = ScriptComponentKind::Unknown;
    const ScriptComponentRegistry::ComponentEntry* custom = nullptr;
    std::string metadata_name;
    ComponentId metadata_id = 0;
};

struct ScriptCommandTarget {
    bool deferred = false;
    EcsId id = 0;
    EcsDeferredEntity deferred_entity{};
};

namespace {

Color script_color(i32 r, i32 g, i32 b) {
    const auto clamp = [](i32 value) {
        return static_cast<u8>(std::clamp(value, 0, 255));
    };
    return Color::rgb(clamp(r), clamp(g), clamp(b));
}





struct ScriptSystemAccessContext {
    std::string system_id;
    std::vector<std::string> all;
    std::vector<std::string> reads;
    std::vector<std::string> writes;
};

thread_local const ScriptSystemAccessContext* current_script_system_access = nullptr;

// The components the scene API knows itself, by their short and full names.
struct ScriptComponentName {
    std::string_view alias;
    std::string_view name;
    ScriptComponentKind kind;
};

constexpr ScriptComponentName script_component_names[] = {
    {"transform", "Transform2D", ScriptComponentKind::Transform},
    {"rect", "RectRenderer", ScriptComponentKind::Rect},
    {"line", "LineRenderer", ScriptComponentKind::Line},
    {"script_owned", "ScriptOwned", ScriptComponentKind::ScriptOwned},
};

ScriptComponentKind component_kind(std::string_view name) {
    for (const ScriptComponentName& known : script_component_names) {
        if (name == known.alias || name == known.name) {
            return known.kind;
        }
    }
    return ScriptComponentKind::Unknown;
}

const ScriptComponentRegistry::ComponentEntry* script_component_entry(const ScriptEngineHost* scene,
                                                                      std::string_view name) {
    const ScriptComponentRegistry* registry = scene ? scene->script_components() : nullptr;
    return registry ? registry->find(name) : nullptr;
}

bool has_component(const EcsEntity& entity, ScriptComponentKind kind) {
    switch (kind) {
    case ScriptComponentKind::Transform: return entity.has<Transform2D>();
    case ScriptComponentKind::Rect: return entity.has<RectRenderer>();
    case ScriptComponentKind::Line: return entity.has<LineRenderer>();
    case ScriptComponentKind::ScriptOwned: return entity.has<ScriptOwned>();
    case ScriptComponentKind::Unknown: return false;
    }
    return false;
}

bool has_component(const EcsEntity& entity, const ScriptComponentFilter& filter) {
    if (filter.custom) {
        return filter.custom->has(entity);
    }
    if (!filter.metadata_name.empty()) {
        return entity.raw().has(filter.metadata_id);
    }
    return has_component(entity, filter.kind);
}

bool contains_component_name(const std::vector<std::string>& names, std::string_view component) {
    return std::ranges::any_of(names, [&](const std::string& name) {
        return name == component;
    });
}

std::string canonical_component_name(std::string_view component) {
    const ScriptComponentKind kind = component_kind(component);
    for (const ScriptComponentName& known : script_component_names) {
        if (known.kind == kind) {
            return std::string{known.name};
        }
    }
    return std::string{component};
}

void warn_undeclared_script_access(std::string_view access, std::string_view component) {
    const ScriptSystemAccessContext* ctx = current_script_system_access;
    if (!ctx) {
        return;
    }
    const std::string canonical = canonical_component_name(component);
    const bool declared = access == "read"
        ? contains_component_name(ctx->reads, canonical) || contains_component_name(ctx->all, canonical)
        : contains_component_name(ctx->writes, canonical);
    if (declared) {
        return;
    }
    KIN_LOG_WARN_F("script",
                   "lua system component access is not declared",
                   (LogFields{
                       {.name = "system", .value = ctx->system_id},
                       {.name = "access", .value = std::string{access}},
                       {.name = "component", .value = canonical},
                   }));
}

bool matches_components(const EcsEntity& entity, const std::vector<ScriptComponentFilter>& components) {
    return std::ranges::all_of(components, [&](const ScriptComponentFilter& filter) {
        return has_component(entity, filter);
    });
}

std::vector<std::string> table_strings(const sol::table& table, std::string_view key) {
    std::vector<std::string> result;
    sol::object value = table[std::string{key}];
    if (value.get_type() != sol::type::table) {
        return result;
    }
    sol::table values = value.as<sol::table>();
    for (const auto& item : values) {
        if (item.second.is<std::string>()) {
            result.push_back(item.second.as<std::string>());
        }
    }
    return result;
}

SystemPhase script_phase(std::string_view name) {
    for (SystemPhase phase : system_phases()) {
        if (system_phase_name(phase) == name) {
            return phase;
        }
    }
    return SystemPhase::Update;
}

sol::object component_value_to_lua(sol::this_state state, const ComponentFieldValue& value) {
    sol::state_view lua{state};
    return std::visit([&](const auto& typed) -> sol::object {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<T, Vec2f>) {
            sol::table result = lua.create_table();
            result["x"] = typed.x;
            result["y"] = typed.y;
            return result;
        } else if constexpr (std::is_same_v<T, Vec3f> || std::is_same_v<T, Quatf>) {
            sol::table result = lua.create_table();
            result["x"] = typed.x; result["y"] = typed.y; result["z"] = typed.z;
            if constexpr (std::is_same_v<T, Quatf>) result["w"] = typed.w;
            return result;
        } else if constexpr (std::is_same_v<T, Vec2i>) {
            sol::table result = lua.create_table();
            result["x"] = typed.x;
            result["y"] = typed.y;
            return result;
        } else if constexpr (std::is_same_v<T, Color>) {
            sol::table result = lua.create_table();
            result["r"] = typed.r;
            result["g"] = typed.g;
            result["b"] = typed.b;
            result["a"] = typed.a;
            return result;
        } else if constexpr (std::is_same_v<T, ComponentEntityRef>) {
            sol::table result = lua.create_table();
            result["id"] = typed.id;
            result["authored_id"] = typed.authored_id;
            return result;
        } else if constexpr (std::is_same_v<T, ComponentAssetRef>) {
            return sol::make_object(state, typed.id);
        } else if constexpr (std::is_same_v<T, std::vector<Vec2f>>) {
            sol::table result = lua.create_table();
            for (std::size_t i = 0; i < typed.size(); ++i) {
                sol::table point = lua.create_table();
                point["x"] = typed[i].x;
                point["y"] = typed[i].y;
                result[i + 1] = point;
            }
            return result;
        } else {
            return sol::make_object(state, typed);
        }
    }, value);
}

std::optional<ComponentFieldValue> component_value_from_lua(ComponentFieldKind kind, sol::object object) {
    switch (kind) {
    case ComponentFieldKind::Bool:
        if (object.is<bool>()) return ComponentFieldValue{object.as<bool>()};
        break;
    case ComponentFieldKind::I32:
        if (object.is<double>() || object.is<int>()) return ComponentFieldValue{static_cast<i32>(object.as<double>())};
        break;
    case ComponentFieldKind::I64:
        if (object.is<double>() || object.is<int>()) return ComponentFieldValue{static_cast<i64>(object.as<double>())};
        break;
    case ComponentFieldKind::U32:
        if (object.is<double>() || object.is<int>()) return ComponentFieldValue{static_cast<u32>(std::max(0.0, object.as<double>()))};
        break;
    case ComponentFieldKind::U64:
        if (object.is<double>() || object.is<int>()) return ComponentFieldValue{static_cast<u64>(std::max(0.0, object.as<double>()))};
        break;
    case ComponentFieldKind::F32:
        if (object.is<double>() || object.is<int>()) return ComponentFieldValue{static_cast<f32>(object.as<double>())};
        break;
    case ComponentFieldKind::F64:
        if (object.is<double>() || object.is<int>()) return ComponentFieldValue{static_cast<f64>(object.as<double>())};
        break;
    case ComponentFieldKind::String:
        if (object.is<std::string>()) return ComponentFieldValue{object.as<std::string>()};
        break;
    case ComponentFieldKind::Vec2f:
        if (object.get_type() == sol::type::table) {
            sol::table table = object.as<sol::table>();
            return ComponentFieldValue{Vec2f{table.get_or("x", 0.0f), table.get_or("y", 0.0f)}};
        }
        break;
    case ComponentFieldKind::Vec2i:
        if (object.get_type() == sol::type::table) {
            sol::table table = object.as<sol::table>();
            return ComponentFieldValue{Vec2i{table.get_or("x", 0), table.get_or("y", 0)}};
        }
        break;
    case ComponentFieldKind::Vec3f:
    case ComponentFieldKind::Quatf:
        if (object.get_type() == sol::type::table) {
            sol::table table = object.as<sol::table>();
            ComponentFieldValue value = kind == ComponentFieldKind::Vec3f
                ? ComponentFieldValue{Vec3f{table.get_or("x", 0.0f), table.get_or("y", 0.0f), table.get_or("z", 0.0f)}}
                : ComponentFieldValue{Quatf{table.get_or("x", 0.0f), table.get_or("y", 0.0f), table.get_or("z", 0.0f), table.get_or("w", 1.0f)}};
            if (detail::field_value_matches_kind(value, kind)) return value;
        }
        break;
    case ComponentFieldKind::Color:
        if (object.get_type() == sol::type::table) {
            sol::table table = object.as<sol::table>();
            return ComponentFieldValue{Color::rgba(static_cast<u8>(table.get_or("r", 0)),
                                                   static_cast<u8>(table.get_or("g", 0)),
                                                   static_cast<u8>(table.get_or("b", 0)),
                                                   static_cast<u8>(table.get_or("a", 255)))};
        }
        break;
    case ComponentFieldKind::EntityRef:
        if (object.is<double>() || object.is<int>()) return ComponentFieldValue{ComponentEntityRef{.id = static_cast<EcsId>(object.as<double>())}};
        if (object.is<std::string>()) return ComponentFieldValue{ComponentEntityRef{.authored_id = object.as<std::string>()}};
        break;
    case ComponentFieldKind::AssetRef:
        if (object.is<std::string>()) return ComponentFieldValue{ComponentAssetRef{.id = object.as<std::string>()}};
        break;
    case ComponentFieldKind::Points:
        // {{x = 0, y = 0}, {x = 32, y = 0}, ...} (or {{0, 0}, {32, 0}, ...}).
        if (object.get_type() == sol::type::table) {
            sol::table table = object.as<sol::table>();
            std::vector<Vec2f> points;
            for (std::size_t i = 1; i <= table.size(); ++i) {
                sol::object item = table[i];
                if (item.get_type() != sol::type::table) {
                    return std::nullopt;
                }
                sol::table point = item.as<sol::table>();
                points.push_back({point.get_or("x", point.get_or(1, 0.0f)), point.get_or("y", point.get_or(2, 0.0f))});
            }
            ComponentFieldValue value{std::move(points)};
            if (detail::field_value_matches_kind(value, kind)) return value;
        }
        break;
    }
    return std::nullopt;
}

struct ComponentPatchValue {
    std::string field;
    ComponentFieldValue value;
};

std::vector<ComponentPatchValue> component_patch_values_from_lua(EcsWorld& world,
                                                                 std::string_view component,
                                                                 const sol::table& values) {
    const ComponentDescriptor* descriptor = world.components().find(component);
    if (!descriptor) {
        throw std::runtime_error("unknown component '" + std::string{component} + "'");
    }

    std::unordered_set<std::string> descriptor_fields;
    descriptor_fields.reserve(descriptor->fields.size());
    for (const ComponentFieldDescriptor& field : descriptor->fields) {
        descriptor_fields.insert(field.name);
    }
    for (const auto& entry : values) {
        if (!entry.first.is<std::string>()) {
            throw std::runtime_error("component patch keys must be strings");
        }
        const std::string field_name = entry.first.as<std::string>();
        if (field_name == "commit") {
            continue;
        }
        if (!descriptor_fields.contains(field_name)) {
            throw std::runtime_error("unknown field '" + field_name + "' on component '" + std::string{component} + "'");
        }
    }

    std::vector<ComponentPatchValue> result;
    for (const ComponentFieldDescriptor& field : descriptor->fields) {
        sol::object value = values[field.name];
        if (value.get_type() == sol::type::nil) {
            continue;
        }
        if (std::optional<ComponentFieldValue> converted = component_value_from_lua(field.kind, value)) {
            result.push_back(ComponentPatchValue{.field = field.name, .value = std::move(*converted)});
        } else {
            throw std::runtime_error("invalid value for field '" + field.name + "' on component '" + std::string{component} + "'");
        }
    }
    return result;
}

void apply_component_patch_values(EcsWorld& world,
                                  EcsEntity entity,
                                  std::string_view component,
                                  const std::vector<ComponentPatchValue>& values) {
    const ComponentDescriptor* descriptor = world.components().find(component);
    if (!descriptor) {
        throw std::runtime_error("unknown component '" + std::string{component} + "'");
    }
    if (!world.components().has(entity, descriptor->id) && !world.components().add(entity, descriptor->id)) {
        throw std::runtime_error(world.components().last_error());
    }
    for (const ComponentPatchValue& value : values) {
        if (!world.components().patch_field(entity, descriptor->id, value.field, value.value)) {
            throw std::runtime_error(world.components().last_error());
        }
    }
}

flecs::entity find_entity_by_name_recursive(flecs::entity parent, std::string_view name) {
    flecs::entity found;
    parent.children([&](flecs::entity child) {
        if (found.is_valid() && found.is_alive()) {
            return;
        }
        EcsEntity candidate{child};
        if (candidate.alive() && candidate.name() == name) {
            found = child;
            return;
        }
        found = find_entity_by_name_recursive(child, name);
    });
    return found;
}

std::optional<ScriptCommandTarget> command_target_from_lua(const sol::object& object) {
    if (object.is<ScriptEntityApi>()) {
        const auto& entity = object.as<const ScriptEntityApi&>();
        if (!entity.alive())
            throw std::runtime_error(scripting_detail::stale_entity_message);
        return ScriptCommandTarget{.id = entity.id()};
    }
    if (object.is<ScriptDeferredEntityApi>()) {
        ScriptDeferredEntityApi entity = object.as<ScriptDeferredEntityApi>();
        return ScriptCommandTarget{.deferred = true, .deferred_entity = entity.entity};
    }
    if (object.is<double>() || object.is<int>()) {
        return ScriptCommandTarget{.id = static_cast<EcsId>(object.as<double>())};
    }
    return std::nullopt;
}

} // namespace

void ScriptSceneApi::collect_matching(const std::vector<ScriptComponentFilter>& components, auto&& callback) {
    if (!components.empty() && components[0].kind == ScriptComponentKind::Unknown && !components[0].custom && components[0].metadata_name.empty()) {
        return;
    }

    const auto visit = [&](EcsEntity entity) {
        if (entity.alive() && !entity.raw().has<flecs::Component>() && matches_components(entity, components)) {
            callback(entity);
        }
    };

    if (components.empty()) {
        scene->ecs().each([&](flecs::entity raw) {
            visit(EcsEntity{raw});
        });
        return;
    }

    if (components[0].custom) {
        std::vector<EcsEntity> candidates;
        components[0].custom->collect(scene->ecs(), candidates);
        for (EcsEntity entity : candidates) {
            visit(entity);
        }
        return;
    }

    if (!components[0].metadata_name.empty()) {
        const auto plan = scene->ecs().build_query_plan({.all = {components[0].metadata_name}});
        for (auto entity : scene->ecs().query_entities(plan)) visit(entity);
        return;
    }

    switch (components[0].kind) {
    case ScriptComponentKind::Transform:
        scene->ecs().query<Transform2D>().each_entity([&](EcsEntity entity, const Transform2D&) {
            visit(entity);
        });
        break;
    case ScriptComponentKind::Rect:
        scene->ecs().query<RectRenderer>().each_entity([&](EcsEntity entity, const RectRenderer&) {
            visit(entity);
        });
        break;
    case ScriptComponentKind::Line:
        scene->ecs().query<LineRenderer>().each_entity([&](EcsEntity entity, const LineRenderer&) {
            visit(entity);
        });
        break;
    case ScriptComponentKind::ScriptOwned:
        scene->ecs().query<ScriptOwned>().each_entity([&](EcsEntity entity, const ScriptOwned&) {
            visit(entity);
        });
        break;
    case ScriptComponentKind::Unknown:
        break;
    }
}

std::string ScriptEntityApi::name() const {
    return entity.name();
}

void ScriptEntityApi::destroy() {
    entity.destroy();
}

bool ScriptEntityApi::has(std::string_view component) const {
    const ScriptComponentKind kind = component_kind(component);
    if (kind != ScriptComponentKind::Unknown) {
        return has_component(entity, kind);
    }
    const ScriptComponentRegistry::ComponentEntry* entry = script_component_entry(scene, component);
    if (entry) {
        return entry->has(entity);
    }
    return scene ? scene->ecs().components().has(entity, component) : false;
}

ScriptEntityApi& ScriptEntityApi::add_script_owned() {
    entity.add<ScriptOwned>();
    return *this;
}

ScriptEntityApi& ScriptEntityApi::remove(std::string_view component) {
    switch (component_kind(component)) {
    case ScriptComponentKind::Transform:
        entity.remove<Transform2D>();
        break;
    case ScriptComponentKind::Rect:
        entity.remove<RectRenderer>();
        break;
    case ScriptComponentKind::Line:
        entity.remove<LineRenderer>();
        break;
    case ScriptComponentKind::ScriptOwned:
        entity.remove<ScriptOwned>();
        break;
    case ScriptComponentKind::Unknown:
        if (const ScriptComponentRegistry::ComponentEntry* entry = script_component_entry(scene, component)) {
            entry->remove(entity);
            break;
        }
        if (scene) {
            scene->ecs().components().remove(entity, component);
        }
        break;
    }
    return *this;
}

sol::object ScriptEntityApi::get(sol::this_state state, std::string_view component) const {
    warn_undeclared_script_access("read", component);
    switch (component_kind(component)) {
    case ScriptComponentKind::Transform:
        return get_transform(state);
    case ScriptComponentKind::Rect:
        return get_rect(state);
    case ScriptComponentKind::Line:
        return get_line(state);
    case ScriptComponentKind::ScriptOwned:
        return has("script_owned") ? sol::make_object(state, true) : sol::object{sol::nil};
    case ScriptComponentKind::Unknown:
        break;
    }

    const ScriptComponentRegistry::ComponentEntry* entry = script_component_entry(scene, component);
    if (entry) {
        return entry->get(state, entity);
    }
    if (!scene) {
        return sol::object{sol::nil};
    }
    sol::state_view lua{state};
    sol::table result = lua.create_table();
    if (!scene->ecs().components().visit_fields(entity, component,
            [&](std::string_view name, ComponentFieldKind, const ComponentFieldValue& value) {
                result[std::string{name}] = component_value_to_lua(state, value);
            })) {
        return sol::object{sol::nil};
    }
    return sol::object{result};
}

sol::object ScriptEntityApi::child(sol::this_state state, std::string_view name) const {
    flecs::entity found = entity.raw().lookup(std::string{name}.c_str());
    if (!found.is_valid() || !found.is_alive()) {
        return sol::nil;
    }
    return sol::make_object(state, ScriptEntityApi{scene, EcsEntity{found}, lease});
}

ScriptEntityApi& ScriptEntityApi::patch(std::string_view component, sol::table values) {
    warn_undeclared_script_access("write", component);
    const ScriptComponentRegistry::ComponentEntry* entry = script_component_entry(scene, component);
    if (entry) {
        entry->patch(entity, values);
        return *this;
    }
    if (!scene) {
        return *this;
    }
    const std::vector<ComponentPatchValue> patch_values = component_patch_values_from_lua(scene->ecs(), component, values);
    apply_component_patch_values(scene->ecs(), entity, component, patch_values);
    return *this;
}

sol::object ScriptEntityApi::component(sol::this_state state, std::string_view component) const {
    warn_undeclared_script_access("read", component);
    warn_undeclared_script_access("write", component);
    if (!scene) {
        return sol::nil;
    }
    const ComponentDescriptor* descriptor = scene->ecs().components().find(component);
    if (!descriptor) {
        throw std::runtime_error("unknown component '" + std::string{component} + "'");
    }
    sol::state_view lua{state};
    sol::table result = lua.create_table();
    scene->ecs().components().visit_fields(entity, descriptor->id,
        [&](std::string_view name, ComponentFieldKind, const ComponentFieldValue& value) {
            result[std::string{name}] = component_value_to_lua(state, value);
        });
    const std::string component_name{component};
    result["commit"] = [scene = scene, entity = entity, component_name, lease = lease](sol::stack_object self) -> sol::table {
        if (self.get_type() != sol::type::table)
            throw std::runtime_error("component commit requires its table; call with ':'");
        if (scene->closing || !entity.alive() || (lease && !lease()))
            throw std::runtime_error(scripting_detail::stale_entity_message);
        sol::table table = self.as<sol::table>();
        const std::vector<ComponentPatchValue> patch_values = component_patch_values_from_lua(scene->ecs(), component_name, table);
        apply_component_patch_values(scene->ecs(), entity, component_name, patch_values);
        return table;
    };
    return sol::object{result};
}

ScriptEntityApi& ScriptEntityApi::timer(std::string_view, f32 duration, sol::optional<bool> repeating) {
    if (!scene) {
        return *this;
    }
    const ComponentDescriptor* descriptor = scene->ecs().components().find("Timer");
    if (!descriptor) {
        throw std::runtime_error("Timer component is not registered");
    }
    if (!scene->ecs().components().has(entity, descriptor->id) && !scene->ecs().components().add(entity, descriptor->id)) {
        throw std::runtime_error(scene->ecs().components().last_error());
    }
    const bool repeat = repeating.value_or(false);
    const ComponentPatchValue values[] = {
        {.field = "duration", .value = std::max(duration, 0.001f)},
        {.field = "elapsed", .value = f32{0.0f}},
        {.field = "repeating", .value = repeat},
        {.field = "finished", .value = false},
        {.field = "ticks", .value = i32{0}},
    };
    for (const ComponentPatchValue& value : values) {
        if (!scene->ecs().components().patch_field(entity, descriptor->id, value.field, value.value)) {
            throw std::runtime_error(scene->ecs().components().last_error());
        }
    }
    return *this;
}

ScriptEntityApi& ScriptEntityApi::cooldown(std::string_view name, f32 seconds) {
    return timer(name, seconds, false);
}

bool ScriptEntityApi::ready(std::string_view) const {
    if (!scene) {
        return false;
    }
    bool ready = false;
    scene->ecs().components().visit_fields(entity, "Timer",
        [&](std::string_view field, ComponentFieldKind, const ComponentFieldValue& value) {
            if (field == "finished") {
                if (const bool* finished = std::get_if<bool>(&value)) {
                    ready = *finished;
                }
            }
        });
    return ready;
}

ScriptEntityApi& ScriptEntityApi::set_transform(f32 x, f32 y) {
    entity.set(Transform2D{{x, y}});
    return *this;
}

sol::object ScriptEntityApi::get_transform(sol::this_state state) const {
    const Transform2D* transform = entity.get<Transform2D>();
    if (!transform) {
        return sol::nil;
    }
    sol::state_view lua{state};
    sol::table result = lua.create_table();
    result["x"] = transform->pos.x;
    result["y"] = transform->pos.y;
    return result;
}

ScriptEntityApi& ScriptEntityApi::translate(f32 x, f32 y) {
    Transform2D& transform = entity.ensure<Transform2D>();
    transform.pos.x += x;
    transform.pos.y += y;
    entity.modified<Transform2D>();
    return *this;
}

ScriptEntityApi& ScriptEntityApi::set_rect(f32 w,
                          f32 h,
                          i32 r,
                          i32 g,
                          i32 b,
                          sol::optional<i32> layer,
                          sol::optional<i32> order) {
    entity.set(RectRenderer{
        .size = {w, h},
        .color = script_color(r, g, b),
        .layer = layer.value_or(layer_value(RenderLayer::World)),
        .order = order.value_or(0),
    });
    return *this;
}

sol::object ScriptEntityApi::get_rect(sol::this_state state) const {
    const RectRenderer* rect = entity.get<RectRenderer>();
    if (!rect) {
        return sol::nil;
    }
    sol::state_view lua{state};
    sol::table result = lua.create_table();
    result["w"] = rect->size.x;
    result["h"] = rect->size.y;
    result["r"] = rect->color.r;
    result["g"] = rect->color.g;
    result["b"] = rect->color.b;
    result["layer"] = rect->layer;
    result["order"] = rect->order;
    result["visible"] = rect->visible;
    return result;
}

ScriptEntityApi& ScriptEntityApi::set_line(f32 ax,
                          f32 ay,
                          f32 bx,
                          f32 by,
                          i32 r,
                          i32 g,
                          i32 b,
                          sol::optional<i32> layer,
                          sol::optional<i32> order) {
    entity.set(LineRenderer{
        .a = {ax, ay},
        .b = {bx, by},
        .color = script_color(r, g, b),
        .layer = layer.value_or(layer_value(RenderLayer::World)),
        .order = order.value_or(0),
    });
    return *this;
}

sol::object ScriptEntityApi::get_line(sol::this_state state) const {
    const LineRenderer* line = entity.get<LineRenderer>();
    if (!line) {
        return sol::nil;
    }
    sol::state_view lua{state};
    sol::table result = lua.create_table();
    result["ax"] = line->a.x;
    result["ay"] = line->a.y;
    result["bx"] = line->b.x;
    result["by"] = line->b.y;
    result["r"] = line->color.r;
    result["g"] = line->color.g;
    result["b"] = line->color.b;
    result["layer"] = line->layer;
    result["order"] = line->order;
    result["visible"] = line->visible;
    return result;
}

ScriptDeferredEntityApi ScriptCommandBufferApi::create(sol::optional<std::string> name) {
    require_commands();
    return ScriptDeferredEntityApi{commands->create_entity(name.value_or(""))};
}

ScriptCommandBufferApi& ScriptCommandBufferApi::destroy(sol::object target) {
    require_commands();
    ScriptCommandTarget resolved = require_target(target);
    if (resolved.deferred) {
        commands->destroy(resolved.deferred_entity);
    } else {
        commands->destroy(resolved.id);
    }
    return *this;
}

ScriptCommandBufferApi& ScriptCommandBufferApi::add(sol::object target, std::string_view component) {
    require_commands();
    ScriptCommandTarget resolved = require_target(target);
    switch (component_kind(component)) {
    case ScriptComponentKind::ScriptOwned:
        if (resolved.deferred) {
            commands->add<ScriptOwned>(resolved.deferred_entity);
        } else {
            commands->add<ScriptOwned>(resolved.id);
        }
        break;
    case ScriptComponentKind::Transform:
    case ScriptComponentKind::Rect:
    case ScriptComponentKind::Line:
    case ScriptComponentKind::Unknown:
        if (resolved.deferred) {
            commands->add(resolved.deferred_entity, std::string{component});
        } else {
            commands->add(resolved.id, std::string{component});
        }
        break;
    }
    return *this;
}

ScriptCommandBufferApi& ScriptCommandBufferApi::remove(sol::object target, std::string_view component) {
    require_commands();
    ScriptCommandTarget resolved = require_target(target);
    switch (component_kind(component)) {
    case ScriptComponentKind::ScriptOwned:
        if (resolved.deferred) {
            commands->remove<ScriptOwned>(resolved.deferred_entity);
        } else {
            commands->remove<ScriptOwned>(resolved.id);
        }
        break;
    case ScriptComponentKind::Transform:
    case ScriptComponentKind::Rect:
    case ScriptComponentKind::Line:
    case ScriptComponentKind::Unknown:
        if (resolved.deferred) {
            commands->remove(resolved.deferred_entity, std::string{component});
        } else {
            commands->remove(resolved.id, std::string{component});
        }
        break;
    }
    return *this;
}

ScriptCommandBufferApi& ScriptCommandBufferApi::patch(sol::object target, std::string_view component, sol::table values) {
    require_commands();
    if (!scene) {
        throw std::runtime_error("script command buffer has no scene");
    }
    warn_undeclared_script_access("write", component);
    ScriptCommandTarget resolved = require_target(target);
    const std::vector<ComponentPatchValue> patch_values = component_patch_values_from_lua(scene->ecs(), component, values);
    for (const ComponentPatchValue& value : patch_values) {
        if (resolved.deferred) {
            commands->patch_field(resolved.deferred_entity, std::string{component}, value.field, value.value);
        } else {
            commands->patch_field(resolved.id, std::string{component}, value.field, value.value);
        }
    }
    return *this;
}

void ScriptCommandBufferApi::require_commands() const {
    if (!commands) {
        throw std::runtime_error("ECS command buffer is not available for this script callback");
    }
}

ScriptCommandTarget ScriptCommandBufferApi::require_target(const sol::object& target) const {
    std::optional<ScriptCommandTarget> resolved = command_target_from_lua(target);
    if (!resolved) {
        throw std::runtime_error("ECS command target must be an entity, deferred entity, or entity id");
    }
    if (resolved->deferred && !commands->owns(resolved->deferred_entity))
        throw std::runtime_error("stale or foreign deferred entity handle");
    return *resolved;
}

ScriptEntityApi ScriptSceneApi::entity(sol::optional<std::string> name) {
    EcsEntity created = name ? scene->ecs().entity(*name) : scene->ecs().entity();
    created.add<ScriptOwned>();
    return ScriptEntityApi{scene, created};
}

sol::object ScriptSceneApi::find(sol::this_state state, std::string_view name) {
    flecs::entity found = scene->ecs().raw().lookup(std::string{name}.c_str());
    if (!found.is_valid() || !found.is_alive()) {
        scene->ecs().each([&](flecs::entity entity) {
            if (found.is_valid() && found.is_alive()) {
                return;
            }
            EcsEntity candidate{entity};
            if (candidate.alive() && candidate.name() == name) {
                found = entity;
                return;
            }
            found = find_entity_by_name_recursive(entity, name);
            if (found.is_valid() && found.is_alive()) {
                return;
            }
        });
    }
    if (!found.is_valid() || !found.is_alive()) {
        return sol::nil;
    }
    return sol::make_object(state, ScriptEntityApi{scene, EcsEntity{found}});
}

sol::object ScriptSceneApi::authored(sol::this_state state, std::string_view id) {
    EcsEntity found = scene->ecs().entities().find_by_authored_id(id);
    if (!found) {
        return sol::nil;
    }
    return sol::make_object(state, ScriptEntityApi{scene, found});
}

sol::table ScriptSceneApi::entities(sol::this_state state) {
    sol::state_view lua{state};
    sol::table result = lua.create_table();
    i32 index = 1;
    scene->ecs().each([&](flecs::entity entity) {
        if (entity.is_valid() && entity.is_alive() && !entity.has<flecs::Component>()) {
            result[index++] = ScriptEntityApi{scene, EcsEntity{entity}};
        }
    });
    return result;
}

sol::table ScriptSceneApi::with(sol::this_state state, sol::variadic_args args) {
    sol::state_view lua{state};
    sol::table result = lua.create_table();
    const std::vector<ScriptComponentFilter> components = component_filters(args);

    i32 index = 1;
    collect_matching(components, [&](EcsEntity entity) {
        result[index++] = ScriptEntityApi{scene, entity};
    });
    return result;
}

i32 ScriptSceneApi::count(sol::variadic_args args) {
    const std::vector<ScriptComponentFilter> components = component_filters(args);
    i32 result = 0;
    collect_matching(components, [&](EcsEntity) {
        ++result;
    });
    return result;
}

void ScriptSceneApi::clear_script_entities() {
    scene->clear_script_entities();
}

void ScriptSceneApi::set_clear_color(i32 r, i32 g, i32 b) { scene->set_clear_color(script_color(r, g, b)); }

void ScriptSceneApi::mark_render_cache_dirty() {
    scene->mark_render_cache_dirty("script api");
}

bool ScriptSceneApi::register_system(std::string_view id, sol::table descriptor, sol::protected_function run) {
    if (scripting_detail::reload_staging) {
        const auto current = scene->world->systems().snapshots();
        if (std::ranges::any_of(current, [&](const auto& entry) { return entry.id == id; }) &&
            std::ranges::find(scene->replaceable, id) == scene->replaceable.end())
            throw std::runtime_error("reload system ID belongs to another owner");
        if (scene->definitions.size() >= 128 ||
            std::ranges::any_of(scene->definitions, [&](const auto& entry) { return entry.first.id == id; }))
            throw std::runtime_error("duplicate/too many reload system declarations");
    }
    EcsQueryPlanDescriptor query_descriptor{
        .all = table_strings(descriptor, "all"),
        .none = table_strings(descriptor, "none"),
        .reads = table_strings(descriptor, "reads"),
        .writes = table_strings(descriptor, "writes"),
    };
    EcsQueryPlan plan = scene->world->build_query_plan(query_descriptor);
    if (!plan.valid) {
        return false;
    }

    SystemDescriptor system_descriptor{
        .id = std::string{id},
        .name = descriptor.get_or("name", std::string{id}),
        .kind = SystemKind::Script,
        .phase = script_phase(descriptor.get_or("phase", std::string{"update"})),
        .order = descriptor.get_or("order", 0),
        .interval_seconds = descriptor.get_or("interval_seconds", 0.0f),
        .rate = descriptor.get_or("rate", 1),
        .reads = query_descriptor.reads,
        .writes = query_descriptor.writes,
        .structural_mutation_policy = SystemStructuralMutationPolicy::CommandBufferOnly,
        .mutation_notes = "Lua systems receive a command buffer through the third callback argument.",
        .source_file = scene->resolved_script_path().string(),
        .owner_scope = SystemOwnerScope::Scene,
    };
    auto access = std::make_shared<ScriptSystemAccessContext>(ScriptSystemAccessContext{
        .system_id = std::string{id},
        .all = query_descriptor.all,
        .reads = query_descriptor.reads,
        .writes = query_descriptor.writes,
    });
    if (scripting_detail::reload_staging) {
        EcsWorld validation;
        scene->world->components().copy_schema_to(validation.components());
        if (validation.systems().register_task(system_descriptor, [](SystemContext&) {}).empty())
            throw std::runtime_error("invalid reload system descriptor");
    }
    auto callback = [scene = scene, plan = std::move(plan), run = std::move(run),
                     access = std::move(access)](SystemContext& ctx) mutable {
        LuaCall budget{*scene->runtime, run.lua_state(), "system." + access->system_id};
        for (EcsEntity entity : scene->ecs().query_entities(plan)) {
            const ScriptSystemAccessContext* previous = current_script_system_access;
            auto* previous_commands = scripting_detail::script_commands;
            struct Reset {
                const ScriptSystemAccessContext* access;
                EcsCommandBuffer* commands;
                ~Reset() {
                    current_script_system_access = access;
                    scripting_detail::script_commands = commands;
                }
            } reset{previous, previous_commands};
            current_script_system_access = access.get();
            scripting_detail::script_commands = ctx.commands;
            ScriptSystemContextApi system_context{
                .scene = scene,
                .commands_ptr = ctx.commands,
                .delta_time = ctx.dt,
                .id = std::string{ctx.system_id},
                .phase_name = std::string{system_phase_name(ctx.phase)},
                .parallel = ctx.running_parallel,
            };
            sol::protected_function_result result = run(ScriptEntityApi{scene, entity}, ctx.dt, system_context);
            current_script_system_access = previous;
            if (!result.valid()) {
                budget.result(false);
                sol::error error = result;
                throw std::runtime_error("system '" + access->system_id +
                                         "' entity=" + std::to_string(entity.id()) + ": " + error.what());
            }
        }
    };
    scene->definitions.emplace_back(system_descriptor, callback);
    if (scripting_detail::reload_staging)
        return true;
    const bool registered =
        !scene->ecs().systems().register_task(std::move(system_descriptor), std::move(callback)).empty();
    if (registered)
        scene->systems.emplace_back(id);
    return registered;
}

std::vector<ScriptComponentFilter> ScriptSceneApi::component_filters(sol::variadic_args args) const {
    std::vector<ScriptComponentFilter> components;
    for (sol::object arg : args) {
        if (arg.is<std::string>()) {
            const std::string name = arg.as<std::string>();
            const ScriptComponentKind kind = component_kind(name);
            if (kind != ScriptComponentKind::Unknown) {
                components.push_back({.kind = kind});
                continue;
            }

            const ScriptComponentRegistry::ComponentEntry* entry = script_component_entry(scene, name);
            if (!entry) {
                if (const auto* descriptor = scene->ecs().components().find(name)) {
                    components.push_back({.kind = ScriptComponentKind::Unknown, .metadata_name = name, .metadata_id = descriptor->id});
                    continue;
                }
                components.clear();
                components.push_back({.kind = ScriptComponentKind::Unknown});
                return components;
            }
            components.push_back({.kind = ScriptComponentKind::Unknown, .custom = entry});
        }
    }
    return components;
}

void ScriptAppApi::quit() {
    if (!app)
        throw std::runtime_error("script host has no application");
    app->quit();
}

bool ScriptDialoguePlayerApi::load(std::string_view path) {
    DialogueLoadResult result = load_dialogue(std::filesystem::path{path});
    if (!result.ok()) {
        return false;
    }
    document = std::move(*result.document);
    return true;
}

void ScriptDialoguePlayerApi::start(sol::optional<std::string> start_node) {
    player.start(document, start_node.value_or(""));
}

void ScriptDialoguePlayerApi::advance() {
    player.advance();
}

bool ScriptDialoguePlayerApi::choose_index(i32 index) {
    return player.choose(index);
}

bool ScriptDialoguePlayerApi::choose_id(std::string_view id) {
    return player.choose(id);
}

void ScriptDialoguePlayerApi::skip_line() {
    player.skip_line();
}

void ScriptDialoguePlayerApi::reset() {
    player.reset();
}

sol::table ScriptDialoguePlayerApi::view(sol::this_state state) const {
    sol::state_view lua{state};
    const DialogueViewModel view = player.current_view(true);
    sol::table result = lua.create_table();
    result["active"] = view.active;
    result["ended"] = view.ended;
    result["speaker_id"] = view.speaker_id;
    result["speaker_name"] = view.speaker_name;
    result["text"] = view.text_markup;
    sol::table choices = lua.create_table();
    for (std::size_t i = 0; i < view.choices.size(); ++i) {
        const DialogueChoiceView& choice = view.choices[i];
        sol::table row = lua.create_table();
        row["id"] = choice.id;
        row["text"] = choice.text_markup;
        row["enabled"] = choice.enabled;
        row["visited"] = choice.visited;
        choices[static_cast<int>(i + 1)] = row;
    }
    result["choices"] = choices;
    return result;
}

sol::table ScriptDialoguePlayerApi::consume_events(sol::this_state state) {
    sol::state_view lua{state};
    sol::table result = lua.create_table();
    const std::vector<DialogueEvent> events = player.consume_events();
    for (std::size_t i = 0; i < events.size(); ++i) {
        sol::table row = lua.create_table();
        row["id"] = events[i].id;
        row["source_node"] = events[i].node_id;
        result[static_cast<int>(i + 1)] = row;
    }
    return result;
}

} // namespace kin::s

namespace kin {

using namespace scripting_detail;
using namespace scripting_detail::scene_api;

EcsEntity scripting_detail::script_entity(const sol::object& object) {
    if (!object.is<ScriptEntityApi>())
        throw std::runtime_error("expected a KinEntity");
    const auto& value = object.as<const ScriptEntityApi&>();
    if (!value.scene || value.scene->closing || !value.alive())
        throw std::runtime_error(stale_entity_message);
    return value.entity;
}

void scripting_detail::script_component_access(std::string_view component, bool write) {
    warn_undeclared_script_access(write ? "write" : "read", component);
}

std::unique_ptr<ScriptEngine> ScriptEngine::prepare_reload(const std::filesystem::path& root,
                                                           const std::filesystem::path& path,
                                                           std::string& error) const {
    try {
        if (!_impl->host)
            throw std::runtime_error("script has no live scene bindings");
        if (!path_within_root(root / path, root))
            throw std::runtime_error("reload script must be inside the candidate content root");
        auto next = std::make_unique<ScriptEngine>(std::function<void(sol::state&)>{}, _impl->runtime().limits());
        const scripting_detail::ReloadStaging staging;
        const LuaInstructionBudget budget{lua_maintenance_budget};
        if (!next->load_file(root, path))
            throw std::runtime_error(next->last_error());
        next->_impl->host = std::make_unique<ScriptEngineHost>();
        static_cast<ScriptSceneBindings&>(*next->_impl->host) = static_cast<const ScriptSceneBindings&>(*_impl->host);
        next->_impl->host->runtime = &next->_impl->runtime();
        next->_impl->host->script_path = root / path;
        next->_impl->host->replaceable = _impl->host->systems;
        sol::object state;
        {
            LuaNativeAllocations native{next->_impl->runtime()};
            state = scripting_detail::copy_lua_data(_impl->lua().globals().raw_get<sol::object>("reload_state"),
                                                    next->_impl->lua());
            auto* lua = next->_impl->lua().lua_state();
            lua_pushglobaltable(lua);
            lua_pushliteral(lua, "reload_state");
            sol::stack::push(lua, state);
            lua_rawset(lua, -3);
            lua_pop(lua, 1);
        }
        if (next->_impl->runtime().memory_exceeded())
            throw std::runtime_error("scene reload exceeds Lua memory limit");
        sol::protected_function migrate = next->_impl->lua().globals().raw_get<sol::object>("on_reload");
        if (!migrate.valid())
            throw std::runtime_error("safe live reload requires on_reload(state, scene); on_load is not rerun");
        LuaCall call{next->_impl->runtime(), next->_impl->vm.state(), "on_reload"};
        if (!record_result(next->_last_error, "on_reload",
                           call.invoke(migrate, state, ScriptSceneApi{next->_impl->host.get()})))
            throw std::runtime_error(next->last_error());
        next->_impl->workflows = _impl->workflows;
        next->_impl->bind = _impl->bind;
        if (next->_impl->bind)
            next->_impl->bind(next->_impl->lua());
        return next;
    } catch (const std::exception& e) {
        error = e.what();
        return {};
    }
}

template <typename... Args>
bool ScriptEngine::call_optional(std::string_view name, Args&&... args) const {
    if (!_impl->halted.empty()) {
        _last_error = _impl->halted;
        return false;
    }
    sol::object object = _impl->lua().globals().raw_get<sol::object>(std::string{name});
    if (object.get_type() == sol::type::nil) {
        return true;
    }
    if (object.get_type() != sol::type::function) {
        _last_error = std::string{name} + " is not a function";
        return false;
    }
    sol::protected_function function = object;
    LuaCall call{_impl->runtime(), _impl->vm.state(), name};
    return record_result(_last_error, name, call.invoke(function, std::forward<Args>(args)...));
}

// Keep native-userdata pushing and sol2 invocation templates out of the runtime
// implementation. Its callers only need these three explicit specializations.
template bool ScriptEngine::call_optional<ScriptSceneApi>(std::string_view, ScriptSceneApi&&) const;
template bool ScriptEngine::call_optional<ScriptContextApi>(std::string_view, ScriptContextApi&&) const;
template bool ScriptEngine::call_optional<ScriptActionsApi>(std::string_view, ScriptActionsApi&&) const;

LuaApiSurface ScriptEngine::api_surface() const {
    LuaApiSurface surface = scripting_detail::lua_api_surface(_impl->lua());
    for (const ScriptComponentName& known : script_component_names) {
        surface.component_aliases.emplace(known.alias, known.name);
    }
    surface.component_fields = {
        {"Transform2D", {{"x", "f32"}, {"y", "f32"}}},
        {"RectRenderer",
         {{"w", "f32"},
          {"h", "f32"},
          {"r", "u8"},
          {"g", "u8"},
          {"b", "u8"},
          {"layer", "i32"},
          {"order", "i32"},
          {"visible", "bool"}}},
        {"LineRenderer",
         {{"ax", "f32"},
          {"ay", "f32"},
          {"bx", "f32"},
          {"by", "f32"},
          {"r", "u8"},
          {"g", "u8"},
          {"b", "u8"},
          {"layer", "i32"},
          {"order", "i32"},
          {"visible", "bool"}}},
        {"ScriptOwned", {}},
    };
    surface.callbacks = {
        {"on_load", {"KinScene"}},   {"on_reload", {"any", "KinScene"}}, {"on_enter", {"KinContext"}},
        {"on_exit", {"KinContext"}}, {"update", {"KinContext"}},         {"collect_actions", {"KinActions"}},
        {"write_report", {}},
    };
    return surface;
}

} // namespace kin
