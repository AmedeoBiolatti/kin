#include <kin/scripting/script_engine.hpp>

#include "lua_l10n.hpp"
#include "lua_paths.hpp"

#include <kin/assets/content.hpp>
#include <kin/core/json.hpp>
#include <kin/dialogue/dialogue.hpp>
#include <kin/ecs/render.hpp>
#include <kin/ecs/system.hpp>
#include <kin/ecs/time.hpp>
#include <kin/platform/input.hpp>
#include <kin/platform/log.hpp>
#include <kin/scripting/script_scene.hpp>

#include <sol/sol.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace kin {

struct ScriptEngine::Impl {
    sol::state lua;
    std::filesystem::path asset_root;
    std::vector<std::filesystem::path> dependencies;
    std::unordered_set<std::string> dependency_keys;
};

namespace {

bool record_result(std::string& last_error, std::string_view name, const sol::protected_function_result& result) {
    if (result.valid()) {
        last_error.clear();
        return true;
    }

    sol::error error = result;
    last_error = std::string{name} + ": " + error.what();
    return false;
}

using scripting_detail::module_name_allowed;
using scripting_detail::path_within_root;

std::filesystem::path normalized_dependency_path(const std::filesystem::path& path) {
    return scripting_detail::normalized_path(path);
}

std::optional<std::filesystem::path> resolve_lua_module(const std::filesystem::path& asset_root, std::string_view module) {
    if (!module_name_allowed(module)) {
        return std::nullopt;
    }
    std::string relative{module};
    std::ranges::replace(relative, '.', '/');
    relative += ".lua";

    const std::filesystem::path candidates[] = {
        asset_root / "scripts" / relative,
        asset_root / relative,
    };
    for (const std::filesystem::path& candidate : candidates) {
        if (path_within_root(candidate, asset_root) && content_file_exists(candidate)) {
            return normalized_dependency_path(candidate);
        }
    }
    return std::nullopt;
}

// Loads a script from the content (a mounted pack or disk), named as
// luaL_loadfile names it, so errors read "path:line: message". Nullopt if the
// file cannot be read.
std::optional<sol::load_result> load_lua_file(sol::state_view lua, const std::filesystem::path& path) {
    const std::optional<std::string> code = read_content_file(path);
    if (!code) {
        return std::nullopt;
    }
    return lua.load(*code, "@" + path.string());
}

Color script_color(i32 r, i32 g, i32 b) {
    const auto clamp = [](i32 value) {
        return static_cast<u8>(std::clamp(value, 0, 255));
    };
    return Color::rgb(clamp(r), clamp(g), clamp(b));
}

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
};

struct ScriptSystemAccessContext {
    std::string system_id;
    std::vector<std::string> all;
    std::vector<std::string> reads;
    std::vector<std::string> writes;
};

thread_local const ScriptSystemAccessContext* current_script_system_access = nullptr;

ScriptComponentKind component_kind(std::string_view name) {
    if (name == "transform" || name == "Transform2D") {
        return ScriptComponentKind::Transform;
    }
    if (name == "rect" || name == "RectRenderer") {
        return ScriptComponentKind::Rect;
    }
    if (name == "line" || name == "LineRenderer") {
        return ScriptComponentKind::Line;
    }
    if (name == "script_owned" || name == "ScriptOwned") {
        return ScriptComponentKind::ScriptOwned;
    }
    return ScriptComponentKind::Unknown;
}

const ScriptComponentRegistry::ComponentEntry* script_component_entry(const ScriptScene* scene,
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
        return filter.kind == ScriptComponentKind::Unknown;
    }
    return has_component(entity, filter.kind);
}

bool contains_component_name(const std::vector<std::string>& names, std::string_view component) {
    return std::ranges::any_of(names, [&](const std::string& name) {
        return name == component;
    });
}

