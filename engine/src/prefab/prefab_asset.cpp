#include <kin/prefab/prefab_asset.hpp>

#include <kin/core/json.hpp>
#include <kin/core/json_value.hpp>
#include <kin/ecs/render.hpp>

#include <algorithm>
#include <fstream>
#include <ostream>
#include <sstream>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace kin {
namespace {

bool apply_component_fields(EcsEntity entity,
                            const PrefabComponentAsset& component,
                            EcsComponentRegistry& registry,
                            std::vector<std::string>& diagnostics);

struct PrefabBackendMetadata {
    std::string prefab_id;
    std::string prefab_entity_id;
    u64 generation = 0;
};

struct CompiledPrefabEntity {
    std::string id;
    EcsEntity prefab_entity;
};

struct CompiledPrefab {
    u64 generation = 0;
    EcsEntity root;
    std::vector<CompiledPrefabEntity> entities;
    std::unordered_map<std::string, EcsEntity> by_id;
};

class PrefabCompiler {
public:
    const CompiledPrefab* compile(EcsWorld& world,
                                  const PrefabAsset& prefab,
                                  EcsComponentRegistry& components,
                                  std::vector<std::string>& diagnostics) {
        std::ostringstream serialized;
        write_prefab_asset(serialized, prefab);
        const std::string signature = serialized.str();
        if (const auto found = _compiled.find(prefab.id);
            found != _compiled.end() && found->second.signature == signature) {
            return &found->second.prefab;
        }

        CompiledPrefab compiled{.generation = _next_generation++};

        for (const PrefabEntityAsset& prefab_entity : prefab.entities) {
            const std::string name = "kin::prefab::" + prefab.id + "#" +
                                     std::to_string(compiled.generation) + "::" + prefab_entity.id;
            EcsEntity entity{world.raw().prefab(name.c_str())};
            entity.set(PrefabBackendMetadata{
                .prefab_id = prefab.id,
                .prefab_entity_id = prefab_entity.id,
                .generation = compiled.generation,
            });
            compiled.entities.push_back({.id = prefab_entity.id, .prefab_entity = entity});
            compiled.by_id[prefab_entity.id] = entity;
            if (prefab_entity.id == prefab.root) {
                compiled.root = entity;
            }
        }

        for (const PrefabEntityAsset& prefab_entity : prefab.entities) {
            EcsEntity entity = compiled.by_id.at(prefab_entity.id);
            for (const PrefabComponentAsset& component : prefab_entity.components) {
                if (!apply_component_fields(entity, component, components, diagnostics)) {
                    destroy_template(compiled);
                    return nullptr;
                }
            }
        }
        if (auto found = _compiled.find(prefab.id); found != _compiled.end()) {
            destroy_template(found->second.prefab);
            _compiled.erase(found);
        }
        auto [stored, inserted] = _compiled.emplace(
            prefab.id, CachedPrefab{.signature = signature, .prefab = std::move(compiled)});
        (void)inserted;
        return &stored->second.prefab;
    }

private:
    static void destroy_template(const CompiledPrefab& compiled) {
        for (auto it = compiled.entities.rbegin(); it != compiled.entities.rend(); ++it) {
            if (it->prefab_entity) {
                EcsEntity entity = it->prefab_entity;
                entity.destroy();
            }
        }
    }

    struct CachedPrefab {
        std::string signature;
        CompiledPrefab prefab;
    };

