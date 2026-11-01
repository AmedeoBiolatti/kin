#include <kin/scene/scene_asset.hpp>

#include <kin/assets/asset_manager.hpp>
#include <kin/assets/content.hpp>
#include <kin/core/json.hpp>
#include <kin/core/json_value.hpp>
#include <kin/ecs/animator.hpp>
#include <kin/ecs/camera.hpp>
#include <kin/ecs/lighting.hpp>
#include <kin/ecs/particles.hpp>
#include <kin/ecs/tilemap.hpp>
#include <kin/ecs/render.hpp>
#include <kin/ecs/script.hpp>
#include <kin/ecs/shader_effect.hpp>
#include <kin/ecs/dialogue.hpp>
#include <kin/ecs/scene_flow.hpp>
#include <kin/ecs/ui_screens.hpp>
#include <kin/ecs/sprite_animator.hpp>
#include <kin/ecs/text.hpp>
#if KIN_ENABLE_AUDIO
#include <kin/ecs/audio.hpp>
#endif
#if KIN_ENABLE_PHYSICS
#include <kin/ecs/physics.hpp>
#endif
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

// A renderer's `blend` field described: how it combines with what is under it.
template <typename T>
EcsComponentRegistry::NativeComponentBuilder<T>& describe_blend(EcsComponentRegistry::NativeComponentBuilder<T>& builder) {
    return builder
        .template property<std::string>("blend", [](const T& r) { return std::string(blend_mode_name(r.blend)); },
                                        [](T& r, const std::string& name) {
                                            const std::optional<BlendMode> mode = blend_mode_named(name.empty() ? "alpha" : name);
                                            if (!mode) return false;
                                            r.blend = *mode;
                                            return true;
                                        })
        .choices({"alpha", "additive", "multiply", "max", "min"})
        .description("How it combines with what is under it: additive for glows and light, multiply for shade.");
}

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

#if KIN_ENABLE_PHYSICS
// A collider component as scenes and tools see it: its first collider (one
// is made by the first field set). A change lets its fixture go, so the next
// sync makes it again from the change (and destroys the one it replaces).
PhysicsFixtureDef first_collider(const PhysicsColliderComponent& component) {
    return component.colliders.empty() ? PhysicsFixtureDef{} : component.colliders.front().def;
}

PhysicsFixtureDef& first_collider(PhysicsColliderComponent& component) {
    if (component.colliders.empty()) {
        component.colliders.emplace_back();
    }
    component.colliders.front().fixture = {};
    return component.colliders.front().def;
}

// `member` (of a PhysicsFixtureDef, const or not) as a property of the first collider.
template <typename Field, typename Member>
EcsComponentRegistry::NativeComponentBuilder<PhysicsColliderComponent>& collider_field(
    EcsComponentRegistry::NativeComponentBuilder<PhysicsColliderComponent>& builder, std::string name, Member member) {
    return builder.property<Field>(std::move(name),
                                   [member](const PhysicsColliderComponent& component) {
                                       const PhysicsFixtureDef def = first_collider(component);
                                       return Field(member(def));
                                   },
                                   [member](PhysicsColliderComponent& component, const Field& value) {
                                       member(first_collider(component)) = value;
                                       return true;
                                   });
}

