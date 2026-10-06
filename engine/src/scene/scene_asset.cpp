#include <kin/scene/scene_asset.hpp>

#include <kin/assets/content.hpp>
#include <kin/core/json.hpp>
#include <kin/core/json_value.hpp>
#include <kin/ecs/render.hpp>

#include <algorithm>
#include <fstream>
#include <ostream>
#include <unordered_set>

namespace kin {
namespace {

const ComponentFieldDescriptor* find_field(const ComponentDescriptor& descriptor, std::string_view name) {
    const auto found = std::ranges::find_if(descriptor.fields, [&](const ComponentFieldDescriptor& field) {
        return field.name == name;
    });
    return found == descriptor.fields.end() ? nullptr : &*found;
}

const SceneEntityDocument* find_scene_entity(const SceneAsset& scene, std::string_view id) {
    const auto found = std::ranges::find_if(scene.entities, [&](const SceneEntityDocument& entity) {
        return entity.id == id;
    });
    return found == scene.entities.end() ? nullptr : &*found;
}

const ScenePrefabInstanceAsset* find_scene_prefab(const SceneAsset& scene, std::string_view id) {
    const auto found = std::ranges::find_if(scene.prefabs, [&](const ScenePrefabInstanceAsset& prefab) {
        return prefab.id == id;
    });
    return found == scene.prefabs.end() ? nullptr : &*found;
}

Color color_from_json(const JsonValue* value, Color fallback) {
    if (!value || !value->is_array() || value->items().size() < 3) {
        return fallback;
    }
    const auto clamp = [](i64 raw) {
        return static_cast<u8>(std::clamp<i64>(raw, 0, 255));
    };
    const auto& items = value->items();
    return Color::rgb(clamp(items[0].as_int()), clamp(items[1].as_int()), clamp(items[2].as_int()));
}

Vec2f vec2f_from_json(const JsonValue* value, Vec2f fallback = {}) {
    if (!value) {
        return fallback;
    }
    if (value->is_object()) {
        return {
            static_cast<f32>(value->number_at("x", fallback.x)),
            static_cast<f32>(value->number_at("y", fallback.y)),
        };
    }
    if (value->is_array() && value->items().size() >= 2) {
        return {
            static_cast<f32>(value->items()[0].as_number(fallback.x)),
            static_cast<f32>(value->items()[1].as_number(fallback.y)),
        };
    }
    return fallback;
}

SceneComponentDocument parse_component(std::string_view component_name,
                                       const JsonValue& json,
                                       const EcsComponentRegistry& components,
                                       std::vector<std::string>& diagnostics) {
    return SceneComponentDocument{
        .name = std::string{component_name},
        .fields = parse_component_fields(component_name, json, components, diagnostics),
    };
}

std::vector<PrefabComponentAsset> parse_prefab_component_map(const JsonValue& json,
                                                             const EcsComponentRegistry& components,
                                                             std::vector<std::string>& diagnostics) {
    std::vector<PrefabComponentAsset> result;
    if (!json.is_object()) {
        diagnostics.push_back("prefab override components must be an object");
        return result;
    }
    for (const auto& [component_name, component_json] : json.members()) {
        PrefabComponentAsset prefab_component{.name = std::string{component_name}};
        for (ComponentFieldSnapshot field : parse_component_fields(component_name, component_json, components, diagnostics)) {
            prefab_component.fields.push_back({
                .name = std::move(field.name),
                .value = std::move(field.value),
            });
        }
        result.push_back(std::move(prefab_component));
    }
    return result;
}

SceneAsset parse_scene_object(const JsonValue& root,
                              const EcsComponentRegistry& components,
                              std::vector<std::string>& diagnostics) {
    SceneAsset scene;
    scene.schema = root.string_at("schema", "kin.scene/1");
    scene.name = root.string_at("name", "Scene");
    scene.script = root.string_at("script");
    scene.clear_color = color_from_json(root.find("clear_color"), scene.clear_color);

    if (const JsonValue* entities = root.find("entities")) {
        if (!entities->is_array()) {
            diagnostics.push_back("scene entities must be an array");
        } else {
            for (const JsonValue& entity_json : entities->items()) {
                if (!entity_json.is_object()) {
                    diagnostics.push_back("scene entity must be an object");
                    continue;
                }
                SceneEntityDocument entity{
                    .id = entity_json.string_at("id"),
                    .name = entity_json.string_at("name"),
                    .parent = entity_json.string_at("parent"),
                };
                if (const JsonValue* component_json = entity_json.find("components")) {
                    if (!component_json->is_object()) {
                        diagnostics.push_back("entity '" + entity.id + "' components must be an object");
                    } else {
                        for (const auto& [component_name, component_value] : component_json->members()) {
                            entity.components.push_back(parse_component(component_name, component_value, components, diagnostics));
                        }
                    }
                }
                scene.entities.push_back(std::move(entity));
            }
        }
    }

    if (const JsonValue* prefabs = root.find("prefabs")) {
        if (!prefabs->is_array()) {
            diagnostics.push_back("scene prefabs must be an array");
        } else {
            for (const JsonValue& prefab_json : prefabs->items()) {
                if (!prefab_json.is_object()) {
                    diagnostics.push_back("scene prefab entry must be an object");
                    continue;
                }
                ScenePrefabInstanceAsset prefab{
                    .id = prefab_json.string_at("id"),
                    .asset = prefab_json.string_at("asset"),
                    .name = prefab_json.string_at("name"),
                    .parent = prefab_json.string_at("parent"),
                    .pos = vec2f_from_json(prefab_json.find("pos")),
                };
                if (const JsonValue* overrides = prefab_json.find("overrides")) {
                    if (!overrides->is_object()) {
                        diagnostics.push_back("prefab '" + prefab.id + "' overrides must be an object");
                    } else {
                        for (const auto& [entity_id, entity_overrides] : overrides->members()) {
                            prefab.overrides.entities[entity_id] =
                                parse_prefab_component_map(entity_overrides, components, diagnostics);
                        }
                    }
                }
                scene.prefabs.push_back(std::move(prefab));
            }
        }
    }
    return scene;
}

void write_component(JsonWriter& json, const SceneComponentDocument& component) {
    json.key(component.name).begin_object();
    for (const ComponentFieldSnapshot& field : component.fields) {
        json.key(field.name);
        write_component_field_value_json(json, field.value);
    }
    json.end_object();
}

void write_prefab_component(JsonWriter& json, const PrefabComponentAsset& component) {
    json.key(component.name).begin_object();
    for (const PrefabFieldAsset& field : component.fields) {
        json.key(field.name);
        write_component_field_value_json(json, field.value);
    }
    json.end_object();
}

std::vector<std::string> validate_components(std::string_view entity_id,
                                             const std::vector<SceneComponentDocument>& scene_components,
                                             const EcsComponentRegistry& registry) {
    std::vector<std::string> diagnostics;
    std::unordered_set<std::string> component_names;
    for (const SceneComponentDocument& component : scene_components) {
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
        for (const ComponentFieldSnapshot& field : component.fields) {
            if (!field_names.insert(field.name).second) {
                diagnostics.push_back("entity '" + std::string{entity_id} + "' component '" + component.name + "' has duplicate field '" + field.name + "'");
            }
            const ComponentFieldDescriptor* descriptor_field = find_field(*descriptor, field.name);
            if (!descriptor_field) {
                diagnostics.push_back("entity '" + std::string{entity_id} + "' component '" + component.name + "' references unknown field '" + field.name + "'");
                continue;
            }
            if (descriptor_field->kind != field.kind) {
                diagnostics.push_back("entity '" + std::string{entity_id} + "' component '" + component.name + "' field '" + field.name + "' has a type mismatch");
            }
        }
    }
    return diagnostics;
}

bool apply_component(EcsWorld& world,
                     EcsEntity entity,
                     const SceneComponentDocument& component,
                     std::vector<std::string>& diagnostics) {
    if (!world.components().has(entity, component.name) && !world.components().add(entity, component.name)) {
        diagnostics.push_back(world.components().last_error());
        return false;
    }
    for (const ComponentFieldSnapshot& field : component.fields) {
        if (!world.components().patch_field(entity, component.name, field.name, field.value)) {
            diagnostics.push_back(world.components().last_error());
            return false;
        }
    }
    return true;
}

std::string scene_prefab_authored_id(std::string_view instance_id, std::string_view prefab_entity_id, bool root) {
    if (root) {
        return std::string{instance_id};
    }
    return std::string{instance_id} + "." + std::string{prefab_entity_id};
}

void assign_prefab_authored_ids(EcsWorld& world,
                                const ScenePrefabInstanceAsset& prefab_config,
                                const PrefabAsset& prefab,
                                PrefabInstance& instance,
                                std::vector<std::string>& diagnostics) {
    for (const PrefabEntityAsset& prefab_entity : prefab.entities) {
        const bool root = prefab_entity.id == prefab.root;
        const std::string authored_id = scene_prefab_authored_id(prefab_config.id, prefab_entity.id, root);
        EcsEntity entity = instance.by_authored_id.at(prefab_entity.id);
        if (!world.entities().set_authored_id(entity, authored_id)) {
            diagnostics.push_back(world.entities().last_error());
            continue;
        }
        instance.by_authored_id[authored_id] = entity;
    }
}

void mark_prefab_metadata(EcsWorld& world,
                          const ScenePrefabInstanceAsset& prefab_config,
                          const PrefabAsset& prefab,
                          PrefabInstance& instance) {
    instance.instance_id = prefab_config.id;
    instance.source_prefab_id = prefab.id;
    instance.root.set(PrefabInstanceComponent{
        .instance_id = prefab_config.id,
        .prefab_id = prefab.id,
        .asset_path = prefab_config.asset.string(),
    });
    for (const PrefabEntityAsset& prefab_entity : prefab.entities) {
        EcsEntity entity = instance.by_authored_id.at(prefab_entity.id);
        entity.set(PrefabEntityComponent{
            .instance_id = prefab_config.id,
            .authored_entity_id = prefab_entity.id,
        });
    }
    world.events().emit_component(EcsEventKind::ComponentAdded,
                                  EcsEventSource::KinApi,
                                  instance.root,
                                  static_cast<ComponentId>(world.component<PrefabInstanceComponent>().id()),
                                  "PrefabInstanceComponent");
}

} // namespace

void register_builtin_editor_components(EcsWorld& world) {
    if (!world.components().find("Transform2D")) {
        world.components().native<Transform2D>("Transform2D")
            .field("pos", &Transform2D::pos)
            .field("rotation", &Transform2D::rotation)
            .field("scale", &Transform2D::scale);
    }
    if (!world.components().find("RectRenderer")) {
        world.components().native<RectRenderer>("RectRenderer")
            .field("offset", &RectRenderer::offset)
            .field("size", &RectRenderer::size)
            .field("color", &RectRenderer::color)
            .field("layer", &RectRenderer::layer)
            .field("order", &RectRenderer::order)
            .field("visible", &RectRenderer::visible);
    }
    if (!world.components().find("LineRenderer")) {
        world.components().native<LineRenderer>("LineRenderer")
            .field("a", &LineRenderer::a)
            .field("b", &LineRenderer::b)
            .field("color", &LineRenderer::color)
            .field("layer", &LineRenderer::layer)
            .field("order", &LineRenderer::order)
            .field("visible", &LineRenderer::visible);
    }
    if (!world.components().find("Timer")) {
        world.components().data("Timer")
            .field_f32("duration", 0.0f)
            .field_f32("elapsed", 0.0f)
            .field_bool("repeating", false)
            .field_bool("finished", false)
            .field_i32("ticks", 0);
    }
    if (!world.components().find("Lifetime")) {
        world.components().data("Lifetime")
            .field_f32("remaining", 0.0f);
    }
}

void register_scene_metadata_components(EcsWorld& world) {
    world.component<PrefabInstanceComponent>("PrefabInstanceComponent");
    world.component<PrefabEntityComponent>("PrefabEntityComponent");
}

SceneLoadResult load_scene_asset(const std::filesystem::path& path,
                                 const EcsComponentRegistry& components) {
    const std::optional<std::string> text = read_content_text(path);
    if (!text) {
        return {.diagnostics = {"failed to open scene '" + path.string() + "'"}};
    }
    return parse_scene_asset(*text, components, path.string());
}

SceneLoadResult parse_scene_asset(std::string_view text,
                                  const EcsComponentRegistry& components,
                                  std::string_view source) {
    JsonParseResult parsed = parse_json(text);
    if (!parsed.ok()) {
        std::string prefix = source.empty() ? std::string{} : std::string{source} + ": ";
        return {.diagnostics = {prefix + parsed.error}};
    }
    if (!parsed.value->is_object()) {
        return {.diagnostics = {"scene root must be an object"}};
    }

    std::vector<std::string> diagnostics;
    SceneAsset scene = parse_scene_object(*parsed.value, components, diagnostics);
    std::vector<std::string> validation = validate_scene_asset(scene, components);
    diagnostics.insert(diagnostics.end(), validation.begin(), validation.end());
    if (!diagnostics.empty()) {
        return {.diagnostics = std::move(diagnostics)};
    }
    return {.scene = std::move(scene)};
}

bool save_scene_asset(const SceneAsset& scene, const std::filesystem::path& path) {
    std::ofstream out(path);
    if (!out) {
        return false;
    }
    write_scene_asset(out, scene);
    return static_cast<bool>(out);
}

void write_scene_asset(std::ostream& out, const SceneAsset& scene) {
    JsonWriter json(out);
    json.begin_object();
    json.field("schema", std::string_view{scene.schema});
    json.field("name", std::string_view{scene.name});
    if (!scene.script.empty()) {
        json.field("script", std::string_view{scene.script.string()});
    }
    json.key("clear_color").begin_array();
    json.value(static_cast<i32>(scene.clear_color.r));
    json.value(static_cast<i32>(scene.clear_color.g));
    json.value(static_cast<i32>(scene.clear_color.b));
    json.end_array();
    json.key("entities").begin_array();
    for (const SceneEntityDocument& entity : scene.entities) {
        json.begin_object();
        json.field("id", std::string_view{entity.id});
        if (!entity.name.empty()) {
            json.field("name", std::string_view{entity.name});
        }
        if (!entity.parent.empty()) {
            json.field("parent", std::string_view{entity.parent});
        }
        json.key("components").begin_object();
        for (const SceneComponentDocument& component : entity.components) {
            write_component(json, component);
        }
        json.end_object();
        json.end_object();
    }
    json.end_array();
    json.key("prefabs").begin_array();
    for (const ScenePrefabInstanceAsset& prefab : scene.prefabs) {
        json.begin_object();
        json.field("id", std::string_view{prefab.id});
        json.field("asset", std::string_view{prefab.asset.string()});
        if (!prefab.name.empty()) {
            json.field("name", std::string_view{prefab.name});
        }
        if (!prefab.parent.empty()) {
            json.field("parent", std::string_view{prefab.parent});
        }
        if (prefab.pos.x != 0.0f || prefab.pos.y != 0.0f) {
            json.key("pos").begin_object();
            json.field("x", static_cast<f64>(prefab.pos.x));
            json.field("y", static_cast<f64>(prefab.pos.y));
            json.end_object();
        }
        if (!prefab.overrides.entities.empty()) {
            json.key("overrides").begin_object();
            for (const auto& [entity_id, components] : prefab.overrides.entities) {
                json.key(entity_id).begin_object();
                for (const PrefabComponentAsset& component : components) {
                    write_prefab_component(json, component);
                }
                json.end_object();
            }
            json.end_object();
        }
        json.end_object();
    }
    json.end_array();
    json.end_object();
    out << '\n';
}

std::vector<std::string> validate_scene_asset(const SceneAsset& scene,
                                              const EcsComponentRegistry& components) {
    std::vector<std::string> diagnostics;
    if (scene.schema != "kin.scene/1") {
        diagnostics.push_back("unsupported scene schema '" + scene.schema + "'");
    }
    std::unordered_set<std::string> ids;
    for (const SceneEntityDocument& entity : scene.entities) {
        if (entity.id.empty()) {
            diagnostics.push_back("scene entity id is required");
        } else if (!ids.insert(entity.id).second) {
            diagnostics.push_back("duplicate scene authored id '" + entity.id + "'");
        }
        if (!entity.parent.empty() && !find_scene_entity(scene, entity.parent)) {
            diagnostics.push_back("entity '" + entity.id + "' parent '" + entity.parent + "' does not exist");
        }
        std::vector<std::string> component_diagnostics = validate_components(entity.id, entity.components, components);
        diagnostics.insert(diagnostics.end(), component_diagnostics.begin(), component_diagnostics.end());
    }
    for (const ScenePrefabInstanceAsset& prefab : scene.prefabs) {
        if (prefab.id.empty()) {
            diagnostics.push_back("scene prefab instance id is required");
            continue;
        }
        if (!ids.insert(prefab.id).second || find_scene_prefab(scene, prefab.id) != &prefab) {
            diagnostics.push_back("duplicate scene authored id '" + prefab.id + "'");
        }
        if (prefab.asset.empty()) {
            diagnostics.push_back("prefab instance '" + prefab.id + "' requires an asset");
        }
        if (!prefab.parent.empty() && !ids.contains(prefab.parent) && !find_scene_entity(scene, prefab.parent)) {
            diagnostics.push_back("prefab instance '" + prefab.id + "' parent '" + prefab.parent + "' does not exist");
        }
    }
    return diagnostics;
}

SceneInstance instantiate_scene_asset(EcsWorld& world,
                                      const SceneAsset& scene,
                                      const std::filesystem::path& asset_root,
                                      EcsComponentRegistry& components) {
    SceneInstance instance;
    instance.diagnostics = validate_scene_asset(scene, components);
    if (!instance.diagnostics.empty()) {
        return instance;
    }

    register_scene_metadata_components(world);

    std::unordered_map<std::string, EcsEntity> created;
    for (const SceneEntityDocument& entity : scene.entities) {
        EcsEntity runtime = world.entities().create({
            .name = entity.name,
            .authored_id = entity.id,
        });
        if (!runtime) {
            instance.diagnostics.push_back(world.entities().last_error());
            return instance;
        }
        created[entity.id] = runtime;
        instance.by_authored_id[entity.id] = runtime;
    }

    for (const SceneEntityDocument& entity : scene.entities) {
        EcsEntity runtime = created.at(entity.id);
        if (!entity.parent.empty()) {
            if (!world.entities().reparent(runtime, created.at(entity.parent))) {
                instance.diagnostics.push_back(world.entities().last_error());
                return instance;
            }
        }
        for (const SceneComponentDocument& component : entity.components) {
            if (!apply_component(world, runtime, component, instance.diagnostics)) {
                return instance;
            }
        }
    }

    for (const ScenePrefabInstanceAsset& prefab_config : scene.prefabs) {
        PrefabLoadResult loaded = load_prefab_asset(asset_root / prefab_config.asset, components);
        if (!loaded.ok()) {
            instance.diagnostics.push_back("failed to load prefab '" + prefab_config.asset.string() + "'");
            instance.diagnostics.insert(instance.diagnostics.end(), loaded.diagnostics.begin(), loaded.diagnostics.end());
            return instance;
        }
        for (const PrefabEntityAsset& prefab_entity : loaded.prefab->entities) {
            const bool root = prefab_entity.id == loaded.prefab->root;
            const std::string authored_id = scene_prefab_authored_id(prefab_config.id, prefab_entity.id, root);
            if (instance.by_authored_id.contains(authored_id) || world.entities().find_by_authored_id(authored_id)) {
                instance.diagnostics.push_back("duplicate prefab authored entity id '" + authored_id + "'");
                return instance;
            }
        }
        EcsEntity parent;
        if (!prefab_config.parent.empty()) {
            if (const auto found = instance.by_authored_id.find(prefab_config.parent); found != instance.by_authored_id.end()) {
                parent = found->second;
            }
            if (!parent) {
                instance.diagnostics.push_back("prefab instance '" + prefab_config.id + "' parent '" + prefab_config.parent + "' does not exist");
                return instance;
            }
        }
        PrefabInstance prefab_instance = instantiate_prefab(world,
                                                           *loaded.prefab,
                                                           PrefabSpawn{
                                                               .pos = prefab_config.pos,
                                                               .name = prefab_config.name,
                                                               .parent = parent,
                                                           },
                                                           components,
                                                           prefab_config.overrides);
        if (!prefab_instance.ok()) {
            instance.diagnostics.insert(instance.diagnostics.end(), prefab_instance.diagnostics.begin(), prefab_instance.diagnostics.end());
            return instance;
        }
        assign_prefab_authored_ids(world, prefab_config, *loaded.prefab, prefab_instance, instance.diagnostics);
        if (!instance.diagnostics.empty()) {
            return instance;
        }
        mark_prefab_metadata(world, prefab_config, *loaded.prefab, prefab_instance);

        instance.prefab_instances[prefab_config.id] = prefab_instance;
        for (const PrefabEntityAsset& prefab_entity : loaded.prefab->entities) {
            const bool root = prefab_entity.id == loaded.prefab->root;
            const std::string authored_id = scene_prefab_authored_id(prefab_config.id, prefab_entity.id, root);
            instance.by_authored_id[authored_id] = prefab_instance.by_authored_id.at(prefab_entity.id);
        }
    }

    for (EcsEntity entity : world.entities().hierarchy_roots()) {
        instance.roots.push_back(entity);
    }
    return instance;
}

SceneAsset scene_asset_from_world(EcsWorld& world, const SceneSaveOptions& options) {
    (void)options;
    SceneAsset scene;
    scene.entities = scene_document_from_world(world).entities;
    return scene;
}

} // namespace kin