    u64 _next_generation = 1;
    std::unordered_map<std::string, CachedPrefab> _compiled;
};

PrefabCompiler& prefab_compiler(EcsWorld& world) {
    flecs::entity state = world.raw().entity("kin::prefab::compiler_state");
    return state.ensure<PrefabCompiler>();
}

bool nonzero(Vec2f value) {
    return value.x != 0.0f || value.y != 0.0f;
}

const ComponentFieldDescriptor* find_field(const ComponentDescriptor& descriptor, std::string_view name) {
    const auto found = std::ranges::find_if(descriptor.fields, [&](const ComponentFieldDescriptor& field) {
        return field.name == name;
    });
    return found == descriptor.fields.end() ? nullptr : &*found;
}

const PrefabEntityAsset* find_entity(const PrefabAsset& prefab, std::string_view id) {
    const auto found = std::ranges::find_if(prefab.entities, [&](const PrefabEntityAsset& entity) {
        return entity.id == id;
    });
    return found == prefab.entities.end() ? nullptr : &*found;
}

const PrefabComponentAsset* find_component(const std::vector<PrefabComponentAsset>& components, std::string_view name) {
    const auto found = std::ranges::find_if(components, [&](const PrefabComponentAsset& component) {
        return component.name == name;
    });
    return found == components.end() ? nullptr : &*found;
}

ComponentFieldKind field_kind_from_value(const ComponentFieldValue& value) {
    return std::visit([](const auto& typed) {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<T, bool>) {
            return ComponentFieldKind::Bool;
        } else if constexpr (std::is_same_v<T, i32>) {
            return ComponentFieldKind::I32;
        } else if constexpr (std::is_same_v<T, i64>) {
            return ComponentFieldKind::I64;
        } else if constexpr (std::is_same_v<T, u32>) {
            return ComponentFieldKind::U32;
        } else if constexpr (std::is_same_v<T, u64>) {
            return ComponentFieldKind::U64;
        } else if constexpr (std::is_same_v<T, f32>) {
            return ComponentFieldKind::F32;
        } else if constexpr (std::is_same_v<T, f64>) {
            return ComponentFieldKind::F64;
        } else if constexpr (std::is_same_v<T, std::string>) {
            return ComponentFieldKind::String;
        } else if constexpr (std::is_same_v<T, Vec2f>) {
            return ComponentFieldKind::Vec2f;
        } else if constexpr (std::is_same_v<T, Vec2i>) {
            return ComponentFieldKind::Vec2i;
        } else if constexpr (std::is_same_v<T, Color>) {
            return ComponentFieldKind::Color;
        } else if constexpr (std::is_same_v<T, ComponentEntityRef>) {
            return ComponentFieldKind::EntityRef;
        } else {
            return ComponentFieldKind::AssetRef;
        }
    }, value);
}

std::string runtime_authored_id(const PrefabSpawn& spawn, std::string_view prefab_entity_id) {
    if (spawn.authored_id_prefix.empty()) {
        return {};
    }
    return spawn.authored_id_prefix + "/" + std::string{prefab_entity_id};
}

PrefabComponentAsset parse_component(std::string_view component_name,
                                     const JsonValue& json,
                                     const EcsComponentRegistry& registry,
                                     std::vector<std::string>& diagnostics) {
    PrefabComponentAsset component{.name = std::string{component_name}};
    for (ComponentFieldSnapshot field : parse_component_fields(component_name, json, registry, diagnostics)) {
        component.fields.push_back({
            .name = std::move(field.name),
            .value = std::move(field.value),
        });
    }
    return component;
}

PrefabAsset parse_prefab_object(const JsonValue& root,
                                const EcsComponentRegistry& components,
                                std::vector<std::string>& diagnostics) {
    PrefabAsset prefab;
    prefab.schema = root.string_at("schema", "kin.prefab/1");
    prefab.id = root.string_at("id");
    prefab.name = root.string_at("name");
    prefab.root = root.string_at("root");

    const JsonValue* entities = root.find("entities");
    if (!entities || !entities->is_array()) {
        diagnostics.push_back("prefab requires an entities array");
        return prefab;
    }

    for (const JsonValue& entity_json : entities->items()) {
        if (!entity_json.is_object()) {
            diagnostics.push_back("prefab entity must be an object");
            continue;
        }

        PrefabEntityAsset entity;
        entity.id = entity_json.string_at("id");
        entity.name = entity_json.string_at("name");
        entity.parent = entity_json.string_at("parent");

        if (const JsonValue* component_json = entity_json.find("components")) {
            if (!component_json->is_object()) {
                diagnostics.push_back("entity '" + entity.id + "' components must be an object");
            } else {
                for (const auto& [component_name, component_value] : component_json->members()) {
                    entity.components.push_back(parse_component(component_name, component_value, components, diagnostics));
                }
            }
        }
        prefab.entities.push_back(std::move(entity));
    }
    return prefab;
}

void write_component(JsonWriter& json, const PrefabComponentAsset& component) {
    json.key(component.name).begin_object();
    for (const PrefabFieldAsset& field : component.fields) {
        json.key(field.name);
        write_component_field_value_json(json, field.value);
    }
    json.end_object();
}