std::string canonical_component_name(std::string_view component) {
    switch (component_kind(component)) {
    case ScriptComponentKind::Transform: return "Transform2D";
    case ScriptComponentKind::Rect: return "RectRenderer";
    case ScriptComponentKind::Line: return "LineRenderer";
    case ScriptComponentKind::ScriptOwned: return "ScriptOwned";
    case ScriptComponentKind::Unknown: return std::string{component};
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

void write_lua_report_value(JsonWriter& json, const sol::object& value, i32 depth);

void write_lua_report_table(JsonWriter& json, const sol::table& table, i32 depth) {
    json.begin_object();
    if (depth <= 0) {
        json.end_object();
        return;
    }

    for (const auto& entry : table) {
        const sol::object key = entry.first;
        if (!key.is<std::string>()) {
            continue;
        }
        json.key(key.as<std::string>());
        write_lua_report_value(json, entry.second, depth - 1);
    }
    json.end_object();
}

void write_lua_report_value(JsonWriter& json, const sol::object& value, i32 depth) {
    switch (value.get_type()) {
    case sol::type::boolean:
        json.value(value.as<bool>());
        break;
    case sol::type::number:
        json.value(static_cast<f64>(value.as<double>()));
        break;
    case sol::type::string:
        json.value(std::string_view{value.as<std::string>()});
        break;
    case sol::type::table:
        write_lua_report_table(json, value.as<sol::table>(), depth);
        break;
    case sol::type::nil:
    default:
        json.value_null();
        break;
    }
}

struct ScriptEntityApi {
    ScriptScene* scene = nullptr;
    EcsEntity entity;

    EcsId id() const {
        return entity.id();
    }

    std::string name() const {
        return entity.name();
    }

    bool valid() const {
        return entity.valid();
    }

    bool alive() const {
        return entity.alive();
    }

    void destroy() {
        entity.destroy();
    }

    bool has(std::string_view component) const {
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

    ScriptEntityApi& add_script_owned() {
        entity.add<ScriptOwned>();
        return *this;
    }

    ScriptEntityApi& remove(std::string_view component) {
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

    sol::object get(sol::this_state state, std::string_view component) const {
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

    sol::object child(sol::this_state state, std::string_view name) const {
        flecs::entity found = entity.raw().lookup(std::string{name}.c_str());
        if (!found.is_valid() || !found.is_alive()) {
            return sol::nil;
        }
        return sol::make_object(state, ScriptEntityApi{scene, EcsEntity{found}});
    }

    ScriptEntityApi& patch(std::string_view component, sol::table values) {
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

    sol::object component(sol::this_state state, std::string_view component) const {
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
        result["commit"] = [scene = scene, entity = entity, component_name](sol::table self) -> sol::table {
            const std::vector<ComponentPatchValue> patch_values = component_patch_values_from_lua(scene->ecs(), component_name, self);
            apply_component_patch_values(scene->ecs(), entity, component_name, patch_values);
            return self;
        };
        return sol::object{result};
    }

    ScriptEntityApi& timer(std::string_view, f32 duration, sol::optional<bool> repeating) {
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

    ScriptEntityApi& cooldown(std::string_view name, f32 seconds) {
        return timer(name, seconds, false);
    }

    bool ready(std::string_view) const {
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

    ScriptEntityApi& set_transform(f32 x, f32 y) {
        entity.set(Transform2D{{x, y}});
        return *this;
    }

    sol::object get_transform(sol::this_state state) const {
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

    ScriptEntityApi& translate(f32 x, f32 y) {
        Transform2D& transform = entity.ensure<Transform2D>();
        transform.pos.x += x;
        transform.pos.y += y;
        entity.modified<Transform2D>();
        return *this;
    }

    ScriptEntityApi& set_rect(f32 w,
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

    sol::object get_rect(sol::this_state state) const {
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

    ScriptEntityApi& set_line(f32 ax,
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

    sol::object get_line(sol::this_state state) const {
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
};

struct ScriptDeferredEntityApi {
    EcsDeferredEntity entity;

    u32 index() const {
        return entity.index;
    }
};

struct ScriptCommandTarget {
    bool deferred = false;
    EcsId id = 0;
    EcsDeferredEntity deferred_entity{};
};

std::optional<ScriptCommandTarget> command_target_from_lua(const sol::object& object) {
    if (object.is<ScriptEntityApi>()) {
        ScriptEntityApi entity = object.as<ScriptEntityApi>();
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

struct ScriptCommandBufferApi {
    ScriptScene* scene = nullptr;
    EcsCommandBuffer* commands = nullptr;

    bool valid() const {
        return commands != nullptr;
    }

    i32 size() const {
        return commands ? static_cast<i32>(commands->size()) : 0;
    }

    ScriptDeferredEntityApi create(sol::optional<std::string> name) {
        require_commands();
        return ScriptDeferredEntityApi{commands->create_entity(name.value_or(""))};
    }

    ScriptCommandBufferApi& destroy(sol::object target) {
        require_commands();
        ScriptCommandTarget resolved = require_target(target);
        if (resolved.deferred) {
            commands->destroy(resolved.deferred_entity);
        } else {
            commands->destroy(resolved.id);
        }
        return *this;
    }

    ScriptCommandBufferApi& add(sol::object target, std::string_view component) {
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

    ScriptCommandBufferApi& remove(sol::object target, std::string_view component) {
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

    ScriptCommandBufferApi& patch(sol::object target, std::string_view component, sol::table values) {
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

private:
    void require_commands() const {
        if (!commands) {
            throw std::runtime_error("ECS command buffer is not available for this script callback");
        }
    }

    static ScriptCommandTarget require_target(const sol::object& target) {
        std::optional<ScriptCommandTarget> resolved = command_target_from_lua(target);
        if (!resolved) {
            throw std::runtime_error("ECS command target must be an entity, deferred entity, or entity id");
        }
        return *resolved;
    }
};

struct ScriptSystemContextApi {
    ScriptScene* scene = nullptr;
    EcsCommandBuffer* commands_ptr = nullptr;
    f32 delta_time = 0.0f;
    std::string id;
    std::string phase_name;
    bool parallel = false;

    f32 dt() const {
        return delta_time;
    }

    std::string system_id() const {
        return id;
    }

    std::string phase() const {
        return phase_name;
    }

    bool running_parallel() const {
        return parallel;
    }

    ScriptCommandBufferApi commands() const {
        return ScriptCommandBufferApi{scene, commands_ptr};
    }
};

struct ScriptSceneApi {
    ScriptScene* scene = nullptr;

    ScriptEntityApi entity(sol::optional<std::string> name) {
        EcsEntity created = name ? scene->ecs().entity(*name) : scene->ecs().entity();
        created.add<ScriptOwned>();
        return ScriptEntityApi{scene, created};
    }

    sol::object find(sol::this_state state, std::string_view name) {
        flecs::entity found = scene->ecs().raw().lookup(std::string{name}.c_str());
        if (!found.is_valid() || !found.is_alive()) {
            scene->ecs().raw().each([&](flecs::entity entity) {
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

    sol::object authored(sol::this_state state, std::string_view id) {
        EcsEntity found = scene->ecs().entities().find_by_authored_id(id);
        if (!found) {
            return sol::nil;
        }
        return sol::make_object(state, ScriptEntityApi{scene, found});
    }

    sol::table entities(sol::this_state state) {
        sol::state_view lua{state};
        sol::table result = lua.create_table();
        i32 index = 1;
        scene->ecs().raw().each([&](flecs::entity entity) {
            if (entity.is_valid() && entity.is_alive() && !entity.has<flecs::Component>()) {
                result[index++] = ScriptEntityApi{scene, EcsEntity{entity}};
            }
        });
        return result;
    }

    sol::table with(sol::this_state state, sol::variadic_args args) {
        sol::state_view lua{state};
        sol::table result = lua.create_table();
        const std::vector<ScriptComponentFilter> components = component_filters(args);

        i32 index = 1;
        collect_matching(components, [&](EcsEntity entity) {
            result[index++] = ScriptEntityApi{scene, entity};
        });
        return result;
    }

    i32 count(sol::variadic_args args) {
        const std::vector<ScriptComponentFilter> components = component_filters(args);
        i32 result = 0;
        collect_matching(components, [&](EcsEntity) {
            ++result;
        });
        return result;
    }

    void clear_script_entities() {
        scene->clear_script_entities();
    }

    void set_clear_color(i32 r, i32 g, i32 b) {
        scene->render_config().clear_color = script_color(r, g, b);
    }

    void mark_render_cache_dirty() {
        scene->mark_render_cache_dirty("script api");
    }

    bool register_system(std::string_view id, sol::table descriptor, sol::protected_function run) {
        EcsQueryPlanDescriptor query_descriptor{
            .all = table_strings(descriptor, "all"),
            .none = table_strings(descriptor, "none"),
            .reads = table_strings(descriptor, "reads"),
            .writes = table_strings(descriptor, "writes"),
        };
        EcsQueryPlan plan = scene->ecs().build_query_plan(query_descriptor);
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
        return !scene->ecs().systems().register_task(std::move(system_descriptor),
                                                     [scene = scene, plan = std::move(plan), run = std::move(run), access = std::move(access)](SystemContext& ctx) mutable {
                                                         for (EcsEntity entity : scene->ecs().query_entities(plan)) {
                                                             const ScriptSystemAccessContext* previous = current_script_system_access;
                                                             current_script_system_access = access.get();
                                                             ScriptSystemContextApi system_context{
                                                                 .scene = scene,
                                                                 .commands_ptr = ctx.commands,
                                                                 .delta_time = ctx.dt,
                                                                 .id = std::string{ctx.system_id},
                                                                 .phase_name = std::string{system_phase_name(ctx.phase)},
                                                                 .parallel = ctx.running_parallel,
                                                             };
                                                             sol::protected_function_result result =
                                                                 run(ScriptEntityApi{scene, entity}, ctx.dt, system_context);
                                                             current_script_system_access = previous;
                                                             if (!result.valid()) {
                                                                 sol::error error = result;
                                                                 throw std::runtime_error("system '" + access->system_id + "' entity=" + std::to_string(entity.id()) + ": " + error.what());
                                                             }
                                                         }
                                                     })
                    .empty();
    }

private:
    std::vector<ScriptComponentFilter> component_filters(sol::variadic_args args) const {
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
                    components.clear();
                    components.push_back({.kind = ScriptComponentKind::Unknown});
                    return components;
                }
                components.push_back({.kind = ScriptComponentKind::Unknown, .custom = entry});
            }
        }
        return components;
    }

    void collect_matching(const std::vector<ScriptComponentFilter>& components, auto&& callback) {
        if (!components.empty() && components[0].kind == ScriptComponentKind::Unknown && !components[0].custom) {
            return;
        }

        const auto visit = [&](EcsEntity entity) {
            if (entity.alive() && !entity.raw().has<flecs::Component>() && matches_components(entity, components)) {
                callback(entity);
            }
        };

        if (components.empty()) {
            scene->ecs().raw().each([&](flecs::entity raw) {
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
};

struct ScriptInputApi {
    Input* input = nullptr;

    bool pressed(std::string_view action) const {
        return input->pressed(action);
    }

    bool held(std::string_view action) const {
        return input->held(action);
    }

    bool released(std::string_view action) const {
        return input->released(action);
    }
};

struct ScriptAppApi {
    App* app = nullptr;

    void quit() {
        app->quit();
    }
};

struct ScriptContextApi {
    SceneContext* ctx = nullptr;

    f32 dt() const {
        return ctx->dt;
    }

    ScriptInputApi input() const {
        return ScriptInputApi{&ctx->input};
    }

    ScriptAppApi app() const {
        return ScriptAppApi{&ctx->app};
    }
};

struct ScriptActionsApi {
    InputActionContext* actions = nullptr;

    void add(std::string_view action, sol::optional<std::string> label) {
        actions->add(action, label.value_or(""));
    }
};

struct ScriptDialoguePlayerApi {
    DialogueDocument document;
    DialoguePlayer player;

    bool load(std::string_view path) {
        DialogueLoadResult result = load_dialogue(std::filesystem::path{path});
        if (!result.ok()) {
            return false;
        }
        document = std::move(*result.document);
        return true;
    }

    void start(sol::optional<std::string> start_node) {
        player.start(document, start_node.value_or(""));
    }

    void advance() {
        player.advance();
    }

    bool choose_index(i32 index) {
        return player.choose(index);
    }

    bool choose_id(std::string_view id) {
        return player.choose(id);
    }

    void skip_line() {
        player.skip_line();
    }

    void reset() {
        player.reset();
    }

    bool ended() const {
        return player.state().ended;
    }

    std::string current_node() const {
        return player.state().current_node;
    }

    sol::table view(sol::this_state state) const {
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

    sol::table consume_events(sol::this_state state) {
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
};

} // namespace

ScriptEngine::ScriptEngine(const std::function<void(sol::state&)>& bind)
    : _impl(std::make_unique<Impl>()) {
    _impl->lua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::table, sol::lib::string);
    sol::table package = _impl->lua.create_table();
    package["loaded"] = _impl->lua.create_table();
    package["path"] = "";
    _impl->lua["package"] = package;
    register_bindings();
    if (bind) {
        bind(_impl->lua);
    }
}

ScriptEngine::~ScriptEngine() = default;

bool ScriptEngine::load_file(const std::filesystem::path& path) {
    const std::filesystem::path absolute = normalized_dependency_path(path);
    return load_file(absolute.parent_path(), absolute);
}

bool ScriptEngine::load_file(const std::filesystem::path& asset_root, const std::filesystem::path& script_path) {
    _impl->asset_root = normalized_dependency_path(asset_root);
    _impl->dependencies.clear();
    _impl->dependency_keys.clear();
    _impl->lua["require"] = [this](sol::this_state state, const std::string& module) -> sol::object {
        sol::state_view lua{state};
        sol::table package = lua["package"];
        sol::table loaded_modules = package["loaded"];
        sol::object cached = loaded_modules[module];
        if (cached.get_type() != sol::type::nil && cached.get_type() != sol::type::boolean) {
            return cached;
        }
        if (cached.is<bool>() && cached.as<bool>()) {
            return cached;
        }

        std::optional<std::filesystem::path> module_path = resolve_lua_module(_impl->asset_root, module);
        if (!module_path) {
            throw std::runtime_error("module '" + module + "' was not found under asset root '" + _impl->asset_root.string() + "'");
        }
        record_dependency(*module_path);

        std::optional<sol::load_result> read = load_lua_file(lua, *module_path);
        if (!read) {
            throw std::runtime_error("cannot open " + module_path->string());
        }
        sol::load_result& loaded = *read;
        if (!loaded.valid()) {
            sol::error error = loaded;
            throw std::runtime_error(error.what());
        }
        sol::protected_function chunk = loaded;
        sol::protected_function_result result = chunk(module);
        if (!result.valid()) {
            sol::error error = result;
            throw std::runtime_error(error.what());
        }

        sol::object module_result = result;
        if (module_result.get_type() == sol::type::nil) {
            module_result = sol::make_object(state, true);
        }
        loaded_modules[module] = module_result;
        return module_result;
    };

    const std::filesystem::path resolved_script = script_path.is_absolute()
        ? normalized_dependency_path(script_path)
        : normalized_dependency_path(_impl->asset_root / script_path);
    if (!path_within_root(resolved_script, _impl->asset_root)) {
        _last_error = "script path is outside asset root: " + resolved_script.string();
        return false;
    }
    record_dependency(resolved_script);

    std::optional<sol::load_result> read = load_lua_file(_impl->lua, resolved_script);
    if (!read) {
        _last_error = "cannot open " + resolved_script.string();
        return false;
    }
    sol::load_result& loaded = *read;
    if (!loaded.valid()) {
        sol::error error = loaded;
        _last_error = error.what();
        return false;
    }

    sol::protected_function script = loaded;
    return record_result(_last_error, "script", script());
}

const std::vector<std::filesystem::path>& ScriptEngine::script_dependencies() const {
    return _impl->dependencies;
}

void ScriptEngine::record_dependency(const std::filesystem::path& path) {
    std::filesystem::path normalized = normalized_dependency_path(path);
    const std::string key = normalized.string();
    if (_impl->dependency_keys.insert(key).second) {
        _impl->dependencies.push_back(std::move(normalized));
    }
}

bool ScriptEngine::call_on_load(ScriptScene& scene) {
    return call_optional("on_load", ScriptSceneApi{&scene});
}

bool ScriptEngine::call_on_enter(SceneContext& ctx) {
    return call_optional("on_enter", ScriptContextApi{&ctx});
}

bool ScriptEngine::call_on_exit(SceneContext& ctx) {
    return call_optional("on_exit", ScriptContextApi{&ctx});
}

bool ScriptEngine::call_update(SceneContext& ctx) {
    return call_optional("update", ScriptContextApi{&ctx});
}

bool ScriptEngine::call_collect_actions(InputActionContext& actions) const {
    return call_optional_const("collect_actions", ScriptActionsApi{&actions});
}

bool ScriptEngine::call_write_report(JsonWriter& json) const {
    sol::object object = _impl->lua["write_report"];
    if (object.get_type() == sol::type::nil) {
        return true;
    }
    if (object.get_type() != sol::type::function) {
        _last_error = "write_report is not a function";
        return false;
    }

    sol::protected_function function = object;
    sol::protected_function_result result = function();
    if (!result.valid()) {
        return record_result(_last_error, "write_report", result);
    }

    sol::object report = result;
    if (report.get_type() == sol::type::nil) {
        _last_error.clear();
        return true;
    }
    if (report.get_type() != sol::type::table) {
        _last_error = "write_report must return a table or nil";
        return false;
    }

    sol::table table = report.as<sol::table>();
    for (const auto& entry : table) {
        const sol::object key = entry.first;
        if (!key.is<std::string>()) {
            continue;
        }
        json.key(key.as<std::string>());
        write_lua_report_value(json, entry.second, 6);
    }
    _last_error.clear();
    return true;
}

template <typename... Args>
bool ScriptEngine::call_optional(std::string_view name, Args&&... args) {
    sol::object object = _impl->lua[std::string{name}];
    if (object.get_type() == sol::type::nil) {
        return true;
    }
    if (object.get_type() != sol::type::function) {
        _last_error = std::string{name} + " is not a function";
        return false;
    }
    sol::protected_function function = object;
    return record_result(_last_error, name, function(std::forward<Args>(args)...));
}

template <typename... Args>
bool ScriptEngine::call_optional_const(std::string_view name, Args&&... args) const {
    sol::object object = _impl->lua[std::string{name}];
    if (object.get_type() == sol::type::nil) {
        return true;
    }
    if (object.get_type() != sol::type::function) {
        _last_error = std::string{name} + " is not a function";
        return false;
    }
    sol::protected_function function = object;
    return record_result(_last_error, name, function(std::forward<Args>(args)...));
}

void ScriptEngine::register_bindings() {
    scripting_detail::bind_l10n(_impl->lua, true);
    _impl->lua.new_usertype<ScriptEntityApi>("KinEntity",
        sol::no_constructor,
        "id", &ScriptEntityApi::id,
        "name", &ScriptEntityApi::name,
        "valid", &ScriptEntityApi::valid,
        "alive", &ScriptEntityApi::alive,
        "destroy", &ScriptEntityApi::destroy,
        "has", &ScriptEntityApi::has,
        "get", &ScriptEntityApi::get,
        "component", &ScriptEntityApi::component,
        "timer", &ScriptEntityApi::timer,
        "cooldown", &ScriptEntityApi::cooldown,
        "ready", &ScriptEntityApi::ready,
        "child", &ScriptEntityApi::child,
        "patch", &ScriptEntityApi::patch,
        "add_script_owned", &ScriptEntityApi::add_script_owned,
        "remove", &ScriptEntityApi::remove,
        "set_transform", &ScriptEntityApi::set_transform,
        "get_transform", &ScriptEntityApi::get_transform,
        "translate", &ScriptEntityApi::translate,
        "set_rect", &ScriptEntityApi::set_rect,
        "get_rect", &ScriptEntityApi::get_rect,
        "set_line", &ScriptEntityApi::set_line,
        "get_line", &ScriptEntityApi::get_line);

    _impl->lua.new_usertype<ScriptDeferredEntityApi>("KinDeferredEntity",
        sol::no_constructor,
        "index", sol::property(&ScriptDeferredEntityApi::index));

    _impl->lua.new_usertype<ScriptCommandBufferApi>("KinCommandBuffer",
        sol::no_constructor,
        "valid", sol::property(&ScriptCommandBufferApi::valid),
        "size", sol::property(&ScriptCommandBufferApi::size),
        "create", &ScriptCommandBufferApi::create,
        "destroy", &ScriptCommandBufferApi::destroy,
        "add", &ScriptCommandBufferApi::add,
        "remove", &ScriptCommandBufferApi::remove,
        "patch", &ScriptCommandBufferApi::patch);

    _impl->lua.new_usertype<ScriptSystemContextApi>("KinSystemContext",
        sol::no_constructor,
        "dt", sol::property(&ScriptSystemContextApi::dt),
        "system_id", sol::property(&ScriptSystemContextApi::system_id),
        "phase", sol::property(&ScriptSystemContextApi::phase),
        "running_parallel", sol::property(&ScriptSystemContextApi::running_parallel),
        "commands", sol::property(&ScriptSystemContextApi::commands));

    _impl->lua.new_usertype<ScriptSceneApi>("KinScene",
        sol::no_constructor,
        "entity", &ScriptSceneApi::entity,
        "find", &ScriptSceneApi::find,
        "authored", &ScriptSceneApi::authored,
        "entities", &ScriptSceneApi::entities,
        "with", &ScriptSceneApi::with,
        "count", &ScriptSceneApi::count,
        "register_system", &ScriptSceneApi::register_system,
        "clear_script_entities", &ScriptSceneApi::clear_script_entities,
        "set_clear_color", &ScriptSceneApi::set_clear_color,
        "mark_render_cache_dirty", &ScriptSceneApi::mark_render_cache_dirty);

    _impl->lua.new_usertype<ScriptInputApi>("KinInput",
        sol::no_constructor,
        "pressed", &ScriptInputApi::pressed,
        "held", &ScriptInputApi::held,
        "released", &ScriptInputApi::released);

    _impl->lua.new_usertype<ScriptAppApi>("KinApp",
        sol::no_constructor,
        "quit", &ScriptAppApi::quit);

    _impl->lua.new_usertype<ScriptContextApi>("KinContext",
        sol::no_constructor,
        "dt", sol::property(&ScriptContextApi::dt),
        "input", sol::property(&ScriptContextApi::input),
        "app", sol::property(&ScriptContextApi::app));

    _impl->lua.new_usertype<ScriptActionsApi>("KinActions",
        sol::no_constructor,
        "add", &ScriptActionsApi::add);

    _impl->lua.new_usertype<ScriptDialoguePlayerApi>("KinDialoguePlayer",
        sol::constructors<ScriptDialoguePlayerApi()>(),
        "load", &ScriptDialoguePlayerApi::load,
        "start", &ScriptDialoguePlayerApi::start,
        "advance", &ScriptDialoguePlayerApi::advance,
        "choose_index", &ScriptDialoguePlayerApi::choose_index,
        "choose_id", &ScriptDialoguePlayerApi::choose_id,
        "skip_line", &ScriptDialoguePlayerApi::skip_line,
        "reset", &ScriptDialoguePlayerApi::reset,
        "ended", sol::property(&ScriptDialoguePlayerApi::ended),
        "current_node", sol::property(&ScriptDialoguePlayerApi::current_node),
        "view", &ScriptDialoguePlayerApi::view,
        "consume_events", &ScriptDialoguePlayerApi::consume_events);
}

} // namespace kin
