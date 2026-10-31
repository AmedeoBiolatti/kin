# Scene flow

A game made of several scenes goes from one to the next through doors:
`SceneDoor` and `SceneTraveller` (`kin/ecs/scene_flow.hpp`), in scene files like
any component.

```json
{"id": "cave_mouth", "name": "CaveMouth", "components": {
  "Transform2D": {"pos": {"x": 900, "y": 420}},
  "SceneDoor": {"scene": "scenes/cave.kinscene", "arrive": "Entrance",
                "size": {"x": 48, "y": 64}, "transition": "iris", "duration": 0.8}}}
```

**A door** names the scene it leads to (none: another place of this scene) and the
entity there that travellers arrive at, by name: another door, or any marker.
Two doors that name each other make a way there and back.

- **`trigger`:** `touch` goes through as the traveller steps into the door's area;
  `action` goes through when the traveller presses the door's `action` inside it.
- **`size` and `offset`:** the area, centred on the entity and scaled with it.
- **`exit`:** where those arriving at this door stand, from it. A traveller that
  arrives inside a door's area goes through it only after stepping out and back in,
  so arriving at a door never sends it straight back.
- **`transition`:** `fade`, `crossfade`, `dissolve`, `pixelate`, `wipe`, `iris` or
  `none`, over `duration` seconds through `color`.

**A traveller** is whoever goes through doors (the player). Its position is what is
in a door's area.

## In the game

`SceneFlow` notes the doors used and hands the game the travel. The game reads the
scene file, then puts the travellers where they arrive:

```cpp
flow.update(world, ctx.input); // after the travellers moved
if (std::optional<kin::SceneTravel> go = flow.take()) {
    auto enter = [this, go = *go](kin::SceneContext&) {
        if (!go.scene.empty()) load_scene(go.scene); // this scene, another file
        flow.arrive(world, go.arrive);
    };
    if (go->transition) {
        ctx.scenes.push(kin::transition_within(enter, *go->transition, go->duration, go->color));
    } else {
        enter(ctx);
    }
}
```

`transition_within` plays a transition over a scene that stays: the scene reads the
other file while the screen is covered, and keeps its music, its dialogue
variables and whatever else the game holds. `transition_to` replaces the scene with
another `Scene` object instead.

`arrive` moves the travellers' bodies too, at the next physics step
(`PhysicsTeleport`). A game can put that tag on any entity with a body to move its
body to where the entity is, once.

**Scripts** ask for a travel with `scene.go(path[, arrive[, transition[, seconds]]])`,
given the flow (`EntityScripts::set_flow`); `scene.going()` is true until the game
takes it.