void register_physics2d_editor_components(EcsWorld& world) {
    EcsComponentRegistry& registry = world.components();
    if (!registry.find("PhysicsBodyComponent")) {
        // A Box2D body, made where the entity is when the game steps a
        // PhysicsWorld (update_physics_world), from these; Transform2D then
        // follows it. Its shapes are its PhysicsColliderComponent's.
        constexpr f32 degrees_per_radian = 180.0f / 3.14159265358979323846f;
        registry.native<PhysicsBodyComponent>("PhysicsBodyComponent")
            .property<std::string>("type",
                                   [](const PhysicsBodyComponent& body) {
                                       switch (body.def.type) {
                                       case PhysicsBodyType::Static: return std::string("static");
                                       case PhysicsBodyType::Dynamic: return std::string("dynamic");
                                       case PhysicsBodyType::Kinematic: return std::string("kinematic");
                                       }
                                       return std::string("static");
                                   },
                                   [](PhysicsBodyComponent& body, const std::string& type) {
                                       if (type == "static") body.def.type = PhysicsBodyType::Static;
                                       else if (type == "dynamic") body.def.type = PhysicsBodyType::Dynamic;
                                       else if (type == "kinematic") body.def.type = PhysicsBodyType::Kinematic;
                                       else return false;
                                       return true;
                                   })
            .choices({"static", "dynamic", "kinematic"})
            .description("static: stays; dynamic: moved by gravity, forces and what it hits; kinematic: moves at its velocity, and pushes.")
            .field("linear_velocity", &PhysicsBodyComponent::def, &PhysicsBodyDef::linear_velocity)
            .units("units/s")
            .description("Its velocity when it is made.")
            .property<f32>("angular_velocity",
                           [=](const PhysicsBodyComponent& body) { return body.def.angular_velocity * degrees_per_radian; },
                           [=](PhysicsBodyComponent& body, const f32& value) {
                               body.def.angular_velocity = value / degrees_per_radian;
                               return true;
                           })
            .units("degrees/s")
            .description("Its turning when it is made.")
            .field("linear_damping", &PhysicsBodyComponent::def, &PhysicsBodyDef::linear_damping)
            .field("angular_damping", &PhysicsBodyComponent::def, &PhysicsBodyDef::angular_damping)
            .field("gravity_scale", &PhysicsBodyComponent::def, &PhysicsBodyDef::gravity_scale)
            .field("fixed_rotation", &PhysicsBodyComponent::def, &PhysicsBodyDef::fixed_rotation)
            .description("It never turns.")
            .field("sync_to_physics", &PhysicsBodyComponent::sync_to_physics)
            .description("The entity leads: the body is put where it is before each step (it follows its parent).")
            .field("sync_from_physics", &PhysicsBodyComponent::sync_from_physics)
            .description("The body leads: Transform2D says where it is after each step.");
    }
    if (!registry.find("PhysicsColliderComponent")) {
        auto collider = registry.native<PhysicsColliderComponent>("PhysicsColliderComponent");
        // On an entity with no body, it is the body above it's (placed as
        // the entity is): a body of several shapes is an entity with a
        // child for each.
        static constexpr std::pair<PhysicsShapeType, const char*> shapes[] = {
            {PhysicsShapeType::Box, "box"},         {PhysicsShapeType::Circle, "circle"}, {PhysicsShapeType::Capsule, "capsule"},
            {PhysicsShapeType::Polygon, "polygon"}, {PhysicsShapeType::Chain, "chain"},
        };
        collider.property<std::string>("shape",
                                       [](const PhysicsColliderComponent& component) {
                                           const PhysicsShapeType type = first_collider(component).shape.type;
                                           for (const auto& [shape, name] : shapes) {
                                               if (shape == type) return std::string(name);
                                           }
                                           return std::string("box");
                                       },
                                       [](PhysicsColliderComponent& component, const std::string& name) {
                                           for (const auto& [shape, shape_name] : shapes) {
                                               if (name == shape_name) {
                                                   first_collider(component).shape.type = shape;
                                                   return true;
                                               }
                                           }
                                           return false;
                                       })
            .choices({"box", "circle", "capsule", "polygon", "chain"})
            .description("box; circle; capsule: a box with round ends; polygon: its points' (any, concave too); chain: a line through its points.");
        collider_field<Vec2f>(collider, "half_extents", [](auto& def) -> auto& { return def.shape.half_extents; })
            .description("A box's half width and half height; a capsule's (round at the ends of the longer).");
        collider_field<f32>(collider, "radius", [](auto& def) -> auto& { return def.shape.radius; })
            .description("A circle's.");
        collider_field<Vec2f>(collider, "offset", [](auto& def) -> auto& { return def.shape.offset; })
            .description("Its centre, from the entity's (a polygon's or a chain's points are from it).");
        constexpr f32 degrees_per_radian = 180.0f / 3.14159265358979323846f;
        collider.property<f32>("angle", [=](const PhysicsColliderComponent& c) { return first_collider(c).shape.angle * degrees_per_radian; },
                               [=](PhysicsColliderComponent& c, const f32& degrees) {
                                   first_collider(c).shape.angle = degrees / degrees_per_radian;
                                   return true;
                               })
            .units("degrees")
            .description("It turned about its centre.");
        collider_field<std::vector<Vec2f>>(collider, "points", [](auto& def) -> auto& { return def.shape.points; })
            .description("A polygon's corners, a chain's points, in order.");
        collider_field<bool>(collider, "loop", [](auto& def) -> auto& { return def.shape.loop; })
            .description("A chain's: back from its last point to its first.");
        collider_field<bool>(collider, "one_sided", [](auto& def) -> auto& { return def.shape.one_sided; })
            .description("A chain's: it stops what comes from the left of it going along it only (a platform to jump up through, drawn left to right).");
        collider_field<f32>(collider, "density", [](auto& def) -> auto& { return def.density; })
            .units("kg/m^2");
        collider_field<f32>(collider, "friction", [](auto& def) -> auto& { return def.friction; });
        collider_field<f32>(collider, "restitution", [](auto& def) -> auto& { return def.restitution; })
            .description("How it bounces: 0 not at all, 1 back as fast.");
        collider_field<bool>(collider, "sensor", [](auto& def) -> auto& { return def.sensor; })
            .description("It says what it overlaps, and stops nothing.");
        collider.property<u32>("category_bits", [](const PhysicsColliderComponent& c) { return u32{first_collider(c).category_bits}; },
                               [](PhysicsColliderComponent& c, const u32& bits) {
                                   if (bits > 0xFFFFu) return false;
                                   first_collider(c).category_bits = static_cast<u16>(bits);
                                   return true;
                               })
            .description("The kinds it is (bits).");
        collider.property<u32>("mask_bits", [](const PhysicsColliderComponent& c) { return u32{first_collider(c).mask_bits}; },
                               [](PhysicsColliderComponent& c, const u32& bits) {
                                   if (bits > 0xFFFFu) return false;
                                   first_collider(c).mask_bits = static_cast<u16>(bits);
                                   return true;
                               })
            .description("The kinds it hits (bits).");
    }
    if (!registry.find("PhysicsJointComponent")) {
        // The entity's body joined to another entity's, or to the world.
        static constexpr std::pair<PhysicsJointType, const char*> types[] = {
            {PhysicsJointType::Hinge, "hinge"}, {PhysicsJointType::Slider, "slider"}, {PhysicsJointType::Distance, "distance"},
            {PhysicsJointType::Weld, "weld"},   {PhysicsJointType::Wheel, "wheel"},
        };
        registry.native<PhysicsJointComponent>("PhysicsJointComponent")
            .property<std::string>("type",
                                   [](const PhysicsJointComponent& joint) {
                                       for (const auto& [type, name] : types) {
                                           if (type == joint.type) return std::string(name);
                                       }
                                       return std::string("hinge");
                                   },
                                   [](PhysicsJointComponent& joint, const std::string& name) {
                                       for (const auto& [type, type_name] : types) {
                                           if (name == type_name) {
                                               joint.type = type;
                                               return true;
                                           }
                                       }
                                       return false;
                                   })
            .choices({"hinge", "slider", "distance", "weld", "wheel"})
            .description("hinge: they turn about the anchor; slider: it slides along the axis; distance: the anchors kept apart (a rod, a rope, a spring); weld: held as they are; wheel: a hinge on a spring along the axis.")
            .field("other", &PhysicsJointComponent::other)
            .description("The entity (with a body) it is joined to; none: to where it is in the world.")
            .field("anchor", &PhysicsJointComponent::anchor)
            .description("Where they are joined, from this entity.")
            .field("other_anchor", &PhysicsJointComponent::other_anchor)
            .description("A distance joint's point on the other entity, from it.")
            .field("axis", &PhysicsJointComponent::axis)
            .description("A slider's or a wheel's: what it moves along (this entity's).")
            .field("collide_connected", &PhysicsJointComponent::collide_connected)
            .description("The two still collide with each other.")
            .field("limit", &PhysicsJointComponent::limit)
            .description("Keep it between lower and upper: a hinge's angle, a slider's or a wheel's travel, a distance joint's length (a rope: 0 to its length).")
            .field("lower", &PhysicsJointComponent::lower)
            .description("A hinge's in degrees, the others' in units.")
            .field("upper", &PhysicsJointComponent::upper)
            .description("A hinge's in degrees, the others' in units.")
            .field("motor", &PhysicsJointComponent::motor)
            .description("It drives itself: a hinge or a wheel turns, a slider slides.")
            .field("motor_speed", &PhysicsJointComponent::motor_speed)
            .description("A hinge's or a wheel's in degrees/s; a slider's in units/s.")
            .field("max_motor_force", &PhysicsJointComponent::max_motor_force)
            .description("How hard the motor may drive (a torque, for turning).")
            .field("length", &PhysicsJointComponent::length)
            .units("units")
            .description("A distance joint's; 0: how far apart its anchors are when it is made.")
            .field("frequency", &PhysicsJointComponent::frequency)
            .units("Hz")
            .range(0.0, 60.0)
            .description("A spring (a distance joint's, a wheel's, a weld's): how often it swings; 0: none, it is stiff.")
            .field("damping_ratio", &PhysicsJointComponent::damping_ratio)
            .range(0.0, 2.0)
            .description("How soon the spring's swinging dies (1: at once).")
            .field("break_force", &PhysicsJointComponent::break_force)
            .description("It breaks when it pulls harder than this; 0: never.")
            .field("broken", &PhysicsJointComponent::broken)
            .description("It broke (it is made again when this is cleared).");
    }
    if (!registry.find("TilemapCollider")) {
        // A tilemap's solid cells as a static body.
        registry.native<TilemapCollider>("TilemapCollider")
            .field("layer", &TilemapCollider::layer)
            .description("Only this layer's cells; empty: every layer that takes part in collision.")
            .field("friction", &TilemapCollider::friction)
            .field("restitution", &TilemapCollider::restitution)
            .description("How it bounces: 0 not at all, 1 back as fast.")
            .field("sensor", &TilemapCollider::sensor)
            .description("It says what it overlaps, and stops nothing.")
            .property<u32>("category_bits", [](const TilemapCollider& c) { return u32{c.category_bits}; },
                           [](TilemapCollider& c, const u32& bits) {
                               if (bits > 0xFFFFu) return false;
                               c.category_bits = static_cast<u16>(bits);
                               c.built_from.reset(); // made again
                               return true;
                           })
            .description("The kinds it is (bits).")
            .property<u32>("mask_bits", [](const TilemapCollider& c) { return u32{c.mask_bits}; },
                           [](TilemapCollider& c, const u32& bits) {
                               if (bits > 0xFFFFu) return false;
                               c.mask_bits = static_cast<u16>(bits);
                               c.built_from.reset();
                               return true;
                           })
            .description("The kinds it hits (bits).");
    }
    if (!registry.find("PhysicsVelocity")) {
        registry.native<PhysicsVelocity>("PhysicsVelocity")
            .field("linear", &PhysicsVelocity::linear)
            .units("units/s")
            .description("Given to the body before each step.");
    }
    // The contacts of an entity's body, each step (for its game to read).
    if (!registry.find("PhysicsContactQueue")) {
        registry.native<PhysicsContactQueue>("PhysicsContactQueue");
    }
    if (!registry.find("PhysicsOverlapSet")) {
        registry.native<PhysicsOverlapSet>("PhysicsOverlapSet");
    }
}
#endif

} // namespace