bool has_parent_cycle(const PrefabAsset& prefab, const PrefabEntityAsset& entity) {
    std::unordered_set<std::string> seen;
    std::string current = entity.parent;
    while (!current.empty()) {
        if (!seen.insert(current).second) {
            return true;
        }
        const PrefabEntityAsset* parent = find_entity(prefab, current);
        if (!parent) {
            return false;
        }
        current = parent->parent;
    }
    return false;
}

std::vector<std::string> validate_components(std::string_view entity_id,
                                             const std::vector<PrefabComponentAsset>& prefab_components,
                                             const EcsComponentRegistry& registry) {
    std::vector<std::string> diagnostics;
    std::unordered_set<std::string> component_names;
    for (const PrefabComponentAsset& component : prefab_components) {
        if (component.name.empty()) {
            diagnostics.push_back("entity '" + std::string{entity_id} + "' has an empty component name");
            continue;
        }
        if (!component_names.insert(component.name).second) {
            diagnostics.push_back("entity '" + std::string{entity_id} + "' has duplicate component '" + component.name + "'");
        }
        const ComponentDescriptor* descriptor = registry.find(component.name);
        if (!descriptor) {
            diagnostics.push_back("entity '" + std::string{entity_id} + "' references unknown component '" + component.name + "'");
            continue;
        }
        std::unordered_set<std::string> field_names;
        for (const PrefabFieldAsset& field : component.fields) {
            if (!field_names.insert(field.name).second) {
                diagnostics.push_back("entity '" + std::string{entity_id} + "' component '" + component.name + "' has duplicate field '" + field.name + "'");
            }
            const ComponentFieldDescriptor* descriptor_field = find_field(*descriptor, field.name);
            if (!descriptor_field) {
                diagnostics.push_back("entity '" + std::string{entity_id} + "' component '" + component.name + "' references unknown field '" + field.name + "'");
                continue;
            }
            if (!detail::field_value_matches_kind(field.value, descriptor_field->kind)) {
                diagnostics.push_back("entity '" + std::string{entity_id} + "' component '" + component.name + "' field '" + field.name + "' has a type mismatch");
            }
        }
    }
    return diagnostics;
}

bool apply_component_fields(EcsEntity entity,
                            const PrefabComponentAsset& component,
                            EcsComponentRegistry& registry,
                            std::vector<std::string>& diagnostics) {
    if (!registry.has(entity, component.name) && !registry.add(entity, component.name)) {
        diagnostics.push_back(registry.last_error());
        return false;
    }
    for (const PrefabFieldAsset& field : component.fields) {
        if (!registry.patch_field(entity, component.name, field.name, field.value)) {
            diagnostics.push_back(registry.last_error());
            return false;
        }
    }
    return true;
}

ComponentFieldValue resolve_prefab_field_value(const ComponentFieldValue& value,
                                               const std::unordered_map<std::string, EcsEntity>& prefab_entities) {
    if (const ComponentEntityRef* ref = std::get_if<ComponentEntityRef>(&value)) {
        ComponentEntityRef resolved = *ref;
        if (!resolved.authored_id.empty()) {
            if (const auto found = prefab_entities.find(resolved.authored_id); found != prefab_entities.end()) {
                resolved.id = found->second.id();
            }
        }
        return resolved;
    }
    return value;
}

PrefabComponentAsset resolve_prefab_component_refs(const PrefabComponentAsset& component,
                                                   const std::unordered_map<std::string, EcsEntity>& prefab_entities) {
    PrefabComponentAsset resolved{
        .name = component.name,
    };
    resolved.fields.reserve(component.fields.size());
    for (const PrefabFieldAsset& field : component.fields) {
        resolved.fields.push_back({
            .name = field.name,
            .value = resolve_prefab_field_value(field.value, prefab_entities),
        });
    }
    return resolved;
}

