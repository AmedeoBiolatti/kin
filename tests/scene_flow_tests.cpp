// Scene flow: doors (SceneDoor) a SceneTraveller goes through, by touch or by
// action; arriving at a named entity (a door's exit); a door one arrives in
// waits to be stepped out of; doors read from a scene file.

#include <kin/ecs/render.hpp>
#include <kin/ecs/scene_flow.hpp>
#include <kin/ecs/world.hpp>
#include <kin/platform/input.hpp>
#include <kin/scene/scene_asset.hpp>
#if KIN_ENABLE_PHYSICS
#include <kin/ecs/physics.hpp>
#endif

#include <cassert>
#include <cmath>
#include <optional>
#include <sstream>
#include <string>

namespace {

using namespace kin;

bool near(Vec2f a, Vec2f b) {
    return std::abs(a.x - b.x) < 1e-3f && std::abs(a.y - b.y) < 1e-3f;
}

void test_doors() {
    kin::EcsWorld world;
    register_builtin_editor_components(world);
    kin::EcsEntity player = world.entities().create({.name = "Player"});
    player.set(Transform2D{.pos = {0.0f, 0.0f}}).set(SceneTraveller{});
    kin::EcsEntity cave = world.entities().create({.name = "CaveMouth"});
    cave.set(Transform2D{.pos = {100.0f, 0.0f}});
    cave.set(SceneDoor{.scene = {.id = "scenes/cave.kinscene"}, .arrive = "Entrance", .size = {20.0f, 20.0f}, .transition = TransitionKind::Iris, .duration = 0.8f});
    kin::EcsEntity hatch = world.entities().create({.name = "Hatch"});
    hatch.set(Transform2D{.pos = {0.0f, 100.0f}});
    hatch.set(SceneDoor{.scene = {}, .arrive = "Ladder", .trigger = DoorTrigger::Action, .action = "use", .size = {20.0f, 20.0f}, .transition = std::nullopt});
    kin::EcsEntity ladder = world.entities().create({.name = "Ladder"});
    ladder.set(Transform2D{.pos = {500.0f, 500.0f}});
    ladder.set(SceneDoor{.arrive = "Hatch", .size = {20.0f, 20.0f}, .exit = {0.0f, -30.0f}});

    Input input;
    input.bind("use", Key::E);
    SceneFlow flow;
    flow.update(world, input);
    assert(!flow.pending());

    // Touch: stepping in goes through, once.
    player.set(Transform2D{.pos = {95.0f, 5.0f}});
    flow.update(world, input);
    std::optional<SceneTravel> go = flow.take();
    assert(go && go->scene == "scenes/cave.kinscene" && go->arrive == "Entrance" && go->door == cave.id());
    assert(go->transition == TransitionKind::Iris && std::abs(go->duration - 0.8f) < 1e-6f);
    assert(!flow.take());
    flow.update(world, input); // still in it: not again
    assert(!flow.pending());

    // Action: in it, nothing until the action is pressed.
    player.set(Transform2D{.pos = {0.0f, 95.0f}});
    flow.update(world, input);
    assert(!flow.pending());
    assert(flow.action_door(world) && flow.action_door(world)->id() == hatch.id());
    input.set_key_pressed(Key::E);
    flow.update(world, input);
    go = flow.take();
    assert(go && go->scene.empty() && go->arrive == "Ladder" && !go->transition);

    // Arriving at a door: at its exit, out of its area; not sent back.
    assert(flow.arrive(world, go->arrive));
    assert(near(player.get<Transform2D>()->pos, {500.0f, 470.0f}));
    flow.update(world, input);
    assert(!flow.pending());
    // Arriving in a door's area (no exit): it waits until stepped out of.
    assert(flow.arrive(world, "CaveMouth"));
    assert(near(player.get<Transform2D>()->pos, {100.0f, 0.0f}));
    flow.update(world, input);
    assert(!flow.pending());
    player.set(Transform2D{.pos = {300.0f, 0.0f}});
    flow.update(world, input);
    player.set(Transform2D{.pos = {100.0f, 0.0f}});
    flow.update(world, input);
    assert(flow.take());

    // Nothing named so: they stay. Disabled doors and travellers do nothing.
    assert(!flow.arrive(world, "Nowhere") && near(player.get<Transform2D>()->pos, {100.0f, 0.0f}));
    flow.update(world, input);
    cave.get_mut<SceneDoor>()->enabled = false;
    player.set(Transform2D{.pos = {300.0f, 0.0f}});
    flow.update(world, input);
    player.set(Transform2D{.pos = {100.0f, 0.0f}});
    flow.update(world, input);
    assert(!flow.pending());

    // Asked for by code: the latest wins.
    flow.go({.scene = "scenes/a.kinscene"});
    flow.go({.scene = "scenes/b.kinscene", .arrive = "Start"});
    go = flow.take();
    assert(go && go->scene == "scenes/b.kinscene" && !go->door);

    // A traveller under a parent arrives where the target is in the world.
    kin::EcsEntity boat = world.entities().create({.name = "Boat"});
    boat.set(Transform2D{.pos = {1000.0f, 0.0f}});
    player.raw().child_of(boat.raw());
    assert(flow.arrive(world, "Ladder"));
    assert(near(current_world_transform(player).pos, {500.0f, 470.0f}));
}

void test_doors_in_scene_files() {
    kin::EcsWorld world;
    register_builtin_editor_components(world);
    const SceneLoadResult read = parse_scene_asset(R"({
  "schema": "kin.scene/1",
  "entities": [
    {"id": "door", "name": "Door", "components": {
      "Transform2D": {"pos": {"x": 10, "y": 20}},
      "SceneDoor": {"scene": "scenes/house.kinscene", "arrive": "FrontDoor", "trigger": "action",
                    "action": "enter", "size": {"x": 40, "y": 60}, "exit": {"x": 0, "y": 50},
                    "transition": "wipe", "duration": 1.5, "prompt": "Go in"}}},
    {"id": "spot", "name": "Spot", "components": {"Transform2D": {}, "SceneDoor": {"transition": "none"}}},
    {"id": "hero", "name": "Hero", "components": {"Transform2D": {}, "SceneTraveller": {}}}
  ]
})", world.components());
    assert(read.ok());
    const SceneInstance made = instantiate_scene_asset(world, *read.scene, ".", world.components());
    assert(made.ok());
    const SceneDoor* door = made.by_authored_id.at("door").get<SceneDoor>();
    assert(door && door->scene.id == "scenes/house.kinscene" && door->arrive == "FrontDoor");
    assert(door->trigger == DoorTrigger::Action && door->action == "enter" && door->prompt == "Go in");
    assert(near(door->size, {40.0f, 60.0f}) && near(door->exit, {0.0f, 50.0f}));
    assert(door->transition == TransitionKind::Wipe && std::abs(door->duration - 1.5f) < 1e-6f);
    assert(!made.by_authored_id.at("spot").get<SceneDoor>()->transition);
    assert(made.by_authored_id.at("hero").has<SceneTraveller>());
    // And written back as read.
    const SceneAsset saved = scene_asset_from_world(world);
    const SceneLoadResult again = parse_scene_asset([&] {
        std::ostringstream out;
        write_scene_asset(out, saved);
        return out.str();
    }(), world.components());
    assert(again.ok());
}