void register_builtin_editor_components(EcsWorld& world) {
#if defined(KIN_EXPERIMENTAL_3D) && KIN_ENABLE_PHYSICS3D
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
        auto rect = world.components().native<RectRenderer>("RectRenderer");
        rect.field("offset", &RectRenderer::offset)
            .field("size", &RectRenderer::size)
            .field("color", &RectRenderer::color)
            .field("layer", &RectRenderer::layer)
            .field("order", &RectRenderer::order)
            .field("visible", &RectRenderer::visible);
        describe_blend(rect);
    }
    if (!world.components().find("TextureRenderer")) {
        // An image of the game's content, by its path (Assets2D binds it).
        auto texture = world.components().native<TextureRenderer>("TextureRenderer");
        texture.field("asset", &TextureRenderer::asset)
            .asset_type(std::string{asset_type_name(AssetType::Image)})
            .field("offset", &TextureRenderer::offset)
            .field("size", &TextureRenderer::size)
            .field("pivot", &TextureRenderer::pivot)
            .field("tint", &TextureRenderer::tint)
            .field("layer", &TextureRenderer::layer)
            .field("order", &TextureRenderer::order)
            .field("visible", &TextureRenderer::visible)
            .field("flip_x", &TextureRenderer::flip_x)
            .field("flip_y", &TextureRenderer::flip_y);
        describe_blend(texture);
    }
    if (!world.components().find("SpriteRenderer")) {
        // A sprite of a catalog of the game's content, by the catalog's path and
        // the sprite's id (Assets2D binds the catalog).
        auto sprite = world.components().native<SpriteRenderer>("SpriteRenderer");
        sprite.field("asset", &SpriteRenderer::asset)
            .asset_type(std::string{asset_type_name(AssetType::SpriteCatalog)})
            .property<std::string>("sprite", [](const SpriteRenderer& r) { return r.sprite.id; },
                                   [](SpriteRenderer& r, const std::string& id) {
                                       r.sprite.id = id;
                                       return true;
                                   })
            .entry_of("asset")
            .field("offset", &SpriteRenderer::offset)
            .field("size", &SpriteRenderer::size)
            .description("Zero: the sprite's own size.")
            .field("pivot", &SpriteRenderer::pivot)
            .description("A fraction of the sprite; below zero: the catalog's pivot.")
            .field("tint", &SpriteRenderer::tint)
            .field("rotation", &SpriteRenderer::rotation)
            .units("degrees")
            .field("layer", &SpriteRenderer::layer)
            .field("order", &SpriteRenderer::order)
            .field("y_sort", &SpriteRenderer::y_sort)
            .field("visible", &SpriteRenderer::visible)
            .field("flip_x", &SpriteRenderer::flip_x)
            .field("flip_y", &SpriteRenderer::flip_y);
        describe_blend(sprite);
    }
    if (!world.components().find("SpriteAnimator")) {
        // An animation of its SpriteRenderer's catalog played on it
        // (update_sprite_animators sets the renderer's sprite).
        world.components().native<SpriteAnimator>("SpriteAnimator")
            .field("animation", &SpriteAnimator::animation)
            .entry_of("SpriteRenderer.asset")
            .description("An animation of its SpriteRenderer's catalog.")
            .field("speed", &SpriteAnimator::speed)
            .range(0.0, 100.0)
            .description("1: as the animation says; 2: twice as fast.")
            .field("playing", &SpriteAnimator::playing)
            .property<i32>("frame", [](const SpriteAnimator& a) { return a.frame; },
                           [](SpriteAnimator&, const i32&) { return true; }) // as it is: nothing to set
            .readonly()
            .description("The frame of the animation shown now (-1: none).")
            .property<bool>("finished", [](const SpriteAnimator& a) { return a.finished; },
                            [](SpriteAnimator&, const bool&) { return true; })
            .readonly()
            .description("A Once animation has played through.");
    }