void apply_spawn_transform(EcsEntity root,
                           Vec2f pos,
                           EcsComponentRegistry& registry,
                           std::vector<std::string>& diagnostics) {
    if (!nonzero(pos)) {
        return;
    }
    const ComponentDescriptor* transform = registry.find("Transform2D");
    if (!transform) {
        return;
    }
    if (!registry.has(root, transform->id)) {
        if (!registry.add(root, transform->id)) {
            diagnostics.push_back(registry.last_error());
            return;
        }
    }

    const ComponentFieldDescriptor* pos_field = find_field(*transform, "pos");
    if (!pos_field) {
        pos_field = find_field(*transform, "position");
    }
    if (!pos_field || pos_field->kind != ComponentFieldKind::Vec2f) {
        return;
    }
    if (!registry.patch_field(root, transform->id, pos_field->name, pos)) {
        diagnostics.push_back(registry.last_error());
    }
}

void rollback_prefab_instance(EcsWorld& world, PrefabInstance& instance) {
    std::vector<EcsEntity> roots;
    std::unordered_set<EcsId> instance_ids;
    for (EcsEntity entity : instance.entities) {
        if (entity) {
            instance_ids.insert(entity.id());
        }
    }
    for (EcsEntity entity : instance.entities) {
        if (!entity) {
            continue;
        }
        EcsEntity parent = entity.parent();
        if (!parent || !instance_ids.contains(parent.id())) {
            roots.push_back(entity);
        }
    }
    if (roots.empty()) {
        roots = instance.entities;
    }
    std::ranges::sort(roots, [](EcsEntity a, EcsEntity b) {
        return a.id() > b.id();
    });
    for (EcsEntity entity : roots) {
        if (!entity) {
            continue;
        }
        if (world.entities().registered(entity)) {
            world.entities().destroy(entity);
        } else {
            entity.destroy();
        }
    }
    instance.root = {};
    instance.entities.clear();
    instance.by_authored_id.clear();
}

bool set_runtime_name(EcsWorld& world, EcsEntity entity, std::string_view name) {
    if (name.empty()) {
        return true;
    }
    std::string runtime_name{name};
    if (world.entities().find_by_name(runtime_name)) {
        runtime_name += "." + std::to_string(entity.id());
    }
    ecs_set_name(world.raw().c_ptr(), static_cast<ecs_entity_t>(entity.id()), runtime_name.c_str());
    return true;
}

} // namespace

