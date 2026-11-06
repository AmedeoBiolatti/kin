#include "scene_components_internal.hpp"

#include <kin/ecs/physics.hpp>

#include <string>
#include <utility>
#include <vector>

namespace kin {
namespace {

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

} // namespace
namespace scene_detail {

void register_scene_physics2d_components(EcsWorld& world) {
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
            .description("A platform to jump up through: it stops only what comes down onto its top (a chain's top: the left of it going along it, drawn left to right).");
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
    if (!registry.find("CharacterController2D")) {
        // A character its game moves: it goes along `velocity` as far as it
        // can, sliding along what stops it, its body made for it.
        registry.native<CharacterController2D>("CharacterController2D")
            .field("velocity", &CharacterController2D::velocity)
            .units("units/s")
            .description("Where the game sends it (a script sets it); after each step, what is left of it (landing stops its fall).")
            .field("gravity", &CharacterController2D::gravity)
            .units("units/s^2")
            .description("Its own, added to its velocity along -up while it is not on a floor.")
            .field("up", &CharacterController2D::up)
            .description("Which way is up for it (on screen, y down: 0, -1).")
            .field("max_slope", &CharacterController2D::max_slope)
            .units("degrees")
            .range(0.0, 85.0)
            .description("A floor is at most this steep; steeper is a wall.")
            .field("snap", &CharacterController2D::snap)
            .units("units")
            .description("Kept on a floor going down slopes and steps this far; 0: it goes off them, falling.")
            .field("stop_on_slope", &CharacterController2D::stop_on_slope)
            .description("Standing on a slope, it doesn't slide down it.")
            .field("push", &CharacterController2D::push)
            .range(0.0, 4.0)
            .description("Dynamic bodies it walks into are pushed on at its speed times this; 0: they stop it.")
            .field("drop_through", &CharacterController2D::drop_through)
            .units("s")
            .description("Seconds in which one-sided platforms let it through (set it to drop off one).")
            .field("on_floor", &CharacterController2D::on_floor)
            .readonly()
            .description("It stands on a floor.")
            .field("on_wall", &CharacterController2D::on_wall)
            .readonly()
            .description("It ran into a wall in the last step.")
            .field("on_ceiling", &CharacterController2D::on_ceiling)
            .readonly()
            .description("It ran into a ceiling in the last step.")
            .field("floor_normal", &CharacterController2D::floor_normal)
            .readonly()
            .description("Which way its floor faces.")
            .field("wall_normal", &CharacterController2D::wall_normal)
            .readonly()
            .description("Which way the wall it ran into faces.");
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

} // namespace scene_detail
} // namespace kin