#if KIN_ENABLE_AUDIO
    if (!world.components().find("AudioEmitter")) {
        // A sound of the game's content (a file, or a cue of an audio catalog)
        // playing from the entity (Assets2D binds it; update_audio_emitters plays it).
        world.components().native<AudioEmitter>("AudioEmitter")
            .field("asset", &AudioEmitter::asset)
            .asset_type(std::string{asset_type_name(AssetType::Audio)} + "," + std::string{asset_type_name(AssetType::AudioCatalog)})
            .field("cue", &AudioEmitter::cue)
            .entry_of("asset")
            .description("A cue of the catalog; empty for a sound file.")
            .field("auto_start", &AudioEmitter::auto_start)
            .field("volume", &AudioEmitter::volume)
            .range(0.0, 4.0)
            .field("pitch", &AudioEmitter::pitch)
            .range(0.01, 8.0)
            .field("paused", &AudioEmitter::paused)
            .field("fade_in", &AudioEmitter::fade_in)
            .units("seconds")
            .property<std::string>("when_done",
                                   [](const AudioEmitter& e) {
                                       switch (e.when_done) {
                                       case AudioEmitterEnd::Restart: return std::string("restart");
                                       case AudioEmitterEnd::Keep: return std::string("keep");
                                       case AudioEmitterEnd::Remove: return std::string("remove");
                                       case AudioEmitterEnd::Despawn: return std::string("despawn");
                                       }
                                       return std::string("restart");
                                   },
                                   [](AudioEmitter& e, const std::string& value) {
                                       if (value == "restart") e.when_done = AudioEmitterEnd::Restart;
                                       else if (value == "keep") e.when_done = AudioEmitterEnd::Keep;
                                       else if (value == "remove") e.when_done = AudioEmitterEnd::Remove;
                                       else if (value == "despawn") e.when_done = AudioEmitterEnd::Despawn;
                                       else return false;
                                       return true;
                                   })
            .choices({"restart", "keep", "remove", "despawn"})
            .description("What it does once its sound ends.")
            .field("offset_3d", &AudioEmitter::offset_3d);
    }
