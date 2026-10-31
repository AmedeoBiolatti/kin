// Bodies of several shapes (colliders on child entities), joints between
// entities, tilemaps' solid cells as bodies, and all of them in scene files.

#include <kin/ecs/physics.hpp>
#include <kin/ecs/tilemap.hpp>
#include <kin/scene/scene_asset.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <sstream>
#include <string>
#include <vector>

using kin::f32;
using kin::PhysicsBodyType;
using kin::PhysicsShape;
using kin::Vec2f;

namespace {

constexpr f32 dt = 1.0f / 60.0f;

bool near(f32 a, f32 b, f32 epsilon = 0.05f) {
    return std::fabs(a - b) <= epsilon;
}

bool near(Vec2f a, Vec2f b, f32 epsilon = 0.05f) {
    return near(a.x, b.x, epsilon) && near(a.y, b.y, epsilon);
}

kin::PhysicsColliderComponent one(PhysicsShape shape) {
    return {.colliders = {{.def = {.shape = std::move(shape)}}}};
}

kin::EcsEntity body(kin::EcsWorld& world, const char* name, Vec2f at, PhysicsBodyType type = PhysicsBodyType::Dynamic) {
    return world.entity(name).set(kin::Transform2D{.pos = at}).set(kin::PhysicsBodyComponent{.def = {.type = type}});
}

// Each debug shape's centre (a polygon's: its corners' middle).
std::vector<Vec2f> centres(const kin::PhysicsWorld& physics) {
    std::vector<Vec2f> found;
    for (const kin::PhysicsDebugShape& shape : physics.debug_shapes()) {
        if (shape.type == kin::PhysicsDebugShapeType::Circle) {
            found.push_back(shape.center);
            continue;
        }
        Vec2f sum{};
        for (Vec2f point : shape.points) {
            sum = {sum.x + point.x, sum.y + point.y};
        }
        found.push_back({sum.x / static_cast<f32>(shape.points.size()), sum.y / static_cast<f32>(shape.points.size())});
    }
    return found;
}

bool has_centre(const kin::PhysicsWorld& physics, Vec2f at) {
    return std::ranges::any_of(centres(physics), [&](Vec2f centre) { return near(centre, at, 0.1f); });
}

void test_child_colliders() {
    kin::EcsWorld world;
    kin::register_physics_components(world);
    world.component<kin::Transform2D>("Transform2D");
    kin::PhysicsWorld physics;
    kin::EcsPhysicsBridge bridge{world, physics};

    // A cart turned a quarter: a box body of its own, a wheel either side.
    kin::EcsEntity cart = body(world, "cart", {100.0f, 0.0f}, PhysicsBodyType::Kinematic);
    cart.set(kin::Transform2D{.pos = {100.0f, 0.0f}, .rotation = 90.0f}).set(one(PhysicsShape::box({20.0f, 5.0f})));
    kin::EcsEntity front = world.entity("front").set(kin::Transform2D{.pos = {20.0f, 0.0f}}).set(one(PhysicsShape::circle(4.0f)));
    kin::EcsEntity back = world.entity("back").set(kin::Transform2D{.pos = {-20.0f, 0.0f}, .scale = {2.0f, 2.0f}}).set(one(PhysicsShape::circle(4.0f)));
    front.child_of(cart);
    back.child_of(cart);
    kin::update_physics_world(world, physics, dt);

    const kin::PhysicsBody cart_body = cart.get<kin::PhysicsBodyComponent>()->body;
    assert(physics.fixtures(cart_body).size() == 3);
    assert(front.get<kin::PhysicsColliderComponent>()->body == cart_body);
    // Turned with the cart: +x is down now; the back wheel is twice as big.
    assert(has_centre(physics, {100.0f, 20.0f}) && has_centre(physics, {100.0f, -20.0f}));
    const std::vector<kin::PhysicsDebugShape> shapes = physics.debug_shapes();
    assert(std::ranges::any_of(shapes, [](const kin::PhysicsDebugShape& shape) { return near(shape.radius, 8.0f); }));

    // A child moved (an animated hitbox): its shape is made again there.
    front.set(kin::Transform2D{.pos = {30.0f, 0.0f}});
    kin::update_physics_world(world, physics, dt);
    assert(physics.fixtures(cart_body).size() == 3);
    assert(has_centre(physics, {100.0f, 30.0f}) && !has_centre(physics, {100.0f, 20.0f}));
    // Its collider taken away: its shape goes, the others stay.
    front.remove<kin::PhysicsColliderComponent>();
    kin::update_physics_world(world, physics, dt);
    assert(physics.fixtures(cart_body).size() == 2);

    // Shapes of all kinds follow the child's place: a polygon's points too.
    kin::EcsEntity fin = world.entity("fin").set(kin::Transform2D{.pos = {0.0f, 10.0f}}).set(one(PhysicsShape::polygon({{0, 0}, {10, 0}, {0, 10}})));
    fin.child_of(cart);
    kin::update_physics_world(world, physics, dt);
    // Its corner (0, 0) is (0, 10) from the cart, turned a quarter: (-10, 0) from it.
    assert(std::ranges::any_of(physics.debug_shapes(), [](const kin::PhysicsDebugShape& shape) {
        return shape.points.size() == 3 && std::ranges::any_of(shape.points, [](Vec2f p) { return near(p, {90.0f, 0.0f}, 0.1f); });
    }));

    // With no body above it, a collider is nothing.
    kin::EcsEntity loose = world.entity("loose").set(kin::Transform2D{}).set(one(PhysicsShape::circle(2.0f)));
    kin::update_physics_world(world, physics, dt);
    assert(!physics.is_alive(loose.get<kin::PhysicsColliderComponent>()->colliders[0].fixture));
    // The body gone: its children's shapes with it; back, they are made again.
    cart.remove<kin::PhysicsBodyComponent>();
    assert(!physics.is_alive(back.get<kin::PhysicsColliderComponent>()->colliders[0].fixture));
    cart.set(kin::PhysicsBodyComponent{.def = {.type = PhysicsBodyType::Kinematic}});
    kin::update_physics_world(world, physics, dt);
    assert(physics.fixtures(cart.get<kin::PhysicsBodyComponent>()->body).size() == 3);
}

void test_joint_components() {
    kin::EcsWorld world;
    kin::register_physics_components(world);
    world.component<kin::Transform2D>("Transform2D");
    kin::PhysicsWorld physics({.gravity = {0.0f, 320.0f}, .units_per_meter = 32.0f});
    kin::EcsPhysicsBridge bridge{world, physics};

    // A door on a hinge to its frame, kept within 0 to 90 degrees.
    kin::EcsEntity frame = body(world, "frame", {0.0f, 0.0f}, PhysicsBodyType::Static);
    frame.set(one(PhysicsShape::box({2.0f, 2.0f})));
    kin::EcsEntity door = body(world, "door", {32.0f, 0.0f});
    door.set(one(PhysicsShape::box({32.0f, 2.0f})));
    door.set(kin::PhysicsJointComponent{.type = kin::PhysicsJointType::Hinge,
                                        .other = {.id = frame.id()},
                                        .anchor = {-32.0f, 0.0f},
                                        .limit = true,
                                        .lower = 0.0f,
                                        .upper = 45.0f});
    for (int i = 0; i < 120; ++i) {
        kin::update_physics_world(world, physics, dt);
    }
    const kin::PhysicsJointComponent* hinge = door.get<kin::PhysicsJointComponent>();
    assert(physics.is_alive(hinge->joint));
    // It swung down (clockwise on screen: a positive angle) to its limit.
    assert(near(door.get<kin::Transform2D>()->rotation, 45.0f, 1.0f));
    assert(near(physics.joint_value(hinge->joint), 0.785f, 0.02f));

    // A motor given at run time: it turns the door back.
    door.get_mut<kin::PhysicsJointComponent>()->motor = true;
    door.get_mut<kin::PhysicsJointComponent>()->motor_speed = -90.0f;
    door.get_mut<kin::PhysicsJointComponent>()->max_motor_force = 1.0e6f;
    const kin::PhysicsJoint before = hinge->joint;
    for (int i = 0; i < 60; ++i) {
        kin::update_physics_world(world, physics, dt);
    }
    assert(door.get<kin::PhysicsJointComponent>()->joint == before); // the same joint, changed
    assert(near(door.get<kin::Transform2D>()->rotation, 0.0f, 1.0f));

    // A weight on a slack rope that breaks: it would hold it hanging (its
    // weight, 0.2 kg at 10 m/s^2, is 63 in points), not the jerk of its fall.
    kin::EcsEntity weight = body(world, "weight", {200.0f, 32.0f});
    weight.set(one(PhysicsShape::circle(8.0f)));
    weight.set(kin::PhysicsJointComponent{.type = kin::PhysicsJointType::Distance,
                                          .other_anchor = {0.0f, -64.0f},
                                          .limit = true,
                                          .upper = 96.0f,
                                          .break_force = 100.0f});
    kin::update_physics_world(world, physics, dt);
    assert(physics.is_alive(weight.get<kin::PhysicsJointComponent>()->joint));
    for (int i = 0; i < 60; ++i) {
        kin::update_physics_world(world, physics, dt);
    }
    assert(weight.get<kin::PhysicsJointComponent>()->broken);
    assert(!physics.is_alive(weight.get<kin::PhysicsJointComponent>()->joint));
    assert(weight.get<kin::Transform2D>()->pos.y > 120.0f); // and it fell

    // The other body gone: the joint goes; the component taken away: too.
    frame.remove<kin::PhysicsBodyComponent>();
    assert(!physics.is_alive(door.get<kin::PhysicsJointComponent>()->joint));
    frame.set(kin::PhysicsBodyComponent{.def = {.type = PhysicsBodyType::Static}});
    kin::update_physics_world(world, physics, dt);
    const kin::PhysicsJoint again = door.get<kin::PhysicsJointComponent>()->joint;
    assert(physics.is_alive(again));
    door.remove<kin::PhysicsJointComponent>();
    assert(!physics.is_alive(again));
    (void)bridge;
}

kin::TilemapAsset map_of(const std::vector<std::string>& rows) {
    kin::TilemapAsset asset;
    kin::TileMap& map = asset.map;
    map.resize(static_cast<kin::i32>(rows[0].size()), static_cast<kin::i32>(rows.size()));
    map.tile_w = 16;
    map.tile_h = 16;
    map.tileset.resize(3);
    map.tileset.tiles[1] = {.id = "wall", .solid = true};
    // A slope: its cell's lower right half.
    map.tileset.tiles[2] = {.id = "slope", .solid = true, .collision_polygon = {{16, 0}, {16, 16}, {0, 16}}};
    for (std::size_t row = 0; row < rows.size(); ++row) {
        for (std::size_t col = 0; col < rows[row].size(); ++col) {
            const char c = rows[row][col];
            if (c != '.') {
                map.set(static_cast<kin::i32>(col), static_cast<kin::i32>(row), c == '#' ? 1 : 2);
            }
        }
    }
    return asset;
}

f32 doubled_area(const std::vector<Vec2f>& loop) {
    f32 area = 0.0f;
    for (std::size_t i = 0; i < loop.size(); ++i) {
        const Vec2f a = loop[i];
        const Vec2f b = loop[(i + 1) % loop.size()];
        area += a.x * b.y - a.y * b.x;
    }
    return area;
}

void test_tile_collision_shapes() {
    // An L of walls: one loop, its six corners (straight sides one each).
    kin::TileCollisionShapes shapes = map_of({"#..", "#..", "###"}).map.collision_shapes();
    assert(shapes.loops.size() == 1 && shapes.loops[0].size() == 6 && shapes.polygons.empty());
    assert(doubled_area(shapes.loops[0]) > 0.0f); // clockwise on screen
    // Two walls touching only at a corner: two loops.
    shapes = map_of({"#.", ".#"}).map.collision_shapes();
    assert(shapes.loops.size() == 2 && shapes.loops[0].size() == 4 && shapes.loops[1].size() == 4);
    // A ring: its outside, and its hole the other way round.
    shapes = map_of({"###", "#.#", "###"}).map.collision_shapes();
    assert(shapes.loops.size() == 2);
    const bool first_outside = std::abs(doubled_area(shapes.loops[0])) > std::abs(doubled_area(shapes.loops[1]));
    const std::vector<Vec2f>& outside = shapes.loops[first_outside ? 0 : 1];
    const std::vector<Vec2f>& hole = shapes.loops[first_outside ? 1 : 0];
    assert(doubled_area(outside) > 0.0f && doubled_area(hole) < 0.0f);
    assert(near(std::abs(doubled_area(hole)), 2.0f * 16.0f * 16.0f));
    // A slope tile: its polygon, from its cell.
    shapes = map_of({"..", ".s"}).map.collision_shapes();
    assert(shapes.loops.empty() && shapes.polygons.size() == 1);
    assert(near(shapes.polygons[0][0], {32.0f, 16.0f}));
    // A layer kept out of collision: nothing.
    kin::TilemapAsset ghost = map_of({"##"});
    ghost.map.layers[0].participates_in_collision = false;
    assert(ghost.map.collision_shapes().loops.empty());
}

void test_tilemap_colliders() {
    kin::EcsWorld world;
    kin::register_physics_components(world);
    world.component<kin::Transform2D>("Transform2D");
    world.component<kin::TilemapComponent>("TilemapComponent");
    kin::PhysicsWorld physics({.gravity = {0.0f, 320.0f}, .units_per_meter = 32.0f});
    kin::EcsPhysicsBridge bridge{world, physics};

    // A floor with a step, its map's top left at (100, 0).
    auto tiles = std::make_shared<kin::TilemapAsset>(map_of({"......", "......", "....##", "######"}));
    kin::EcsEntity level = world.entity("level")
                               .set(kin::Transform2D{.pos = {100.0f, 0.0f}})
                               .set(kin::TilemapComponent{.tilemap = tiles})
                               .set(kin::TilemapCollider{});
    kin::EcsEntity ball = body(world, "ball", {120.0f, 0.0f});
    ball.set(one(PhysicsShape::circle(6.0f)));
    kin::EcsEntity crate = body(world, "crate", {180.0f, -10.0f});
    crate.set(one(PhysicsShape::box({6.0f, 6.0f})));
    for (int i = 0; i < 120; ++i) {
        kin::update_physics_world(world, physics, dt);
    }
    const kin::TilemapCollider* collider = level.get<kin::TilemapCollider>();
    assert(physics.is_alive(collider->body) && physics.fixtures(collider->body).size() == 1); // one loop
    assert(physics.user_id(collider->body) == level.id());
    // The ball rests on the floor (its top at y = 48), the crate on the step (32).
    assert(near(ball.get<kin::Transform2D>()->pos.y, 42.0f, 0.5f));
    assert(near(crate.get<kin::Transform2D>()->pos.y, 26.0f, 0.5f));

    // The map read again: its body made again from it.
    const kin::PhysicsBody first = collider->body;
    level.set(kin::TilemapComponent{.tilemap = std::make_shared<kin::TilemapAsset>(map_of({"#.", "##"}))});
    kin::update_physics_world(world, physics, dt);
    assert(!physics.is_alive(first) && physics.is_alive(level.get<kin::TilemapCollider>()->body));
    // Taken away: the body goes.
    const kin::PhysicsBody second = level.get<kin::TilemapCollider>()->body;
    level.remove<kin::TilemapCollider>();
    assert(!physics.is_alive(second));
    (void)bridge;
}

void test_in_scene_files() {
    kin::EcsWorld world;
    kin::register_builtin_editor_components(world);
    kin::register_physics_components(world);
    kin::EcsComponentRegistry& components = world.components();
    for (const char* name : {"PhysicsJointComponent", "TilemapCollider"}) {
        assert(components.find(name));
    }
    const kin::ComponentDescriptor* collider = components.find("PhysicsColliderComponent");
    const auto field = [&](const kin::ComponentDescriptor* descriptor, std::string_view name) {
        return std::ranges::find_if(descriptor->fields, [&](const kin::ComponentFieldDescriptor& f) { return f.name == name; });
    };
    assert(field(collider, "points")->kind == kin::ComponentFieldKind::Points);
    assert(std::ranges::count(field(collider, "shape")->choices, std::string("capsule")) == 1);
    assert(field(components.find("PhysicsJointComponent"), "other")->kind == kin::ComponentFieldKind::EntityRef);

    const kin::SceneLoadResult read = kin::parse_scene_asset(R"({
  "schema": "kin.scene/1",
  "entities": [
    {"id": "frame", "name": "Frame", "components": {"Transform2D": {}, "PhysicsBodyComponent": {"type": "static"}}},
    {"id": "door", "name": "Door", "components": {
      "Transform2D": {"pos": {"x": 32, "y": 0}},
      "PhysicsBodyComponent": {"type": "dynamic"},
      "PhysicsColliderComponent": {"shape": "polygon", "points": [[-32, -2], [32, -2], [32, 2], [-32, 2]]},
      "PhysicsJointComponent": {"upper": 90, "type": "hinge", "other": "frame", "anchor": {"x": -32, "y": 0}, "limit": true}}},
    {"id": "knob", "name": "Knob", "parent": "door", "components": {
      "Transform2D": {"pos": {"x": 28, "y": 0}},
      "PhysicsColliderComponent": {"shape": "capsule", "half_extents": {"x": 2, "y": 4}, "angle": 90}}}
  ]
})", components);
    assert(read.ok());
    const kin::SceneInstance made = kin::instantiate_scene_asset(world, *read.scene, ".", components);
    assert(made.ok());
    const kin::EcsEntity door = made.by_authored_id.at("door");
    const kin::PhysicsJointComponent* joint = door.get<kin::PhysicsJointComponent>();
    assert(joint->other.id == made.by_authored_id.at("frame").id());
    assert(joint->type == kin::PhysicsJointType::Hinge && joint->limit && joint->upper == 90.0f);
    const kin::PhysicsShape& shape = door.get<kin::PhysicsColliderComponent>()->colliders[0].def.shape;
    assert(shape.type == kin::PhysicsShapeType::Polygon && shape.points.size() == 4 && near(shape.points[1], {32.0f, -2.0f}));
    const kin::PhysicsShape& knob = made.by_authored_id.at("knob").get<kin::PhysicsColliderComponent>()->colliders[0].def.shape;
    assert(knob.type == kin::PhysicsShapeType::Capsule && near(knob.angle, 1.5708f, 0.001f));