#if KIN_ENABLE_PHYSICS
void test_bodies_arrive() {
    kin::EcsWorld world;
    register_builtin_editor_components(world);
    PhysicsWorld physics{PhysicsWorldDef{.gravity = {0.0f, 0.0f}}};
    EcsPhysicsBridge bodies{world, physics};
    kin::EcsEntity player = world.entities().create({.name = "Player"});
    player.set(Transform2D{}).set(SceneTraveller{});
    player.set(PhysicsBodyComponent{.def = {.type = PhysicsBodyType::Dynamic}});
    kin::EcsEntity marker = world.entities().create({.name = "Marker"});
    marker.set(Transform2D{.pos = {40.0f, 50.0f}});
    update_physics_world(world, physics, 1.0f / 60.0f); // the body made
    const PhysicsBody body = player.get<PhysicsBodyComponent>()->body;
    assert(physics.is_alive(body));
    SceneFlow flow;
    assert(flow.arrive(world, "Marker"));
    update_physics_world(world, physics, 1.0f / 60.0f);
    assert(near(physics.position(body), {40.0f, 50.0f}));
    assert(near(player.get<Transform2D>()->pos, {40.0f, 50.0f}));
    assert(!player.has<PhysicsTeleport>()); // once
}
#endif

} // namespace

int main() {
    test_doors();
    test_doors_in_scene_files();
#if KIN_ENABLE_PHYSICS
    test_bodies_arrive();
#endif
    return 0;
}