#endif
    if (!world.components().find("LineRenderer")) {
        world.components().native<LineRenderer>("LineRenderer")
            .field("a", &LineRenderer::a)
            .field("b", &LineRenderer::b)
            .field("color", &LineRenderer::color)
            .field("layer", &LineRenderer::layer)
            .field("order", &LineRenderer::order)
            .field("visible", &LineRenderer::visible);
    }
#if KIN_ENABLE_PHYSICS
    register_physics2d_editor_components(world);
#endif
    if (!world.components().find("ParticleSystemComponent")) {
        // Particles: an effect of a particle catalog of the game's content,
        // burst where the entity is (update_particles; Assets2D binds the catalog).
        auto particles = world.components().native<ParticleSystemComponent>("ParticleSystemComponent");
        particles.field("asset", &ParticleSystemComponent::asset)
            .asset_type(std::string{asset_type_name(AssetType::Particles)})
            .field("effect", &ParticleSystemComponent::effect)
            .entry_of("asset")
            .description("An effect of the catalog; empty: the game emits.")
            .field("rate", &ParticleSystemComponent::rate)
            .units("bursts/s")
            .range(0.0, 1000.0)
            .description("How often it bursts; an effect that streams streams at its own rate instead.")
            .field("emitting", &ParticleSystemComponent::emitting)
            .description("It bursts at its rate, or its effect streams.")
            .field("burst_at_start", &ParticleSystemComponent::burst_at_start)
            .description("One burst as it starts.")
            .field("offset", &ParticleSystemComponent::offset)
            .description("Where it bursts, from the entity.")
            .field("sprites", &ParticleSystemComponent::sprites_asset)
            .asset_type(std::string{asset_type_name(AssetType::SpriteCatalog)})
            .description("The sprite catalog its effects' sprites are of.")
            .property<u32>("active", [](const ParticleSystemComponent& p) { return static_cast<u32>(p.system.active_count()); },
                           [](ParticleSystemComponent&, const u32&) { return true; }) // as it is: nothing to set
            .readonly()
            .description("Its particles alive now.");
        describe_blend(particles);
    }
    if (!world.components().find("ParticleFieldComponent")) {
        // A field of particles (snow, stars) of a particle catalog, its area
        // from where the entity is (Assets2D makes it).
        auto field = world.components().native<ParticleFieldComponent>("ParticleFieldComponent");
        field.field("asset", &ParticleFieldComponent::asset)
            .asset_type(std::string{asset_type_name(AssetType::Particles)})
            .field("field", &ParticleFieldComponent::name)
            .entry_of("asset")
            .description("A field of the catalog.")
            .field("sprites", &ParticleFieldComponent::sprites_asset)
            .asset_type(std::string{asset_type_name(AssetType::SpriteCatalog)})
            .description("The sprite catalog its sprites are of.");
        describe_blend(field);
    }
    if (!world.components().find("TilemapComponent")) {
        // A tilemap file (square cells or hexagons) drawn from where the entity
        // is (Assets2D reads it, its tileset and the tileset's image).
        world.components().native<TilemapComponent>("TilemapComponent")
            .field("asset", &TilemapComponent::asset)
            .asset_type(std::string{asset_type_name(AssetType::TileMap)})
            .field("layer", &TilemapComponent::layer)
            .field("order", &TilemapComponent::order)
            .description("Its layers' orders come after this.")
            .field("visible", &TilemapComponent::visible)
            .property<u32>("cells",
                           [](const TilemapComponent& t) { return t.tilemap ? static_cast<u32>(t.tilemap->map.cols * t.tilemap->map.rows) : 0u; },
                           [](TilemapComponent&, const u32&) { return true; }) // as it is: nothing to set
            .readonly()
            .description("Its map's cells, once read (0: not read).");
    }
#if KIN_ENABLE_SCRIPTING
    if (!world.components().find("ScriptComponent")) {
        // A Lua script of the game's content run for the entity (EntityScripts:
        // on_start, on_update, on_contact; kin/scripting/entity_scripts.hpp).
        world.components().native<ScriptComponent>("ScriptComponent")
            .field("asset", &ScriptComponent::asset)
            .asset_type(std::string{asset_type_name(AssetType::Script)})
            .description("A .lua file of the content.")
            .field("enabled", &ScriptComponent::enabled);
    }