PrefabLoadResult load_prefab_asset(const std::filesystem::path& path,
                                   const EcsComponentRegistry& components) {
    std::ifstream in(path);
    if (!in) {
        return {.diagnostics = {"failed to open prefab '" + path.string() + "'"}};
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return parse_prefab_asset(buffer.str(), components, path.string());
}

PrefabLoadResult parse_prefab_asset(std::string_view text,
                                    const EcsComponentRegistry& components,
                                    std::string_view source) {
    JsonParseResult parsed = parse_json(text);
    if (!parsed.ok()) {
        std::string prefix = source.empty() ? std::string{} : std::string{source} + ": ";
        return {.diagnostics = {prefix + parsed.error}};
    }
    if (!parsed.value->is_object()) {
        return {.diagnostics = {"prefab root must be an object"}};
    }

    std::vector<std::string> diagnostics;
    PrefabAsset prefab = parse_prefab_object(*parsed.value, components, diagnostics);
    std::vector<std::string> validation = validate_prefab_asset(prefab, components);
    diagnostics.insert(diagnostics.end(), validation.begin(), validation.end());
    if (!diagnostics.empty()) {
        return {.diagnostics = std::move(diagnostics)};
    }
    return {.prefab = std::move(prefab)};
}

bool save_prefab_asset(const PrefabAsset& prefab, const std::filesystem::path& path) {
    std::ofstream out(path);
    if (!out) {
        return false;
    }
    write_prefab_asset(out, prefab);
    return static_cast<bool>(out);
}

void write_prefab_asset(std::ostream& out, const PrefabAsset& prefab) {
    JsonWriter json(out);
    json.begin_object();
    json.field("schema", std::string_view{prefab.schema});
    json.field("id", std::string_view{prefab.id});
    if (!prefab.name.empty()) {
        json.field("name", std::string_view{prefab.name});
    }
    json.field("root", std::string_view{prefab.root});
    json.key("entities").begin_array();
    for (const PrefabEntityAsset& entity : prefab.entities) {
        json.begin_object();
        json.field("id", std::string_view{entity.id});
        if (!entity.name.empty()) {
            json.field("name", std::string_view{entity.name});
        }
        if (!entity.parent.empty()) {
            json.field("parent", std::string_view{entity.parent});
        }
        json.key("components").begin_object();
        for (const PrefabComponentAsset& component : entity.components) {
            write_component(json, component);
        }
        json.end_object();
        json.end_object();
    }
    json.end_array();
    json.end_object();
    out << '\n';
}

std::vector<std::string> validate_prefab_asset(const PrefabAsset& prefab,
                                               const EcsComponentRegistry& components) {
    std::vector<std::string> diagnostics;
    if (prefab.schema != "kin.prefab/1") {
        diagnostics.push_back("unsupported prefab schema '" + prefab.schema + "'");
    }
    if (prefab.id.empty()) {
        diagnostics.push_back("prefab id is required");
    }
    if (prefab.root.empty()) {
        diagnostics.push_back("prefab root is required");
    }
    if (prefab.entities.empty()) {
        diagnostics.push_back("prefab requires at least one entity");
    }

    std::unordered_set<std::string> ids;
    for (const PrefabEntityAsset& entity : prefab.entities) {
        if (entity.id.empty()) {
            diagnostics.push_back("prefab entity id is required");
        } else if (!ids.insert(entity.id).second) {
            diagnostics.push_back("duplicate prefab entity id '" + entity.id + "'");
        }
    }
    if (!prefab.root.empty() && !ids.contains(prefab.root)) {
        diagnostics.push_back("prefab root '" + prefab.root + "' does not exist");
    }

    for (const PrefabEntityAsset& entity : prefab.entities) {
        if (!entity.parent.empty() && !ids.contains(entity.parent)) {
            diagnostics.push_back("entity '" + entity.id + "' parent '" + entity.parent + "' does not exist");
        }
        if (entity.id == prefab.root && !entity.parent.empty()) {
            diagnostics.push_back("prefab root '" + prefab.root + "' cannot have a parent");
        }
        if (has_parent_cycle(prefab, entity)) {
            diagnostics.push_back("entity '" + entity.id + "' participates in a parent cycle");
        }
        std::vector<std::string> component_diagnostics = validate_components(entity.id, entity.components, components);
        diagnostics.insert(diagnostics.end(), component_diagnostics.begin(), component_diagnostics.end());
    }
    return diagnostics;
}

std::vector<std::string> validate_prefab_overrides(const PrefabAsset& prefab,
                                                   const PrefabOverrideSet& overrides,
                                                   const EcsComponentRegistry& components) {
    std::vector<std::string> diagnostics;
    for (const auto& [entity_id, component_overrides] : overrides.entities) {
        if (!find_entity(prefab, entity_id)) {
            diagnostics.push_back("override references unknown entity '" + entity_id + "'");
            continue;
        }
        std::vector<std::string> component_diagnostics = validate_components(entity_id, component_overrides, components);
        diagnostics.insert(diagnostics.end(), component_diagnostics.begin(), component_diagnostics.end());
    }
    return diagnostics;
}

PrefabBuildResult prefab_asset_from_scene_document(const SceneDocument& document,
                                                   const PrefabFromSceneOptions& options,
                                                   const EcsComponentRegistry& components) {
    PrefabAsset prefab;
    prefab.id = options.prefab_id;
    prefab.name = options.name;
    prefab.root = options.root;

    if (prefab.root.empty()) {
        std::vector<std::string> roots;
        for (const SceneEntityDocument& entity : document.entities) {
            if (entity.parent.empty()) {
                roots.push_back(entity.id);
            }
        }
        if (roots.size() == 1) {
            prefab.root = roots.front();
        } else {
            return {.diagnostics = {"prefab root is required when scene document has " + std::to_string(roots.size()) + " roots"}};
        }
    }

    prefab.entities.reserve(document.entities.size());
    for (const SceneEntityDocument& scene_entity : document.entities) {
        PrefabEntityAsset prefab_entity{
            .id = scene_entity.id,
            .name = scene_entity.name,
            .parent = scene_entity.parent,
        };
        prefab_entity.components.reserve(scene_entity.components.size());
        for (const SceneComponentDocument& scene_component : scene_entity.components) {
            PrefabComponentAsset prefab_component{
                .name = scene_component.name,
            };
            prefab_component.fields.reserve(scene_component.fields.size());
            for (const ComponentFieldSnapshot& scene_field : scene_component.fields) {
                prefab_component.fields.push_back({
                    .name = scene_field.name,
                    .value = scene_field.value,
                });
            }
            prefab_entity.components.push_back(std::move(prefab_component));
        }
        prefab.entities.push_back(std::move(prefab_entity));
    }

    std::vector<std::string> diagnostics = validate_prefab_asset(prefab, components);
    if (!diagnostics.empty()) {
        return {.diagnostics = std::move(diagnostics)};
    }
    return {.prefab = std::move(prefab)};
}

SceneDocument scene_document_from_prefab_asset(const PrefabAsset& prefab) {
    SceneDocument document;
    document.entities.reserve(prefab.entities.size());
    for (const PrefabEntityAsset& prefab_entity : prefab.entities) {
        SceneEntityDocument scene_entity{
            .id = prefab_entity.id,
            .name = prefab_entity.name,
            .parent = prefab_entity.parent,
        };
        scene_entity.components.reserve(prefab_entity.components.size());
        for (const PrefabComponentAsset& prefab_component : prefab_entity.components) {
            SceneComponentDocument scene_component{
                .name = prefab_component.name,
            };
            scene_component.fields.reserve(prefab_component.fields.size());
            for (const PrefabFieldAsset& prefab_field : prefab_component.fields) {
                scene_component.fields.push_back({
                    .name = prefab_field.name,
                    .kind = field_kind_from_value(prefab_field.value),
                    .value = prefab_field.value,
                });
            }
            scene_entity.components.push_back(std::move(scene_component));
        }
        document.entities.push_back(std::move(scene_entity));
    }
    return document;
}

PrefabInstance instantiate_prefab(EcsWorld& world,
                                  const PrefabAsset& prefab,
                                  const PrefabSpawn& spawn,
                                  EcsComponentRegistry& components,
                                  const PrefabOverrideSet& overrides) {
    PrefabInstance instance;
    instance.diagnostics = validate_prefab_asset(prefab, components);
    std::vector<std::string> override_diagnostics = validate_prefab_overrides(prefab, overrides, components);
    instance.diagnostics.insert(instance.diagnostics.end(), override_diagnostics.begin(), override_diagnostics.end());
    if (!spawn.authored_id_prefix.empty()) {
        for (const PrefabEntityAsset& prefab_entity : prefab.entities) {
            const std::string authored_id = runtime_authored_id(spawn, prefab_entity.id);
            if (world.entities().find_by_authored_id(authored_id)) {
                instance.diagnostics.push_back("duplicate authored entity id '" + authored_id + "'");
            }
        }
    }
    if (!instance.diagnostics.empty()) {
        return instance;
    }

    const CompiledPrefab* compiled = prefab_compiler(world).compile(world, prefab, components, instance.diagnostics);
    if (!compiled) {
        if (instance.diagnostics.empty()) {
            instance.diagnostics.push_back("failed to compile prefab backend for '" + prefab.id + "'");
        }
        return instance;
    }

    instance.entities.reserve(prefab.entities.size());
    for (const PrefabEntityAsset& prefab_entity : prefab.entities) {
        std::string runtime_name = prefab_entity.name.empty() ? prefab_entity.id : prefab_entity.name;
        if (prefab_entity.id == prefab.root && !spawn.name.empty()) {
            runtime_name = spawn.name;
        }
        EcsEntity entity{world.raw().entity()};
        set_runtime_name(world, entity, runtime_name);
        entity.raw().is_a(compiled->by_id.at(prefab_entity.id).raw());
        instance.entities.push_back(entity);
        instance.by_authored_id[prefab_entity.id] = entity;
        if (prefab_entity.id == prefab.root) {
            instance.root = entity;
        }
    }

    for (const PrefabEntityAsset& prefab_entity : prefab.entities) {
        EcsEntity entity = instance.by_authored_id.at(prefab_entity.id);
        const std::string authored_id = runtime_authored_id(spawn, prefab_entity.id);
        if (!world.entities().register_existing(entity, authored_id)) {
            instance.diagnostics.push_back(world.entities().last_error());
            rollback_prefab_instance(world, instance);
            return instance;
        }
        world.events().emit_entity(EcsEventKind::EntityCreated, entity);
    }

    for (const PrefabEntityAsset& prefab_entity : prefab.entities) {
        EcsEntity entity = instance.by_authored_id.at(prefab_entity.id);
        if (!prefab_entity.parent.empty()) {
            if (!world.entities().reparent(entity, instance.by_authored_id.at(prefab_entity.parent))) {
                instance.diagnostics.push_back(world.entities().last_error());
                rollback_prefab_instance(world, instance);
                return instance;
            }
        }
    }
    if (spawn.parent && instance.root) {
        if (!world.entities().reparent(instance.root, spawn.parent)) {
            instance.diagnostics.push_back(world.entities().last_error());
            rollback_prefab_instance(world, instance);
            return instance;
        }
    }

    for (const PrefabEntityAsset& prefab_entity : prefab.entities) {
        EcsEntity entity = instance.by_authored_id.at(prefab_entity.id);
        for (const PrefabComponentAsset& component : prefab_entity.components) {
            if (!apply_component_fields(entity, resolve_prefab_component_refs(component, instance.by_authored_id), components, instance.diagnostics)) {
                rollback_prefab_instance(world, instance);
                return instance;
            }
        }
    }

    for (const auto& [entity_id, component_overrides] : overrides.entities) {
        EcsEntity entity = instance.by_authored_id.at(entity_id);
        for (const PrefabComponentAsset& component : component_overrides) {
            apply_component_fields(entity, resolve_prefab_component_refs(component, instance.by_authored_id), components, instance.diagnostics);
            if (!instance.diagnostics.empty()) {
                rollback_prefab_instance(world, instance);
                return instance;
            }
        }
    }

    apply_spawn_transform(instance.root, spawn.pos, components, instance.diagnostics);
    if (!instance.diagnostics.empty()) {
        rollback_prefab_instance(world, instance);
        return instance;
    }
    return instance;
}

PrefabInstance PrefabInstanceRegistry::instantiate(EcsWorld& world,
                                                  const PrefabAsset& prefab,
                                                  const PrefabSpawn& spawn,
                                                  EcsComponentRegistry& components,
                                                  const PrefabOverrideSet& overrides) {
    PrefabInstance instance = instantiate_prefab(world, prefab, spawn, components, overrides);
    if (!instance.ok()) {
        _last_error = instance.diagnostics.empty() ? "failed to instantiate prefab" : instance.diagnostics.front();
        return instance;
    }

    instance.instance_id = next_instance_id(prefab);
    instance.source_prefab_id = prefab.id;
    _records[instance.instance_id] = PrefabInstanceRecord{
        .id = instance.instance_id,
        .prefab_id = prefab.id,
        .root = instance.root,
        .entities = instance.entities,
        .by_authored_id = instance.by_authored_id,
        .overrides = overrides,
    };
    _last_error.clear();
    return instance;
}

const PrefabInstanceRecord* PrefabInstanceRegistry::find(std::string_view id) const {
    const auto found = _records.find(std::string{id});
    return found == _records.end() ? nullptr : &found->second;
}

std::vector<PrefabInstanceRecord> PrefabInstanceRegistry::records() const {
    std::vector<PrefabInstanceRecord> result;
    result.reserve(_records.size());
    for (const auto& [id, record] : _records) {
        result.push_back(record);
    }
    std::ranges::sort(result, [](const PrefabInstanceRecord& a, const PrefabInstanceRecord& b) {
        return a.id < b.id;
    });
    return result;
}

bool PrefabInstanceRegistry::remove(std::string_view id) {
    if (_records.erase(std::string{id}) == 0) {
        _last_error = "unknown prefab instance '" + std::string{id} + "'";
        return false;
    }
    _last_error.clear();
    return true;
}

void PrefabInstanceRegistry::clear() {
    _records.clear();
    _last_error.clear();
}

std::string PrefabInstanceRegistry::next_instance_id(const PrefabAsset& prefab) {
    const std::string base = prefab.id.empty() ? "prefab" : prefab.id;
    return base + "#" + std::to_string(_next_instance_sequence++);
}

} // namespace kin
