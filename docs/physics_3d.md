# 3D physics (experimental)

`kin/physics3d` is rigid body physics for kin's experimental 3D work, on
[Jolt Physics](https://github.com/jrouwe/JoltPhysics) 5.5.0 (MIT). It exists only with
`KIN_EXPERIMENTAL_3D` (on by default; the build without it never fetches Jolt). The Box2D module
(`kin/physics`, 2D) is separate and unchanged. The API may change in any release; the plan and
measurements are in [3d_plan.md](3d_plan.md) ("P2a - Jolt core - results", "P2b - queries, events, constraints, debug draw, ECS
- results").

Only kin's own types appear in the headers (`kin/physics3d/world3d.hpp`, `shape3d.hpp`): no Jolt
header leaks. Units are metres, kilograms and seconds; right-handed, +Y up (see
[3D rendering](rendering_3d.md)); rotations are `Quatf`.

## A world and bodies

```cpp
#include <kin/physics3d/world3d.hpp>

kin::World3D world;                              // gravity (0, -9.81, 0), 1 collision step, default job pool
world.create_body({.shape = kin::Shape3D::box({20, 0.5f, 20}),
                   .motion = kin::Body3DMotion::Static,
                   .position = {0, -0.5f, 0}});
kin::Body3D crate = world.create_body({.shape = kin::Shape3D::box({0.5f, 0.5f, 0.5f}),
                                       .position = {0, 4, 0},
                                       .density = 600.0f, .friction = 0.6f, .restitution = 0.1f,
                                       .user_id = 42});
world.optimize_broad_phase();                    // once, after a level's bodies are in

for (;;) {                                       // the engine's fixed step
    world.step(1.0f / 60.0f);
    kin::Vec3f p = world.position(crate);
    kin::Quatf q = world.rotation(crate);
}
```

* `World3DConfig`: `gravity` (a vector: sideways, or toward a planet's centre by `set_gravity` each
  step), `collision_steps` per `step()`, capacities (`max_bodies`, `max_body_pairs`,
  `max_contact_constraints`), `temp_memory_bytes`, `jobs` (a `kin::JobSystem*`, null is
  `default_job_system()`), `single_threaded`.
* `Body3DDesc`: shape, motion (`Static`, `Kinematic`, `Dynamic`), position, rotation, velocities,
  `mass` or `density` (a dynamic body's mass is `mass` if above zero, else `density * volume`),
  friction (a contact uses `sqrt(a * b)`), restitution (the larger of the two), damping, gravity
  factor, layer, `sensor`, `allow_sleeping`, `continuous` (swept against static geometry),
  `start_active`, `user_id` (yours; hashed).
* A `Body3D` is `{index, generation}`: a destroyed body's handle stays dead when its number is reused.
  Calls with a dead handle do nothing and read as zero. `create_body` returns an invalid handle for an
  invalid shape, a non-finite number, a dynamic body of a mesh or height field, or a non-static body
  in layer 0.
* Moving: `set_position` / `set_rotation` (a teleport), `set_linear_velocity` /
  `set_angular_velocity`, `move_kinematic(body, position, rotation, dt)` (a kinematic body gets the
  velocities that reach the target by the end of the next step, so it pushes what it touches).
  `add_force*` and `add_torque` act for the next step only; `add_impulse*` once.
* Helpers for bodies something else drives: `teleport(body, position, rotation)` (no velocity left, awake),
  `follow_transform(body, position, rotation, dt)` (what suits the motion: a kinematic body is moved as `move_kinematic` does, a
  dynamic one gets the velocities that reach the target in `dt` and still collides on the way, a static one is placed),
  `set_layer`, `set_sensor` / `is_sensor` after creation, `set_shape(body, shape)` (a crouch, a different size: build a shape of the
  new size; a dynamic body keeps its mass and takes the new shape's inertia; a mesh is refused for it), `bodies()` (the live ones by
  number). `user_id` is the per-body user data (a `u64`, hashed; `set_user_id`).
* Reading: `position` (the body's origin), `centre_of_mass_position`, `rotation`, velocities, `mass`,
  `motion`, `layer`, `user_id`, `shape`, `is_active`; `activate` / `deactivate`; `contains`,
  `body_count`, `active_body_count`. `destroy_body` wakes what overlaps the body (Jolt does not).

## Shapes

`Shape3D` is a shareable, immutable handle; one shape serves any number of bodies and worlds.

| Factory | Notes |
|---|---|
| `box(half_extents)` | corners rounded by at most 5 cm inside the box |
| `sphere(radius)` | |
| `capsule(half_height, radius)` | along Y; `half_height` is half the straight part |
| `cylinder(half_height, radius)` | along Y |
| `convex_hull(points)` | at least 4 points, not coplanar |
| `triangle_mesh(vertices, indices)` | static and kinematic bodies only; double sided |
| `height_field(heights, n, offset, scale)` | n x n samples (even, at least 4), `FLT_MAX` is a hole; 8 bits a sample per block |
| `compound(children)` | `ShapeChild3D {shape, position, rotation}` placed in one rigid shape |

`volume()`, `centre_of_mass()` and `bounds_min()` / `bounds_max()` are in the shape's own space (a
body's origin). A factory that cannot build returns an invalid handle (`valid()` false).

## Layers

A body is in one of 32 layers (`layers3d::Static` 0, `Dynamic` 1, `Kinematic` 2, `Character` 3,
`Sensor` 4, `User0` 5 to 31; `Auto` picks Static, Dynamic or Kinematic by motion). Layer 0 is for
static bodies only. Which pairs collide is a symmetric table: `set_layers_collide(a, b, bool)`,
`layers_collide`, `collision_mask(layer)`; everything collides at first but sensor with sensor. A set
of layers is a `Layer3DMask` (`layer_bit(n)`, `all_layers3d`): what queries will filter by.

## Determinism

A step is a pure function of the world's state, the calls made since the last step and `dt`: no
clock, no random numbers, and the same bits for no workers, 1 or many (tested). Jolt is built with
cross-platform determinism, so another machine should give the same bits (not yet run on another
platform). Keep the order of the calls that change the world the same between runs: bodies are
numbered in creation order with freed numbers reused last in, first out.

* `state_hash()`: FNV-1a 64 over each body, in number order: number, generation, user id, motion,
  layer, awake, position, rotation, velocities; floats bit for bit (NaNs as one NaN, -0 apart from
  +0). The same on every platform. Feed it to a lockstep check, or per frame to a test.
* `save_state()` / `restore_state(bytes)`: the dynamic state, contacts included, for a rollback; the
  restored world steps to the same bits. It needs the same set of bodies (none created or destroyed
  since) and returns false otherwise.

## Threads

`step()` cuts the work into Jolt jobs that run on the `kin::JobSystem` (the stepping thread works on
them too), so a pool that is busy only makes the step slower. The pool must outlive the world. A
world is used from one thread at a time.

## Queries

All are `const`, return values (no callbacks) and take a `QueryFilter3D {mask, ignore, sensors}`: bodies whose layer is in the
`Layer3DMask`, not the `ignore` body, and (unless `sensors`) not sensors. Sensors are left out by default so that a ray meant for
floors and walls does not stop at a trigger volume.

| Call | Gives |
|---|---|
| `raycast(from, to, filter)` | the first hit: body, user id, point, unit normal (out of the body; a mesh's front side), fraction, distance, sub shape (the index of the compound child that was hit) |
| `raycast_all(from, to, filter)` | every hit (a body once for each sub shape the ray crosses), nearest first; ties by body number, then sub shape |
| `shape_cast(shape, position, rotation, displacement, filter)` | the first thing the swept shape meets: fraction (time of impact), where the shape is then, the point on the hit body, the normal toward the shape, penetration (above 0 when it started overlapping, fraction 0). Not for meshes or height fields |
| `overlap(shape, position, rotation, filter)` | the bodies it overlaps, one entry each, **sorted by body number**, with the deepest contact |
| `point_inside(point, filter)` | bodies whose solid contains the point, by body number |
| `closest_point(point, max_distance, filter)` | the nearest surface within the distance (a point inside a solid is at distance 0) |

A ray that starts inside a convex solid hits it at fraction 0; a triangle mesh is hit from either side (its normal is its front's).
Order never depends on pointers or hash maps: the same world answers the same in every run and on every worker count.

## Contact events

`step()` collects what Jolt's contact callbacks saw (under a lock, on Jolt's threads; nothing of the game runs there) and turns it,
after the step, into `ContactEvent3D`s: `a` and `b` (the body with the lower number first), their user ids, `kind` (`Begin`,
`Persist`, `End`), `sensor`, the mean contact `point`, the `normal` from a to b, `closing_speed` (along it, before the solver ran) and
the manifold's point count. Each step's batch is sorted by (a, b, kind), batches in step order; they wait in `events()` until
`drain_events()` takes them (`dropped_events()` counts what `max_events` pushed out).

* One `Begin` and one `End` for a body pair however many sub shapes (compound children, mesh triangles) touch.
* A pair that **falls asleep together is still touching**: Jolt drops such contacts, but no `End` is reported and no second `Begin`
  when they wake and still touch. `End` comes when a woken body leaves, or a body is destroyed (the next step reports it).
* Sensors give `Begin` and `End` with `sensor = true`. A static sensor only sees awake bodies; a kinematic sensor that is awake sees
  sleeping ones too (Jolt's rule). A kinematic body at rest sleeps like any other.
* `World3DConfig::contact_events` (default on; off installs nothing), `persist_events` (a `Persist` for every touching pair each
  step: for tools; costs a record a pair a step).
* A constraint can break (below); `drain_constraint_events()` reports them.

## Constraints

`create_constraint(Constraint3DDesc)` makes one from the bodies as they are now (anchors and axes in the world; `a` invalid is the
world); handles are `{index, generation}`. Kinds: `Fixed`, `Distance` (min and max, a negative bound is the length it has; with
`spring_frequency` the limits give), `Point`, `Hinge` (axis, limits in degrees from the pose at creation, friction, motor),
`Cone` (a swing cone and twist limits: a shoulder), `Slider` (axis, limits in metres, friction, motor). `set_constraint_motor(c, mode,
target, max_force)` (velocity or position) for hinge and slider. `break_force` / `break_torque` (0: never): above it in a step the
constraint is disabled and reported (the force is the last collision step's impulse over its length: an estimate). Bodies a
constraint joins do **not** collide with each other unless `collide_connected` is set (a door and its post, links of a chain).
`constraint_info` gives the anchors and axis as they are now and the hinge angle, slider position or distance. Constraints go with
their bodies when one is destroyed. They are in `state_hash()`, `save_state()` and `restore_state()` (which needs the same set).

## Debug draw

`DebugDraw3D` (`kin/physics3d/debug_draw3d.hpp`) makes coloured line segments in world space, as plain data, from kin's own view of
the shapes (no debug renderer is built into Jolt): `world(w)` draws every body (static grey, awake dynamic green, kinematic cyan,
asleep blue, sensors yellow) and constraint, `contacts(events)` crosses at contact points and their normals, `ray(from, to, hit)`,
`shape_cast(...)`, `shape(...)`, `line(...)`. Boxes are twelve edges, spheres three circles, capsules and cylinders their rings
and sides, hulls their faces, meshes and height fields their triangles up to `max_triangles_per_shape` (an even stride) and
`max_lines`. Options carry the colours.

In a 3D view: `WorldPhysicsState3D::collect_debug(renderer, scene)` (below), or `draw_debug_lines(renderer, scene, lines)` for
any list: an unlit wireframe mesh, one draw, depth tested; each line is a sliver triangle (a, b, c with c a pixel or so off b: a
software rasteriser such as lavapipe drops a triangle of no area, so the (a, b, b) lines of the wireframe material's own comment vanish
there). Each end is also moved toward the eye along its view ray by 0.2 % of its distance (at least 3 mm): it shows at the same pixel
but wins the depth test against the face it edges. A sphere's circles still show dotted where the facets of its 24 segment mesh lie
outside them. The picture is the same on NVIDIA and lavapipe (tested). Off by default (`debug_draw = false`); off, no mesh is
made and the picture is byte for byte what it was.

## Entities

```cpp
#include <kin/ecs/physics3d.hpp>

kin::World3D physics;
kin::WorldPhysicsState3D bodies{ecs_world, physics};   // flecs::world& or EcsWorld&
world.entity().set(kin::Transform3D{.position = {0, 5, 0}})
              .set(kin::Collider3D{.kind = kin::Collider3D::Kind::Box, .half_extents = {0.5f, 0.5f, 0.5f}})
              .set(kin::RigidBody3D{.density = 600.0f});
bodies.advance(frame_seconds);               // or bodies.step(fixed_dt) from the engine's fixed step
for (const kin::ContactEvent3D& e : bodies.events()) { auto entity = bodies.entity_of(e.user_a); }
```

* `Collider3D`: `Box`, `Sphere`, `Capsule`, `Cylinder`, `ConvexHull` and `TriangleMesh` (from a shared `MeshData`), `Compound`
  (`children`, each with its own offset and rotation); `offset`, `rotation`, `scale`; or a ready `Shape3D` shared between entities.
  `RigidBody3D`: motion, mass or density, friction, restitution, damping, gravity factor, layer, sensor, sleeping, continuous,
  initial velocities. An entity with Transform3D and Collider3D has a body: a static one without RigidBody3D.
* Bodies are made, in **ascending entity id order**, at the next `sync_bodies()` / `step()` / `advance()`, where the entity is in the
  world with its **world scale applied** to the collider's sizes, and destroyed when the entity, its Collider3D or its Transform3D
  goes. Body numbers and so the step are the same in two runs that do the same. A body's user id is the entity's id.
* **Bodies are world level.** A dynamic body of an entity with a parent is not carried by the parent: it is where the simulation
  puts it, and the entity's Transform3D (its place from the parent as it is now) is rewritten after each step; the parent moving
  leaves the body behind. A kinematic body follows its entity (parents included) and a static one is moved when its entity is.
* Dynamic bodies write position and rotation (not scale) into Transform3D after the step; a sleeping body is not written. Game code
  that changes a dynamic entity's Transform3D between steps moves the body there (velocities kept). `interpolate(alpha)` (opt-in,
  call after `advance()` with `alpha()`) writes the pose between the last two steps.
* Changing a live body's components does nothing until `rebuild(entity)`; changing `RigidBody3D::motion` rebuilds by itself.
* The events of the last `step()` (all the steps of one `advance()`) are in `events()` and on the ECS world as the singleton
  `PhysicsContacts3D`. `body_of(entity)`, and the `PhysicsBody3D` component on the entity, name its body.
* `collider_from_mesh(entity)`: the GPU `Mesh` keeps no triangles, so a mesh entity gives only a box about its bounds;
  `collider_hull_from_mesh_data` and `collider_triangles_from_mesh_data` make a hull or a static triangle mesh from the `MeshData` the
  mesh was made from.
* `ground_query_from_world(world, mask, ignore)` is a `GroundQuery` for foot placement (`IKConstraint::ground`): a raycast.

## Character controller

`kin/physics3d/character3d.hpp` and the `World3D::*_character*` calls: a capsule controller on Jolt's `CharacterVirtual`, with its
feet as the origin. The step is a pure function of (its state, the command, dt): no clock, the same bits with no workers, 1 or 4, and
in `state_hash()`, `save_state()` and `restore_state()` (a restored world repeats the exact trajectory).

```cpp
kin::CharacterDesc3D desc;                       // radius 0.3, height 1.8, mass 70, slope 45, step 0.4, stick 0.5, walk 4 m/s, jump 5 m/s
desc.position = {0, 1, 0};
const kin::Character3D hero = world.create_character(desc);
...each fixed step:
world.set_character_input(hero, {.move = {0, 1}, .forward = camera.forward(), .jump = pressed, .sprint = held});
world.step(dt);                                  // characters first (in number order), then the world
const kin::CharacterState3D s = world.character(hero);   // position (feet), velocity, ground, ground_body, ground_speed, ...
for (const kin::CharacterEvent3D& e : world.drain_character_events()) { /* Landed, LeftGround, HitHead, ContactBegin, ContactEnd */ }
```

* **Command.** `CharacterInput3D`: `move` (x right, y forward, each -1 to 1, cut to length 1), `forward` (a world vector, the camera's or
  the character's; its part across `up` is used), `jump` (a level: a jump happens on a rising edge), `crouch`, `sprint`. Held until
  set again. Never raw device state: it is what a netcode layer sends.
* **Descriptor.** Capsule `radius`, `height`, `crouch_height`; `mass` and `push_strength` (N: a body that needs more to move is a
  wall); `max_slope_degrees` (steeper ground is slid down), `max_step_height` (0 = no stairs), `step_down_distance` (the stick to the
  ground: 0 = it hops over humps), `walk_speed`, `sprint_speed`, `crouch_speed`, `acceleration`, `deceleration`, `air_acceleration`,
  `ground_friction` (scales acceleration and braking: 0.05 is ice), `wall_friction` (Coulomb against walls it is pressed to; 0 slides
  along a wall at full speed), `gravity_scale`, `jump_speed` (apex = v^2 / 2g), options `coyote_seconds` and `jump_buffer_seconds` (off),
  `up` (zero = against the world's gravity), `up_blend_rate`, `layer` (default `layers3d::Character`) and `collides_with` (a mask), `user_id`.
* **Order.** `World3D::step` updates the characters BEFORE the world's step, in ascending handle number (Jolt's own order: a
  character is swept against the world as the last step left it; a kinematic platform already holds this step's velocity, so a
  character on it is carried exactly; what it pushes gets its impulse before the solver). Two characters that want the same room: the
  lower number moves first.
* **State.** `character(c)`: `position` (feet), `velocity` (what the last step achieved: the ground's included; a stair shows as a short
  spike), `up`, `ground` (`OnGround`, `OnSteepSlope`, `Sliding`, `InAir`), `ground_body` / `ground_user_id` / `ground_normal` /
  `ground_velocity`, `ground_speed` (m/s along the surface it stands on, relative to it: an animation blend's input), `vertical_speed`,
  `crouching`, `stand_blocked`, `air_time`, `height`, `body`, `jumped`. `character_contacts(c)` lists what it touches (by body number;
  point, normal toward it, sensor, walkable).
* **Up and gravity.** Gravity for a character is `|world gravity| x gravity_scale` along its own `-up`. Default: up is against the world's
  gravity and follows `set_gravity`; `set_character_up(c, v)` makes it explicit (a wall, a ceiling, a planet's surface). It turns toward
  the target by `up_blend_rate` a second (normalised blending, no trigonometry), so walking onto a wall is a turn, not a jump.
* **Crouch.** `input.crouch` decides each step; standing is tried every step while wanted and the head clearance is checked (it stays
  crouched under a low ceiling and `stand_blocked` says so). `set_character_crouch` does it at once and says whether it could.
* **Its body.** A kinematic capsule in its layer, never asleep (Jolt's inner body): other bodies are stopped by it, sensors report it,
  queries hit it (`QueryFilter3D::ignore = state.body` for a foot ray). It is one of the world's bodies (`body_count()`, `bodies()`, the
  hash, the debug draw); `destroy_body`, `set_position` and the other body calls refuse it. Two characters stop each other; they do not
  push each other.
* **Layers.** The body is in `desc.layer`; the world's layer table decides what else collides with it; `collides_with` masks what its own
  movement hits (a layer left out is walked through).
* **Events.** `Landed` (`speed`: how fast it was falling, `body`: the ground), `LeftGround` (`jumped`), `HitHead` (`speed` going up),
  `ContactBegin` / `ContactEnd` (the body and its user id; `sensor` for a trigger volume). Each step's batch is in character order, then in
  the order of the kinds.

### Entities

`CharacterController3D {desc, input, facing, apply_facing, state}` on an entity with `Transform3D`: made by the same system
(`WorldPhysicsState3D`) after the bodies of the sync, in ascending entity id order, from the entity's world place (a `Collider3D` on the
same entity makes no second body). The game sets `input` (and `facing`); the system sends it, steps, writes the feet into
`Transform3D::position`, `facing` into its rotation (physics never turns a character) and the state into the component. Moving the
entity from game code is a teleport. A character under a parent is rewritten as its place from the parent, as a body is.
`character_events()` and the singleton `CharacterEvents3D` carry the events with entity ids. A kinematic platform's entity moved by game
code carries a character standing on it (`follow_transform`). `rebuild(entity)` makes the character again from the descriptor.

**Animation.** `locomotion_animator()` is an `Animator3D` with a `ground` state (a 1D blend over `speed`: idle, walk, run clips) and an
`air` state (a fall pose), crossfading on `airborne`; add a `CharacterAnimationLink3D` to the entity and the system sets `speed` (the
ground speed), `airborne`, `crouching` and `vertical` after each step, and hands the animator's foot placer constraint `feet` the
physics world as its ground (`ground_query_from_world`, the character's own capsule and sensors left out) with weight 1 on the ground
and 0 in the air. `drive_character_animator(animator, state, world)` does the same by hand.

### The test level and the harness

`tests/character_level.hpp` builds the level every test, the picture and the bench use: a floor, a ramp of 20 degrees up to a plateau
and one of 35 down, a round hump, six stairs of 15 cm, a 60 cm ledge, a 50 degree ramp, a low tunnel, a light box and a heavy one, a
platform that moves along X, one that turns, a trigger. The kin_reference scene `physics_character` runs a scripted route through it
(commands, teleports between pieces, checkpoints: boxes the feet must be in at a frame) with a skinned person
(`made/skinned_biped_walk.glb`, made by `tools/reference/make_scenes.py`), and `kin-physics3d-character` runs the same route from the
same JSON with no GPU. `kin_physics3d_character_tests -r scene.json N` prints where the character is every N frames (to write
checkpoints from).

### Limits

Jolt's capsule rolls over a step's corner, so the highest step climbed is about 0.1 m above `max_step_height` for this capsule. It has
no friction of its own (the acceleration limits and `wall_friction` are kin's). The orientation of the capsule about its axis is not
meaningful and is not kept; a character never turns itself. Character against character is blocking only, no pushing. Cross-platform
determinism is Jolt's claim (only this machine was run). The viewer's level is on its physics floor only.

## Grab and climb

`kin/physics3d/traversal3d.hpp`, `World3D::probe_grips`, `set_traversal_input`, `traversal`, `set_surface_material`, `set_hold`:
a general way for a character to hang from the world by its hands, shimmy along it, pull up over a ledge, jump off and let go.
Nothing in it knows about a mountain or a game: a surface is any collider, and what makes one grippable, slippery or tiring is data
on its body. Off unless `CharacterDesc3D::traversal.enabled`; with it off a character is the P2c controller bit for bit (its state is
not in the hash, no byte of it). `enabled` is fixed at creation (it is in the saved state's layout); every other number is read live.

```cpp
kin::CharacterDesc3D desc;
desc.traversal.enabled = true;                          // two hands (limb_count 2), reach 0.8 m, 100 units of stamina
const kin::Character3D hero = world.create_character(desc);
world.set_surface_material(ice, {.slip = 1.0f, .drain = 2.0f, .tag = 1});     // data, not code
world.set_hold(rung, {.quality = 0.8f, .snap = true, .anchor = {0, 0, 0}});   // an authored hold
...each fixed step:
world.set_character_input(hero, {.move = {0, 1}, .forward = wall_direction}); // hanging: x along the surface, y up it
world.set_traversal_input(hero, {.grab = {left_held, right_held}, .reach = camera.forward(),
                                 .pull_up = f, .jump_off = space, .drop = x});
world.step(dt);
const kin::TraversalState3D t = world.traversal(hero);  // mode, the grips, the resource
```

* **Four layers.** (1) *Probe*: `probe_grips({origin, direction, up, reach, radius, spread, mask, ignore, ...})` casts a fan of seven
  sphere sweeps (`shape_cast`) and refines each hit with a ray (`raycast`) against the layers of `mask`; the hits go through the
  providers (hold, then ledge, then surface: a hit is one candidate) and come back best first: `{body, user_id, point, normal, right,
  surface_up, kind, quality, score, surface_tag, drain, slip, distance, fixed, ledge, ledge_top, clear}`. Pure: nothing is held.
  (2) *Grip*: one per limb (`GripState3D`: phase `Free`, `Reaching`, `Held`, `Slipping`, `Released`), its anchor stored in the
  GRABBED BODY'S OWN SPACE, so a hold on a platform or a crate goes where it goes; `strength` = quality x the surface's grip x the
  exhaustion fade. (3) *Body*: `Walking`, `Hanging`, `Mantling`. Hanging is kinematic: the feet are driven from the anchors (below the
  mean of the held anchors by `hang_drop`, a capsule's width out from the surface, the held limbs' shoulders under their hands) through
  Jolt's swept move, with a damped swing about that place (a pendulum across `up`, stiff arms along it). (4) *Resource*: `Resource3D
  {value, max, delay, exhausted}` with `step(config, dt, drain_per_second)` and `take(config, amount)`; stamina for the grips and
  what sprinting spends.
* **Providers** share one interface (`GripProvider3D`: `probe`, `attach`, `move`, `detach`; `grip_provider(kind)`): the *surface*
  (any collider whose material is grippable and whose normal is not floor-like), the *ledge* (a ray down just inside the surface finds
  the top edge within `ledge_search_height` of the hit; the hand goes to the lip; `clear` says the capsule fits over it and has a floor
  to stand on; the underside of a ledge counts: the edge is on the hand's side) and the authored *hold* (`HoldDesc3D`: quality, drain,
  `snap` to a point of the body, `slide`; taken from the front, not the side the sweep grazed). A rope would be another provider.
* **Surface materials.** `SurfaceMaterial3D {grippable, grip, slip, drain, tag}` on a body: `grippable` false takes it out (a smooth
  wall; its holds are bodies of their own); `slip` (x `slip_speed` m/s) slides a held grip down the surface and lets it go after
  `slip_release_seconds` or at the end of the surface; `drain` multiplies the resource's drain while a hand holds it; `tag` is yours
  (reported in candidates, grips and events). Not in the hash: it is the level's setup, set it at creation.
* **Commands.** `TraversalInput3D`: `grab[limb]` (a level), `reach` (a world direction; zero: the way it faces), `pull_up` and
  `jump_off` (rising edges), `drop` (a level). A hand the character let go of (exhausted, slipped, dropped, jumped off) takes hold
  again only after its `grab` is given up and given again. While hanging `CharacterInput3D::move` is x along the surface (shimmy,
  `shimmy_speed`) and y up it (`climb_speed`): every hand moves together or none (a hand at the end of a wall stops the others), a
  hold with `snap` does not move, and a hand going up a face finds the lip within 12 cm and takes it.
* **Mantle.** `pull_up` over a ledge grip: the capsule must fit over the top and have a floor, and be free where it comes over the
  edge and all the way in (an overlap and a sweep) or it stays hanging with `MantleBlocked` (`NoLedge`, `NoRoom`, `Tired`). Else a
  deterministic move of `mantle_seconds` in the ledge body's own space (up the wall, then over the edge, smoothstep), swept so that it
  cannot pass through anything, then walking: `GripReleased` (`Mantle`) for each hand and `MantleFinished`.
* **Load.** A held DYNAMIC body gets the character's weight (`mass x |g| x gravity_scale` along -up, shared between the hands) as a
  force at each anchor every step: hanging from a loose crate moves and tilts it, and the character follows the anchor. A kinematic
  body carries the character (the anchor is predicted over the step: no lag).
* **Resource.** Hanging: `drain_one_hand` (12 a second) for one hand, `drain_two_hands` (7) for two or more, `+ drain_moving`, times the
  mean of the held surfaces' `drain`; standing on walkable ground costs `ground_drain_factor` of that (0: rest, and it comes back);
  `grab_cost` and `pull_up_cost` once; sprinting `drain_sprint`. It comes back at `regen_rate` after `regen_delay` seconds without being
  spent; it is `exhausted` at 0 until `recover_fraction` of `max` is back (no grip and no sprint while it is). Exhausted and hanging: the
  weakest hand's strength fades to 0 over `exhaust_fade` and it lets go, the next `exhaust_interval` later.
* **Events** (in `character_events()` with the others, per step in character order, the traversal ones after the walking ones
  sorted by kind and then limb): `Gripped` (`hand`, the body, point, normal), `Slipped` (`speed`), `GripReleased` (`reason`:
  `GripRelease3D`), `Exhausted`, `MantleStarted`, `MantleFinished`, `MantleBlocked` (`reason`: `MantleBlock3D`), `Fell` (let go of
  everything and not by a jump or a pull-up).
* **Gravity** is the character's `up`: every direction, frame and force above uses it, so a wall treated as a floor climbs the same.
* **State.** The grips (phase, kind, body, anchor, quality, drain, slip, timers, latches), the resource, mode, swing velocity, hang
  place, the mantle's path and body, the command and its edges are in `state_hash()`, `save_state()` and `restore_state()`.
  `World3D::traversal(c).grips[i].point` is where the anchor is NOW (after the bodies' step).

### Entities

`CharacterController3D::desc.traversal.enabled`, `::traversal` (the command, sent each step) and `::traversal_state` (written);
`SurfaceMaterial3D` (the physics type is the component) and `Hold3D` on an entity with a collider, read when its body is made
(`rebuild(entity)` reads them again); `WorldPhysicsState3D::traversal_of(entity)`. The system turns a hanging character to face the
surface (`controller.facing`, slerped). With `CharacterAnimationLink3D::hands = {"hand_L", "hand_R"}` (names of two-bone IK
constraints with the hand as their end, by limb number) each hand's constraint gets the anchor as its target (`hand_clearance` off
the surface), the surface's frame as its rotation (`align_hands`, `hand_offset`) and weight 1 while the limb reaches or holds, 0 when it
is free or let go (the constraint's own `fade_seconds` makes the hand come and go), and `hanging` is a parameter; `locomotion_animator`
takes a `hang` clip (empty: the `fall` pose plays while it hangs). `DebugDraw3D::grips(world, character)` and `candidates(...)` draw
the anchors (yellow reaching, green held, orange slipping), normals and frames, the line to the body, the hang place, and a probe's
candidates by provider; `WorldPhysicsState3DConfig::debug_grips` adds the first to `collect_debug`.

### The climb level and the harness

`tests/climb_level.hpp`: a 4 m wall with a ledge, a 5 m wall with a roof reaching out at 3 m, a smooth wall that cannot be gripped with
four holds on it and a lip of rock standing out on top, a kinematic slab that moves along X (a hold), a pedestal with a 30 kg crate a third
over its edge, a 7 m wall of ice (slip 1, drain 2), a ledge at 3 m under a roof 1.4 m over it. The kin_reference scene `physics_climb`
runs a scripted timeline of commands through it (`tests/reference/scenes/physics_climb.json`: grab per hand, reach, move, pull up,
jump off, drop, teleports, stamina; checkpoints with the feet's box, the mode and how many hands hold) with the biped, and
`kin-physics3d-traversal` runs the same timeline from the same JSON with no GPU. `kin_physics3d_traversal_tests -r scene.json N` prints
where the character is every N frames (to write checkpoints from) and the events.

### Limits

The probe's casts stop at the first thing they meet: a grippable thing exactly flush behind a smooth wall cannot be seen (the lip of the
test level stands 20 cm out); a ledge is found on the body that was hit (a top made of another body is not). A hand takes the aimed
point, not "the best hold": reach and the command are the game's. No feet on the wall: the legs dangle (the `fall` pose, or a clip), and
P1b's foot placement is not active while it hangs. Two grips on one hold are allowed more than 0.2 m apart. Ropes, ladders as a mode of
their own, and a mantle that is interrupted are not built. The keyboard and mouse path of the viewer is not exercised by any test.

## Cost

`kin_physics3d_bench` (built with the benchmarks): 500 boxes in a pile step in 1 to 4 ms, 2000 in
about 4 to 10 ms with 4 workers; the table is in the plan. Jolt builds in about 18 s with `-j8` and is
a 3.8 MB static library.

## Build

Jolt is fetched by `cmake/dependencies.cmake` (`v5.5.0`, only with `KIN_EXPERIMENTAL_3D`), or taken
from a clone with `-DFETCHCONTENT_SOURCE_DIR_JOLTPHYSICS=<path>`. It is built static, single
precision, with no samples, tests, viewer, debug renderer, profiler or object stream. Its glue is
compiled as its own object library (`kin_physics3d_objects`) so that Jolt's instruction-set flags do
not reach the rest of the engine. Its licence is installed beside games with the other third-party
texts ([third_party_notices.md](third_party_notices.md)).

Queries cost 1 to 8 microseconds against a 2000 body pile and 1 to 7 against a 100,000 triangle mesh; contact events add under 1 % to
a 500 body step (persist events 6 %); the entity system adds 0.2 to 0.3 ms a step for 500 bodies. Table in the plan ("P2b").
A character costs about 2 to 5 microseconds a step in the test level (100 of them 0.2 ms alone, 0.7 ms beside a heap of 500 boxes
they wake); `kin_physics3d_bench characters`, table in the plan ("P2c"). A grip probe is about 14 to 16 microseconds, a hanging
character 4 to 5 microseconds a step; `kin_physics3d_bench climbing`, table in the plan ("P2d").

## Not yet

Ragdoll; ropes (the grip providers' interface is ready for one); soft bodies; per-contact material callbacks; continuous collision
between dynamic bodies beyond Jolt's linear cast; scaled or mutable shapes (swap with `set_shape`).
