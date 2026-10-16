#include <kin/scene/scene_asset.hpp>

#include <kin/assets/content.hpp>
#include <kin/core/json.hpp>
#include <kin/core/json_value.hpp>
#include <kin/ecs/render.hpp>
#if defined(KIN_EXPERIMENTAL_3D)
#include <kin/ecs/scene3d.hpp>
#endif

#include <algorithm>
#include <fstream>
#include <ostream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include "../prefab/prefab_asset_json.hpp"

namespace kin {
namespace {

struct SceneHeader {
    std::string name;
    std::filesystem::path script;
    Color clear_color;
};

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

SceneComponentDocument parse_component(std::string_view component_name,
                                       const JsonValue& json,
                                       const EcsComponentRegistry& components,
                                       std::vector<std::string>& diagnostics) {
    return SceneComponentDocument{
        .name = std::string{component_name},
        .fields = parse_component_fields(component_name, json, components, diagnostics),
    };
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
            for (const JsonValue& reference_json : prefabs->items()) {
                if (!reference_json.is_object()) {
                    diagnostics.push_back("scene prefab entry must be an object");
                    continue;
                }
                scene.prefabs.push_back(prefab_json::reference(reference_json,components,diagnostics));
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
                          PrefabInstance& instance,
                          std::shared_ptr<const SceneDocument> source) {
    instance.instance_id = prefab_config.id;
    instance.source_prefab_id = prefab.id;
    instance.root.set(PrefabInstanceComponent{
        .instance_id = prefab_config.id,
        .prefab_id = prefab.id,
        .asset_path = prefab_config.asset.string(),
        .source = std::move(source),
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
#if defined(KIN_EXPERIMENTAL_3D)
    register_render3d_components(world);
    register_physics3d_components(world);
#endif
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
    for(const auto& prefab:scene.prefabs)prefab_json::write_reference(json,prefab);
    json.end_array();
    json.end_object();
    out << '\n';
}

std::vector<std::string> validate_scene_asset(const SceneAsset& scene,
                                              const EcsComponentRegistry& components) {
    std::vector<std::string> diagnostics;
    const auto parent_root = [&](std::string_view id) -> std::string {
        if (find_scene_entity(scene, id)) return std::string(id);
        const ScenePrefabInstanceAsset* match = nullptr;
        for (const auto& prefab : scene.prefabs) {
            if (id == prefab.id || id.starts_with(prefab.id + "."))
                if (!match || prefab.id.size() > match->id.size()) match = &prefab;
        }
        return match ? match->id : std::string{};
    };
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
        if (!entity.parent.empty() && parent_root(entity.parent).empty()) {
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
        if (!prefab.parent.empty() && parent_root(prefab.parent).empty()) {
            diagnostics.push_back("prefab instance '" + prefab.id + "' parent '" + prefab.parent + "' does not exist");
        }
    }
    std::unordered_map<std::string, std::string> parents;
    for (const auto& entity : scene.entities) parents[entity.id] = parent_root(entity.parent);
    for (const auto& prefab : scene.prefabs) parents[prefab.id] = parent_root(prefab.parent);
    for (const auto& [id, ignored] : parents) {
        (void)ignored;
        std::unordered_set<std::string> seen;
        auto node = id;
        while (!node.empty() && parents.contains(node)) {
            if (!seen.insert(node).second) { diagnostics.push_back("scene parent cycle involving '" + id + "'"); break; }
            node = parents.at(node);
        }
    }
    return diagnostics;
}

SceneInstance instantiate_scene_asset(EcsWorld& world,
                                      const SceneAsset& scene,
                                      const std::filesystem::path& asset_root,
                                      EcsComponentRegistry& components,PrefabAssetLoadOptions limits) {
    SceneInstance instance;
    instance.diagnostics = validate_scene_asset(scene, components);
    if (!instance.diagnostics.empty()) {
        return instance;
    }

    register_scene_metadata_components(world);

    struct Rollback {
        EcsWorld& world;
        std::vector<EcsEntity> owned;
        bool committed = false;
        ~Rollback() {
            if (!committed) for (auto it = owned.rbegin(); it != owned.rend(); ++it)
                if (it->alive()) world.entities().destroy(*it);
        }
    } rollback{world};

    std::unordered_map<std::string, EcsEntity> created;
    std::unordered_map<std::string, std::shared_ptr<const SceneDocument>> prefab_sources;
    std::unordered_map<std::string, PrefabLoadResult> prefab_assets;
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
        rollback.owned.push_back(runtime);
        instance.by_authored_id[entity.id] = runtime;
    }

    for (const SceneEntityDocument& entity : scene.entities) {
        EcsEntity runtime = created.at(entity.id);
        for (const SceneComponentDocument& component : entity.components) {
            if (!apply_component(world, runtime, component, instance.diagnostics)) {
                return instance;
            }
        }
    }

    std::vector<const ScenePrefabInstanceAsset*> pending;
    for (const auto& prefab : scene.prefabs) pending.push_back(&prefab);
    while (!pending.empty()) {
        const auto next = std::ranges::find_if(pending, [&](const auto* prefab) {
            return prefab->parent.empty() || instance.by_authored_id.contains(prefab->parent);
        });
        if (next == pending.end()) {
            instance.diagnostics.push_back("scene prefab parents cannot be resolved");
            return instance;
        }
        const auto& prefab_config = **next;
        pending.erase(next);
        const auto source_path = (asset_root / prefab_config.asset).lexically_normal();
        const auto source_key = source_path.string();
        auto [cached, inserted] = prefab_assets.try_emplace(source_key);
        limits.content_root=asset_root;
        if (inserted) cached->second = load_prefab_asset(source_path, components,limits);
        auto& loaded = cached->second;
        if (!loaded.ok()) {
            instance.diagnostics.push_back("failed to load prefab '" + prefab_config.asset.string() + "'");
            instance.diagnostics.insert(instance.diagnostics.end(), loaded.diagnostics.begin(), loaded.diagnostics.end());
            return instance;
        }
        auto edited=apply_prefab_overrides(*loaded.prefab,prefab_config.overrides,components,false);
        if(!edited.ok()){instance.diagnostics=std::move(edited.diagnostics);return instance;}
        const auto& effective=*edited.prefab;
        for (const PrefabEntityAsset& prefab_entity : effective.entities) {
            const bool root = prefab_entity.id == effective.root;
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
        rollback.owned.insert(rollback.owned.end(), prefab_instance.entities.begin(), prefab_instance.entities.end());
        assign_prefab_authored_ids(world, prefab_config, effective, prefab_instance, instance.diagnostics);
        if (!instance.diagnostics.empty()) {
            return instance;
        }
        auto& source = prefab_sources[source_key];
        if(!source)source=prefab_instance.source;
        mark_prefab_metadata(world,prefab_config,effective,prefab_instance,source);

        instance.prefab_instances[prefab_config.id] = prefab_instance;
        for (const PrefabEntityAsset& prefab_entity : effective.entities) {
            const bool root = prefab_entity.id == effective.root;
            const std::string authored_id = scene_prefab_authored_id(prefab_config.id, prefab_entity.id, root);
            instance.by_authored_id[authored_id] = prefab_instance.by_authored_id.at(prefab_entity.id);
        }
    }

    for (const auto& entity : scene.entities) if (!entity.parent.empty()) {
        const auto parent = instance.by_authored_id.find(entity.parent);
        if (parent == instance.by_authored_id.end() || !world.entities().reparent(created.at(entity.id), parent->second)) {
            instance.diagnostics.push_back("entity '" + entity.id + "' parent '" + entity.parent + "' cannot be resolved");
            return instance;
        }
    }
    // Global authored references can point forward to ordinary entities or
    // prefab children. Local prefab refs already have their runtime IDs.
    for (const auto& [id, entity] : instance.by_authored_id) {
        (void)id;
        for (const auto& component : world.snapshot(entity).components) for (const auto& field : component.fields) {
            if (const auto* reference = std::get_if<ComponentEntityRef>(&field.value); reference && !reference->id && !reference->authored_id.empty()) {
                const auto target = world.entities().find_by_authored_id(reference->authored_id);
                if (!target) { instance.diagnostics.push_back("unresolved entity reference '" + reference->authored_id + "'"); return instance; }
                auto resolved = *reference; resolved.id = target.id();
                if (!components.patch_field(entity, component.descriptor.id, field.name, resolved)) {
                    instance.diagnostics.push_back(components.last_error()); return instance;
                }
            }
        }
    }

    for (EcsEntity entity : world.entities().hierarchy_roots()) {
        instance.roots.push_back(entity);
    }
    world.raw().entity("kin::prefab::scene_header").set(SceneHeader{scene.name, scene.script, scene.clear_color});
    rollback.committed = true;
    return instance;
}

SceneAsset scene_asset_from_world(EcsWorld& world, const SceneSaveOptions& options) {
    auto result = build_scene_asset(world, options);
    if (!result.ok()) throw std::runtime_error(result.diagnostics.front());
    return std::move(*result.scene);
}

bool link_prefab_entity(EcsWorld& world,EcsEntity root,EcsEntity child,std::string local_id) {
    if(!root||!child||root.id()==child.id()||local_id.empty()||child.has<PrefabInstanceComponent>())return false;
    const auto* link=root.get<PrefabInstanceComponent>();const auto* root_member=root.get<PrefabEntityComponent>();
    const auto parent_entity=child.parent();if(!parent_entity)return false;
    const auto* parent=parent_entity.get<PrefabEntityComponent>();
    if(!link||!root_member||local_id==root_member->authored_entity_id||!parent||parent->instance_id!=link->instance_id)return false;
    if(const auto* member=child.get<PrefabEntityComponent>();member&&(member->instance_id!=link->instance_id||member->authored_entity_id!=local_id))return false;
    const auto instance=link->instance_id;const auto global=instance+"."+local_id;
    if(auto existing=world.entities().find_by_authored_id(global);existing&&existing.id()!=child.id())return false;
    if(!world.entities().set_authored_id(child,global))return false;
    child.set(PrefabEntityComponent{instance,std::move(local_id)});return true;
}

bool remove_prefab_component(EcsWorld& world,EcsEntity entity,std::string_view component) {
    if(!entity||!entity.has<PrefabEntityComponent>()||!world.components().has(entity,component))return false;
    const auto snapshot=world.snapshot(entity);
    for(const auto& c:snapshot.components) {
        if(c.descriptor.kind==ComponentKind::Native){
            const auto* info=ecs_get_type_info(world.raw().c_ptr(),c.descriptor.id);
            if(info&&info->size){const auto* value=ecs_get_id(world.raw().c_ptr(),entity.id(),c.descriptor.id);ecs_set_id(world.raw().c_ptr(),entity.id(),c.descriptor.id,info->size,value);}
            else entity.raw().add(c.descriptor.id);
        } else {
            if(!world.components().add(entity,c.descriptor.id))return false;
            for(const auto& f:c.fields)if(!world.components().patch_field(entity,c.descriptor.id,f.name,f.value))return false;
        }
    }
    entity.raw().remove(flecs::IsA,flecs::Wildcard);
    return world.components().remove(entity,component);
}

SceneSaveResult build_scene_asset(EcsWorld& world, const SceneSaveOptions& options) {
    SceneAsset scene;
    SceneSaveResult result;
    const auto header = world.raw().lookup("kin::prefab::scene_header");
    if (header) if (const auto* value = header.get<SceneHeader>()) {
        scene.name = value->name; scene.script = value->script; scene.clear_color = value->clear_color;
    }
    auto document = scene_document_from_world(world);
    std::unordered_map<std::string, EcsEntity> entities;
    for (const auto& item : document.entities) {
        auto entity = world.entities().find_by_authored_id(item.id);
        if (entity) entities.emplace(item.id, entity);
    }
    if (options.include_runtime_entities) {
        const auto include = [&](EcsEntity entity) {
            // Authored entities already have a canonical document ID. Adding
            // a synthetic alias could overwrite prefab membership with an ID
            // absent from the document, depending on unordered-map iteration.
            const auto authored = world.entities().authored_id(entity).value_or(std::string{});
            entities.try_emplace(authored.empty() ? "entity." + std::to_string(entity.id()) : authored, entity);
        };
        for (auto entity : world.entities().all()) include(entity);
        for (auto entity : world.components().entities()) include(entity);
        world.raw().each([&](flecs::entity raw) { include(EcsEntity{raw}); });
    }
    std::unordered_map<std::string, const SceneEntityDocument*> documents;
    for (const auto& item : document.entities) if (entities.contains(item.id)) documents.emplace(item.id, &item);
    for (auto& item : document.entities) if (entities.contains(item.id)) {
        for (auto& component : item.components) for (auto& field : component.fields) {
            bool valid = detail::field_value_matches_kind(field.value, field.kind);
            if (const auto* scalar = std::get_if<f32>(&field.value)) valid &= std::isfinite(*scalar);
            if (const auto* scalar = std::get_if<f64>(&field.value)) valid &= std::isfinite(*scalar);
            if (!valid) result.diagnostics.push_back("entity '" + item.id + "' has an invalid value for '" + component.name + "." + field.name + "'");
            if (auto* ref = std::get_if<ComponentEntityRef>(&field.value)) {
                if (ref->id && world.raw().is_alive(ref->id)) {
                    ref->authored_id = world.entities().authored_id(world.entity_by_id(ref->id)).value_or(std::string{});
                    if (ref->authored_id.empty() && options.include_runtime_entities) ref->authored_id = "entity." + std::to_string(ref->id);
                }
                if ((ref->id || !ref->authored_id.empty()) && !documents.contains(ref->authored_id))
                    result.diagnostics.push_back("entity '" + item.id + "' reference '" + component.name + "." + field.name + "' targets an entity excluded from this save");
            }
        }
    }
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> members;
    for (const auto& [id, entity] : entities) if (const auto* member = entity.get<PrefabEntityComponent>()) {
        if(!members[member->instance_id].emplace(member->authored_entity_id,id).second)
            result.diagnostics.push_back("duplicate local prefab entity ID '"+member->authored_entity_id+"'");
    }
    const auto error = [&](const std::string& instance, const std::string& reason) {
        result.diagnostics.push_back("prefab '" + instance + "': " + reason + "; unpack explicitly for unsupported edits");
    };
    if(options.preserve_prefabs)for(const auto& [id,group]:members) {
        const bool rooted=std::ranges::any_of(entities,[&](const auto& entry){const auto* link=entry.second.template get<PrefabInstanceComponent>();return link&&link->instance_id==id;});
        if(!rooted)error(id,"orphaned instance members have no linked root");
    }
    for (auto& item : document.entities) {
        const auto runtime = entities.find(item.id);
        if (runtime == entities.end()) continue;
        const auto* member = runtime->second.get<PrefabEntityComponent>();
        if (!options.preserve_prefabs || !member) {
            scene.entities.push_back(std::move(item));
            continue;
        }
        const auto* link = runtime->second.get<PrefabInstanceComponent>();
        if (!link) continue; // serialized as an override of the instance root
        if(item.id!=link->instance_id){error(item.id,"linked instance authored ID changed");continue;}
        if (!link->source || link->asset_path.empty()) { error(item.id, "source defaults are unavailable"); continue; }
        ScenePrefabInstanceAsset prefab{.id = item.id, .asset = link->asset_path, .name = item.name, .parent = item.parent};
        const auto& group = members[link->instance_id];
        const auto canonical = [&](ComponentFieldValue value) {
            if (auto* ref = std::get_if<ComponentEntityRef>(&value)) {
                if (ref->id && world.raw().is_alive(ref->id)) {
                    const auto target = world.entity_by_id(ref->id);
                    if (const auto* local = target.get<PrefabEntityComponent>(); local && local->instance_id == link->instance_id)
                        ref->authored_id = local->authored_entity_id;
                }
                ref->id = 0;
            }
            return value;
        };
        for (const auto& base : link->source->entities) {
            const auto found = group.find(base.id);
            if (found == group.end() || !documents.contains(found->second)) {
                if(base.parent.empty()||group.contains(base.parent))prefab.overrides.remove_entities.push_back(base.id);
                continue;
            }
            const auto& live = *documents.at(found->second);
            const bool original_name = live.name == base.name || live.name == base.name + "." + std::to_string(entities.at(found->second).id());
            if (base.id != member->authored_entity_id) {
                if (!original_name) {
                    if(live.name.empty())error(item.id,"child names cannot be empty");
                    else prefab.overrides.names[base.id]=live.name;
                }
                const auto parent = group.find(base.parent);
                const auto expected=parent==group.end()?std::string{}:parent->second;
                if(live.parent!=expected) {
                    const auto local=std::ranges::find_if(group,[&](const auto& pair){return pair.second==live.parent;});
                    if(local==group.end())error(item.id,"child parent is outside the prefab instance");
                    else prefab.overrides.parents[base.id]=local->first;
                }
            } else {
                if (original_name) prefab.name.clear();
                else if (live.name.empty()) error(item.id, "root name was cleared");
            }
            for (const auto& component : base.components) {
                if (std::ranges::none_of(live.components, [&](const auto& candidate) { return candidate.name == component.name; }))
                    prefab.overrides.remove_components[base.id].push_back(component.name);
            }
            for (const auto& component : live.components) {
                const auto original = std::ranges::find_if(base.components, [&](const auto& candidate) { return candidate.name == component.name; });
                PrefabComponentAsset changed{.name = component.name};
                for (const auto& field : component.fields) {
                    const auto* before = original == base.components.end() ? nullptr : [&]() -> const ComponentFieldSnapshot* {
                        const auto f = std::ranges::find_if(original->fields, [&](const auto& old) { return old.name == field.name; });
                        return f == original->fields.end() ? nullptr : &*f;
                    }();
                    if (!before || canonical(before->value) != canonical(field.value)) changed.fields.push_back({field.name, canonical(field.value)});
                }
                if (original == base.components.end() || !changed.fields.empty()) prefab.overrides.entities[base.id].push_back(std::move(changed));
            }
        }
        for(const auto& [local,id]:group) {
            if(std::ranges::any_of(link->source->entities,[&](const auto& e){return e.id==local;}))continue;
            if(!documents.contains(id)){error(item.id,"added child is excluded from save");continue;}
            const auto& live=*documents.at(id);
            const auto parent=std::ranges::find_if(group,[&](const auto& pair){return pair.second==live.parent;});
            if(parent==group.end()){error(item.id,"added child parent is outside the prefab instance");continue;}
            PrefabEntityAsset added{.id=local,.name=live.name,.parent=parent->first};
            for(const auto& component:live.components){PrefabComponentAsset c{.name=component.name};for(const auto& field:component.fields)c.fields.push_back({field.name,canonical(field.value)});added.components.push_back(std::move(c));}
            prefab.overrides.add_entities.push_back(std::move(added));
        }
        std::ranges::sort(prefab.overrides.add_entities,{},&PrefabEntityAsset::id);
        scene.prefabs.push_back(std::move(prefab));
    }
    if (!result.diagnostics.empty()) return result;
    result.diagnostics = validate_scene_asset(scene, world.components());
    if (result.diagnostics.empty()) result.scene = std::move(scene);
    return result;
}

} // namespace kin