#endif
    if (!world.components().find("Animator")) {
        // An animation of an animation file of the game's content played on
        // the entity: its tracks set its components' fields (update_animators).
        world.components().native<Animator>("Animator")
            .field("asset", &Animator::asset)
            .asset_type(std::string{asset_type_name(AssetType::Animation)})
            .field("animation", &Animator::animation)
            .entry_of("asset")
            .description("An animation of the file.")
            .field("machine", &Animator::machine)
            .entry_of("asset")
            .description("A state machine of the file: it plays in place of the animation.")
            .field("speed", &Animator::speed)
            .range(0.0, 100.0)
            .description("1: as the animation says; 2: twice as fast.")
            .field("playing", &Animator::playing)
            .property<f32>("time", [](const Animator& a) { return a.time; }, [](Animator&, const f32&) { return true; })
            .readonly()
            .units("seconds")
            .description("Into the animation now.")
            .property<bool>("finished", [](const Animator& a) { return a.finished; }, [](Animator&, const bool&) { return true; })
            .readonly()
            .description("One that ends has played through.")
            .property<std::string>("state", [](const Animator& a) { return a.state; }, [](Animator&, const std::string&) { return true; })
            .readonly()
            .description("The machine's state now.");
    }
    if (!world.components().find("TalkComponent")) {
        // A character the player talks to: its dialogue starts when the
        // Talker, near it, presses the talk action (update_talk).
        world.components().native<TalkComponent>("TalkComponent")
            .field("asset", &TalkComponent::asset)
            .asset_type(std::string{asset_type_name(AssetType::Dialogue)})
            .description("A .kindialogue of the content.")
            .field("start", &TalkComponent::start)
            .description("The node it starts at; empty: the file's start.")
            .field("radius", &TalkComponent::radius)
            .range(0.0, 100000.0)
            .description("How near the Talker must be.")
            .field("prompt", &TalkComponent::prompt)
            .field("enabled", &TalkComponent::enabled);
    }
    if (!world.components().find("Talker")) {
        // The one who talks (the player): TalkComponents are measured from it.
        world.components().native<Talker>("Talker").field("enabled", &Talker::enabled);
    }
    if (!world.components().find("SceneDoor")) {
        // A way to another scene, or another place of this one: a
        // SceneTraveller stepping into its area (or pressing its action in
        // it) goes there (SceneFlow, kin/ecs/scene_flow.hpp).
        world.components().native<SceneDoor>("SceneDoor")
            .field("scene", &SceneDoor::scene)
            .asset_type(std::string{asset_type_name(AssetType::Scene)})
            .description("The scene it leads to; none: a place of this one.")
            .field("arrive", &SceneDoor::arrive)
            .entry_of("scene")
            .description("The entity there travellers arrive at, by its name (another door, a marker).")
            .property<std::string>("trigger",
                                   [](const SceneDoor& d) { return std::string(d.trigger == DoorTrigger::Action ? "action" : "touch"); },
                                   [](SceneDoor& d, const std::string& value) {
                                       if (value == "touch") d.trigger = DoorTrigger::Touch;
                                       else if (value == "action") d.trigger = DoorTrigger::Action;
                                       else return false;
                                       return true;
                                   })
            .choices({"touch", "action"})
            .description("Touch: stepping into it; action: pressing its action in it.")
            .field("action", &SceneDoor::action)
            .description("An action door's (an action of input.kininput).")
            .field("size", &SceneDoor::size)
            .description("Its area, centred on the entity.")
            .field("offset", &SceneDoor::offset)
            .description("The area's centre, from the entity.")
            .field("exit", &SceneDoor::exit)
            .description("Where those arriving at this door stand, from it (out of its area).")
            .property<std::string>("transition",
                                   [](const SceneDoor& d) {
                                       return d.transition ? std::string(transition_name(*d.transition)) : std::string("none");
                                   },
                                   [](SceneDoor& d, const std::string& value) {
                                       if (value == "none") {
                                           d.transition.reset();
                                           return true;
                                       }
                                       const std::optional<TransitionKind> kind = transition_from_name(value);
                                       if (!kind) return false;
                                       d.transition = kind;
                                       return true;
                                   })
            .choices({"fade", "crossfade", "dissolve", "pixelate", "wipe", "iris", "none"})
            .field("duration", &SceneDoor::duration)
            .range(0.0, 10.0)
            .units("seconds")
            .field("color", &SceneDoor::color)
            .description("What the screen goes to between the scenes.")
            .field("prompt", &SceneDoor::prompt)
            .description("What the game shows by an action door.")
            .field("enabled", &SceneDoor::enabled);
    }
    if (!world.components().find("SceneTraveller")) {
        // Who goes through SceneDoors (the player): where it is, is what is in a door.
        world.components().native<SceneTraveller>("SceneTraveller").field("enabled", &SceneTraveller::enabled);
    }
    if (!world.components().find("UiScreenComponent")) {
        // A UI screen (.kinui) shown over the world while the entity is there
        // (SceneScreens draws it; kin/ecs/ui_screens.hpp).
        world.components().native<UiScreenComponent>("UiScreenComponent")
            .field("asset", &UiScreenComponent::asset)
            .asset_type(std::string{asset_type_name(AssetType::UiScreen)})
            .description("A .kinui screen of the content.")
            .field("visible", &UiScreenComponent::visible)
            .field("order", &UiScreenComponent::order)
            .description("Higher draws over lower.");
    }
    if (!world.components().find("LightComponent")) {
        // A light where its entity is: it brightens the world when the scene
        // has a SceneLighting (kin/ecs/lighting.hpp).
        world.components().native<LightComponent>("LightComponent")
            .field("color", &LightComponent::color)
            .field("radius", &LightComponent::radius)
            .range(0.0, 100000.0)
            .description("Nothing further is lit; scaled with its entity.")
            .field("intensity", &LightComponent::intensity)
            .range(0.0, 4.0)
            .description("1 adds its colour in full at the centre.")
            .field("shape", &LightComponent::shape_asset)
            .asset_type(std::string{asset_type_name(AssetType::Image)})
            .description("An image whose alpha shapes it (a cone, a window); none: round.")
            .field("rotation", &LightComponent::rotation)
            .units("degrees")
            .field("enabled", &LightComponent::enabled);
    }
    if (!world.components().find("SceneLighting")) {
        // The scene darkened to an ambient light and lit by its lights (the
        // first one found is the scene's).
        world.components().native<SceneLighting>("SceneLighting")
            .field("enabled", &SceneLighting::enabled)
            .field("ambient", &SceneLighting::ambient)
            .description("What is not lit looks like this much light: black is dark, white unlit.")
            .field("resolution", &SceneLighting::resolution)
            .range(0.05, 1.0)
            .description("Light-map pixels per screen point (lights are smooth: half is plenty).");
    }
    if (!world.components().find("ColorGrade")) {
        // The game's colour output while it is in the scene: exposure and
        // tonemapping (a linear game's), a look, dithering.
        world.components().native<ColorGrade>("ColorGrade")
            .field("enabled", &ColorGrade::enabled)
            .field("exposure", &ColorGrade::exposure)
            .range(0.0, 16.0)
            .description("Scales the light (a linear game's).")
            .property<std::string>("tonemap", [](const ColorGrade& g) { return std::string(tonemap_name(g.tonemap)); },
                                   [](ColorGrade& g, const std::string& name) {
                                       const std::optional<Tonemap> tonemap = tonemap_named(name.empty() ? "none" : name);
                                       if (!tonemap) return false;
                                       g.tonemap = *tonemap;
                                       return true;
                                   })
            .choices({"none", "reinhard", "aces"})
            .description("How light above white comes into range (a linear game's).")
            .field("look", &ColorGrade::lut_asset)
            .asset_type(std::string{asset_type_name(AssetType::ColorLut)} + "," + std::string{asset_type_name(AssetType::Image)})
            .description("A .cube file or a LUT image the colours are graded through.")
            .field("strength", &ColorGrade::strength)
            .range(0.0, 1.0)
            .description("How much of the look.")
            .field("dither", &ColorGrade::dither)
            .description("Noise of half a step, so dark gradients do not band.");
    }
    if (!world.components().find("ShaderEffect")) {
        // A fragment shader of the content drawn over a box at the entity
        // (kin/ecs/shader_effect.hpp).
        auto effect = world.components().native<ShaderEffect>("ShaderEffect");
        effect.field("asset", &ShaderEffect::asset)
            .asset_type(std::string{asset_type_name(AssetType::Shader)})
            .description("A .frag.glsl file of the content.")
            .field("texture", &ShaderEffect::texture_asset)
            .asset_type(std::string{asset_type_name(AssetType::Image)})
            .description("An image it samples (sampler 0).")
            .field("size", &ShaderEffect::size)
            .description("The box it covers, centred on the entity.")
            .field("offset", &ShaderEffect::offset)
            .field("params", &ShaderEffect::params)
            .description("Its uniforms' values by name: \"speed=1.5 tint=1,0.8,0.6,1\".")
            .field("layer", &ShaderEffect::layer)
            .field("order", &ShaderEffect::order)
            .field("y_sort", &ShaderEffect::y_sort)
            .field("visible", &ShaderEffect::visible);
        describe_blend(effect);
    }
    if (!world.components().find("CameraComponent")) {
        // What the game sees of its world: EcsScene draws through the first
        // current one, looking at where its entity is (kin/ecs/camera.hpp).
        world.components().native<CameraComponent>("CameraComponent")
            .field("current", &CameraComponent::current)
            .description("The one the game is drawn through (the first current one).")
            .field("zoom", &CameraComponent::zoom)
            .range(0.01, 100.0)
            .description("2: the world twice as big.")
            .field("smoothing", &CameraComponent::smoothing)
            .units("seconds")
            .range(0.0, 10.0)
            .description("How long it takes to catch its entity up (update_cameras); 0: at once.")
            .field("limited", &CameraComponent::limited)
            .description("It shows nothing past its limits.")
            .field("limit_min", &CameraComponent::limit_min)
            .description("The world's top-left corner it shows no further than.")
            .field("limit_max", &CameraComponent::limit_max)
            .description("And its bottom-right.");
    }
    if (!world.components().find("ShapeRenderer")) {
        // A vector shape at the entity: a box, a rounded box, an ellipse, a
        // regular polygon, or an SVG file (Assets2D makes its mesh).
        auto shape = world.components().native<ShapeRenderer>("ShapeRenderer");
        shape.property<std::string>("shape", [](const ShapeRenderer& r) { return std::string(shape_kind_name(r.spec.kind)); },
                                   [](ShapeRenderer& r, const std::string& name) {
                                       const std::optional<ShapeKind> kind = shape_kind_named(name);
                                       if (!kind) return false;
                                       r.spec.kind = *kind;
                                       return true;
                                   })
            .choices({"none", "rect", "rounded_rect", "ellipse", "polygon", "svg"})
            .description("none: the game sets its mesh; svg: the file `asset` names.")
            .field("asset", &ShapeRenderer::asset)
            .asset_type(std::string{asset_type_name(AssetType::Shape)})
            .description("An SVG file of the content (shape svg).")
            .field("size", &ShapeRenderer::spec, &ShapeSpec::size)
            .description("Centred on the entity; an SVG is fitted in it (zero: its own size).")
            .field("radius", &ShapeRenderer::spec, &ShapeSpec::radius)
            .description("A rounded box's corners.")
            .field("sides", &ShapeRenderer::spec, &ShapeSpec::sides)
            .range(3.0, 256.0)
            .description("A polygon's.")
            .field("fill", &ShapeRenderer::spec, &ShapeSpec::fill)
            .field("stroke", &ShapeRenderer::spec, &ShapeSpec::stroke)
            .description("Its outline's colour.")
            .field("stroke_width", &ShapeRenderer::spec, &ShapeSpec::stroke_width)
            .range(0.0, 1000.0)
            .field("offset", &ShapeRenderer::offset)
            .field("tint", &ShapeRenderer::tint)
            .description("Multiplies its colours (an SVG's too).")
            .field("layer", &ShapeRenderer::layer)
            .field("order", &ShapeRenderer::order)
            .field("y_sort", &ShapeRenderer::y_sort)
            .field("visible", &ShapeRenderer::visible);
        describe_blend(shape);
    }
    if (!world.components().find("ClipGroup")) {
        // Its entity's renderers, and those of every entity under it, cut to a
        // shape that moves, turns and scales with it.
        const auto clip_field = [](auto member) {
            return [member](ClipGroup& group, const auto& value) {
                group.*member = value;
                reshape_clip(group);
                return true;
            };
        };
        world.components().native<ClipGroup>("ClipGroup")
            .property<std::string>("shape",
                                   [](const ClipGroup& g) {
                                       switch (g.shape) {
                                       case ClipShape::None: return std::string("none");
                                       case ClipShape::Rect: return std::string("rect");
                                       case ClipShape::RoundedRect: return std::string("rounded_rect");
                                       case ClipShape::Ellipse: return std::string("ellipse");
                                       }
                                       return std::string("none");
                                   },
                                   [](ClipGroup& g, const std::string& name) {
                                       if (name == "rect") g.shape = ClipShape::Rect;
                                       else if (name == "rounded_rect") g.shape = ClipShape::RoundedRect;
                                       else if (name == "ellipse") g.shape = ClipShape::Ellipse;
                                       else if (name == "none" || name.empty()) g.shape = ClipShape::None;
                                       else return false;
                                       reshape_clip(g);
                                       return true;
                                   })
            .choices({"none", "rect", "rounded_rect", "ellipse"})
            .description("What shows of them: what is inside it (none: the game sets its clip).")
            .property<Vec2f>("size", [](const ClipGroup& g) { return g.size; }, clip_field(&ClipGroup::size))
            .property<Vec2f>("offset", [](const ClipGroup& g) { return g.offset; }, clip_field(&ClipGroup::offset))
            .description("Its centre, from the entity.")
            .property<f32>("radius", [](const ClipGroup& g) { return g.radius; }, clip_field(&ClipGroup::radius))
            .description("A rounded box's corners.")
            .field("layer", &ClipGroup::layer)
            .description("They are drawn together, at its layer and order.")
            .field("order", &ClipGroup::order)
            .field("y_sort", &ClipGroup::y_sort);
    }
    if (!world.components().find("TextRenderer")) {
        // Text in the world at the entity, in a font of the content (Assets2D
        // reads it) or the system's.
        world.components().native<TextRenderer>("TextRenderer")
            .field("text", &TextRenderer::text)
            .field("key", &TextRenderer::key)
            .description("A translation key; empty: scene.<scene>.<entity>, where a language has it.")
            .field("font", &TextRenderer::font)
            .asset_type(std::string{asset_type_name(AssetType::Font)})
            .description("A font file of the content; empty: the system's.")
            .field("size", &TextRenderer::size)
            .units("points")
            .range(1.0, 512.0)
            .field("color", &TextRenderer::color)
            .field("offset", &TextRenderer::offset)
            .field("pivot", &TextRenderer::pivot)
            .description("The point of its box at the entity: 0.5, 0.5 its centre.")
            .field("wrap", &TextRenderer::wrap)
            .description("Lines break past this wide (0: only at line ends).")
            .property<std::string>("align", [](const TextRenderer& t) { return std::string(text_align_name(t.align)); },
                                   [](TextRenderer& t, const std::string& name) {
                                       const std::optional<TextAlign> align = text_align_named(name);
                                       if (!align) return false;
                                       t.align = *align;
                                       return true;
                                   })
            .choices({"left", "center", "right"})
            .field("outline", &TextRenderer::outline)
            .range(0.0, 64.0)
            .description("Points round each letter.")
            .field("outline_color", &TextRenderer::outline_color)
            .field("layer", &TextRenderer::layer)
            .field("order", &TextRenderer::order)
            .field("y_sort", &TextRenderer::y_sort)
            .field("visible", &TextRenderer::visible);
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
                                      EcsComponentRegistry& components,PrefabAssetLoadOptions limits,SceneInstantiateOptions options) {
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
        // Its text translated under its own key (scene.<scene>.<entity>), where a language has it.
        if (const TextRenderer* text = runtime.get<TextRenderer>(); text && text->auto_key.empty()) {
            runtime.get_mut<TextRenderer>()->auto_key = scene_text_key(scene.name, entity.id);
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

    std::unordered_set<EcsId> managed;
    for (const auto& [id, entity] : instance.by_authored_id) managed.insert(entity.id());
    for (const auto& [id, entity] : instance.by_authored_id)
        if (!entity.parent() || !managed.contains(entity.parent().id())) instance.roots.push_back(entity);
    std::ranges::sort(instance.roots, {}, &EcsEntity::id);
    if (options.update_scene_header) apply_scene_header(world, scene);
    rollback.committed = true;
    return instance;
}

void apply_scene_header(EcsWorld& world, const SceneAsset& scene) {
    world.raw().entity("kin::prefab::scene_header").set(SceneHeader{scene.name, scene.script, scene.clear_color});
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
