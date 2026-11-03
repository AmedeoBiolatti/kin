# 2D Physics: Queries, Characters, One-Way Platforms

Kin's 2D physics is Box2D behind `kin::PhysicsWorld` (`kin/physics/physics.hpp`),
and the components scenes give entities (`kin/ecs/physics.hpp`; their fields:
[components in scenes](ecs_editor_workflow.md#kins-components-in-scenes)).
This page is about asking the world what is where, moving characters, and
platforms to jump up through.

Lengths are in the game's units: a world in points says how many make a metre
(`PhysicsWorldDef::units_per_meter`). On screen y goes down, so up is -y.

## What is where

```cpp
kin::PhysicsFilter filter{.include_sensors = false, .ignore = my_body};
world.query_point(point, filter);                    // fixtures under a point
world.query_aabb(rect, filter);                      // whose boxes overlap a rectangle
world.raycast(from, to, filter);                     // each fixture's nearest hit, nearest first
world.query_shape(kin::PhysicsShape::circle(12), at, filter); // fixtures a shape overlaps
world.cast_shape(kin::PhysicsShape::box({8, 16}), from, to, filter); // the first thing a shape meets
```

- `PhysicsFilter` takes `mask_bits` (the kinds of collider to find),
  `include_sensors`, and `ignore`, a body left out (the one asking).
- `query_shape` and `cast_shape` take a box, a circle, a capsule or a polygon
  (as its outline's hull), placed at the point by its `offset` and `angle`. A
  chain is not a shape that moves.
- `cast_shape` returns a `PhysicsShapeHit`: the body and fixture, the `point`
  touched, the `normal` out of what was hit and the `fraction` of the way gone.
  A shape that starts touching something is stopped by it only if it goes on
  into it, so a shape standing on a floor can be cast along the floor.
- `body_of(fixture)` says which body a fixture is on.

## Characters

A character is moved by its game, not pushed about by the simulation: it goes
where it is told, as far as it can, and slides along what stops it.

```json
"Player": {
  "Transform2D": {"pos": {"x": 64, "y": 300}},
  "PhysicsBodyComponent": {},
  "PhysicsColliderComponent": {"shape": "capsule", "half_extents": {"x": 10, "y": 18}},
  "CharacterController2D": {"gravity": 2000, "snap": 8},
  "ScriptComponent": {"asset": "scripts/player.lua"}
}
```

```lua
function on_update(self, dt)
    local _, vy = self:velocity()
    if self:on_floor() and input.pressed("jump") then
        vy = -760
    end
    self:set_velocity(input.axis("move_x") * 280, vy)
end
```

`CharacterController2D` is stepped by `update_physics_world`, before the world
steps:

- Its shape is its first collider that is not a sensor: a box (its corners a
  little round, so it slides over seams), a circle or a capsule.
- Its body is made for it: dynamic, never turning, never asleep and with no
  gravity of the world's. Sensors see it (coins), dynamic bodies bump into it,
  and scripts' `on_contact` hears it.
- Each step it goes along `velocity`, with `gravity` (its own, along -up)
  added while it is not on a floor. What stops it is a floor when it is at
  most `max_slope` steep (degrees), a ceiling when it is that close to facing
  down, and otherwise a wall.
- Landing stops its fall: the part of `velocity` along -up goes. A ceiling
  stops its rise, and a wall what went into it. `velocity` keeps the rest.
- It walks up a slope as fast as on the flat. A slope steeper than
  `max_slope` is a wall: walking into it climbs nothing, and it slides down it
  when falling. With `stop_on_slope` (the default) it stands on a slope without
  sliding down.
- `snap` (units): on a floor and not going up, it is kept on the floor as it
  walks down slopes and steps this far. At 0 it walks off them and falls.
- What it stands on carries it, as it moves or turns. That holds whether the
  simulation moves it (a kinematic platform with a velocity) or its entity
  does (`sync_to_physics`). Moving bodies are met where they will be as they
  move in the step, so a platform going down doesn't leave it in the air.
- A dynamic body it walks into is pushed on at its speed times `push` (1 by
  default; 0: such bodies stop it as walls do).
- After each step it says what it found: `on_floor`, `on_wall`, `on_ceiling`,
  `floor_normal` and `wall_normal`.
- Putting it somewhere else (`self:set_position`, or `PhysicsTeleport`) moves
  its body there at the next step, and what it stood on stops carrying it.

The same move without the component is `PhysicsWorld::move_character(def,
position, velocity, dt, on_floor, carry)`. It returns where the character
ended, its velocity less what ran into something, what it found, and its
`hits`. The character's own body is not moved: put it there, as the component
does by giving it the velocity that takes it there.

## One-way platforms

A collider with `one_sided` is a platform to jump up through. A chain stops
only what comes from its left going along it: drawn left to right, its top. Any
other shape stops only what comes down onto its top, its -y turned with it.
What touches it from below or from the side passes through. This holds:

- for characters, which land on it only when they come down onto it from
  wholly above it. `CharacterController2D::drop_through` (seconds) lets one
  fall through, for dropping down off a platform;
- for dynamic bodies, which pass through until they stop touching it (their
  contacts still begin and end, as a sensor's do);
- for `cast_shape`, which hits it only coming down onto it.