    // Made in the world: the door's two shapes on its body, joined to the frame.
    kin::PhysicsWorld physics;
    kin::EcsPhysicsBridge bridge{world, physics};
    kin::update_physics_world(world, physics, dt);
    assert(physics.fixtures(door.get<kin::PhysicsBodyComponent>()->body).size() == 2);
    assert(physics.is_alive(door.get<kin::PhysicsJointComponent>()->joint));

    // Written back as read: the points, the ref by its authored id.
    const kin::SceneAsset saved = kin::scene_asset_from_world(world);
    std::ostringstream out;
    kin::write_scene_asset(out, saved);
    const std::string text = out.str();
    assert(text.find("\"other\": \"frame\"") != std::string::npos || text.find("\"other\":\"frame\"") != std::string::npos);
    const kin::SceneLoadResult again = kin::parse_scene_asset(text, components);
    assert(again.ok());
    (void)bridge;
}

void test_points_values() {
    std::string error;
    const kin::JsonValue json = *kin::parse_json(R"([[1, 2], {"x": 3, "y": 4}])").value;
    const std::optional<kin::ComponentFieldValue> points = kin::component_field_value_from_json(kin::ComponentFieldKind::Points, json, error);
    assert(points && std::get<std::vector<Vec2f>>(*points).size() == 2 && near(std::get<std::vector<Vec2f>>(*points)[1], {3.0f, 4.0f}));
    assert(kin::component_field_kind_from_value(*points) == kin::ComponentFieldKind::Points);
    assert(kin::component_field_kind_name(kin::ComponentFieldKind::Points) == "points");
    assert(!kin::component_field_value_from_json(kin::ComponentFieldKind::Points, *kin::parse_json(R"([[1, 2], "no"])").value, error));
    assert(!kin::component_field_value_from_json(kin::ComponentFieldKind::Points, *kin::parse_json("3").value, error));
    std::ostringstream written;
    kin::JsonWriter writer(written, false);
    kin::write_component_field_value_json(writer, *points);
    assert(written.str() == "[[1,2],[3,4]]");
}

} // namespace

int main() {
    test_child_colliders();
    test_joint_components();
    test_tile_collision_shapes();
    test_tilemap_colliders();
    test_in_scene_files();
    test_points_values();
    return 0;
}
