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

## Ragdolls

`kin/physics3d/ragdoll3d.hpp`, `World3D::create_ragdoll`, `start_ragdoll`, `blend_ragdoll`, `ragdoll_get_up`, `ragdoll_pose`,
`ragdoll_get_up_place`, ECS `Ragdoll3D`: a body per bone of a skinned character, joined by constraints, that takes the pose over from a
clip and gives it back. PASSIVE: nothing drives the bodies (no motors); the pose is what physics makes of them, and a "get up" is a
blend back to a clip (which clip, and when, is the game's). Everything is ordinary bodies and constraints (made through the world's
public calls, in bone order), so queries, events, grips and the debug lines already see them.

```cpp
kin::RagdollDesc desc = kin::auto_ragdoll_desc(skeleton, skeleton.rest_pose());   // capsules from the bind pose
const kin::RagdollHandle3D r = world.create_ragdoll(desc, bone_world_matrices);    // starts Animated: kinematic bodies at the pose
...each fixed step:
world.set_ragdoll_targets(r, clip_pose_in_the_world);   // what the kinematic bodies follow
world.step(dt);
world.start_ragdoll(r, {.blend_seconds = 0.15f, .set_velocity = true, .velocity = v, .impulse_bone = chest, .impulse = hit});
world.ragdoll_pose(r, bones);                           // per bone: share, place (what a pose writer takes)
const kin::RagdollPlace3D at = world.ragdoll_get_up_place(r);   // the ground under the pelvis, its facing, lying face up or down
world.ragdoll_get_up(r, 0.8f);                          // blend back to the clip over 0.8 s
```

* **Building.** `RagdollDesc` is per bone (shape capsule, box or sphere; radius, half height, offset and rotation in the BONE'S frame;
  mass or density; friction, damping; the joint to the parent: swing-twist with a swing half angle and twist range, a hinge with limits
  in degrees from the pose it was made in, or fixed; the share `weight`) and for the ragdoll (layer, `self_collision`, `continuous`,
  `animated_bodies`, settle thresholds, `freeze_on_settle`, solver iterations, the root bone and the way it faces). The body of a bone
  is at its joint with the joint's rotation, so that the bone's pose is the body's pose exactly. `auto_ragdoll_desc(skeleton, pose,
  options)` makes the capsules: from a joint to its main child (the one that continues the parent's direction; the longest for the
  root), a short capsule for a leaf (a foot that points down is laid forward along the ground), radius a share of the bone's length
  (`radius_ratio` 0.22, kept between 2.5 and 12 cm), the mass of the whole (70 kg) shared by volume, swing-twist joints with default
  limits and hinges where a name matches the override table (knee, elbow: for a character facing +Z in a T pose; another rig brings
  its own), bones shorter than 2 cm and named ones skipped (a bone's parent is the nearest one with a body), from the joint called
  `pelvis` down. Neighbours never collide with each other (joined: `collide_connected` false); all the bones of one ragdoll are in a
  collision group and skip each other too unless `self_collision` is set (then only neighbours skip).
* **States.** `Animated`: kinematic bodies that follow the targets (so characters and projectiles can hit them; `animated_bodies`
  false parks them: no collisions, asleep, until the ragdoll starts). `Ragdoll`: dynamic, the pose is the bodies', blending in over
  `blend_seconds` (smoothstep). `Blend`: dynamic, a held share (`blend_ragdoll(alpha, seconds)`; 0 reached means Animated again).
  `GetUp`: the bodies are back to kinematic on the clip at once and the POSE blends from where the bodies lay (a snapshot) to the clip
  over `seconds`, then Animated and a `Recovered` event: nothing fights the character's capsule or the floor while it stands up. A
  per-bone weight (`set_ragdoll_bone_weights`) multiplies the share; weight 0 keeps a bone kinematic in every state, and
  `set_ragdoll_subtree(bone)` makes a PARTIAL ragdoll (an arm, a tail): the rest stays animated, bit for bit.
* **Settling and cost.** Dynamic bodies sleep. `Settled` is reported once a start, when every dynamic body is asleep or slower than
  `settle_speed` / `settle_angular_speed` for `settle_seconds`; `freeze_on_settle` then makes them kinematic and puts them to sleep:
  hittable, free, until the ragdoll is started, blended or told to get up. 17 bodies a person: see "Cost" below.
* **Events** (`ragdoll_events()`, `drain_ragdoll_events()`; user id = the entity's id from the ECS): `Started`, `Settled`, `Recovered`
  and, from the ECS helper, `Blocked`; each step's batch is sorted by (ragdoll, kind).
* **State.** The mode, blend (from, to, time, length), settle timer, flags, per-bone weights, targets and the get-up snapshot are in
  `state_hash()`, `save_state()` and `restore_state()` (bodies and joints are in them already); a restore puts the bodies' motion
  types back before Jolt restores their places. With no ragdoll nothing of it is in the hash.

### Entities and the pose

An entity with `Transform3D`, `SkinnedMeshRenderer` and `Ragdoll3D` gets a ragdoll, made at the next sync after the bodies and the
characters in ascending entity id order, from the model's skeleton at its rest pose (`Ragdoll3D::desc` with bones is used as given,
`auto_options` otherwise), at the entity's place and scale; an `Animator3D` is added if there is none. Each step the system sets the
targets from the animator's clip pose (IK is the renderer's, solved at draw time, and is not followed by the kinematic bodies), steps,
and writes the bodies' pose into `Animator3D::pose_override` (`PoseOverride3D`: per joint a share and a world place). With every
share 0 the override is cleared: an animator is exactly what it was. `WorldPhysicsState3D::interpolate(alpha)` interpolates the
override between the last two steps. Commands: `start_ragdoll`, `blend_ragdoll`, `ragdoll_get_up`, `animate_ragdoll`,
`set_ragdoll_subtree(entity, joint_name)`.

**Where the pose is written.** In `WorldRenderState3D` (`collect()` and `apply_ik()`), AFTER the clips, the layers and the IK
constraints, in this order, and BEFORE the joint matrices: ragdolled bones take the body's ROTATION (the world rotation turned into
the parent's space through the parent's final matrix) and a joint at the top of the ragdolled part (its parent has less share)
also its place; every other ragdolled joint keeps the bind pose's translation, so bone lengths and scale are exactly the bind
pose's (the physics joints open a little under a hard hit: the pose does not follow that). A share between 0 and 1 is a slerp and
a lerp from the pose the clips and IK made. Drawing, the TAA velocity (the renderer keeps the previous frame's matrices by the
entity's id), the shadow pass and the shadow cache all read the joint matrices, so they need nothing of their own. IK that
should hold on a ragdolled limb is overridden by it (physics wins); IK on a limb that is not ragdolled is exactly what it was.

**From controller to ragdoll and back** (`hand_over_to_ragdoll`, `recover_from_ragdoll`). A hand-over disables the character's
capsule (`World3D::set_character_enabled(false)`: it is not updated, collides with nothing, and the system teleports it to the
pelvis's ground point each step, so that the entity's `Transform3D` follows the ragdoll), gives every body the character's
velocity and starts the ragdoll (an impulse at a bone on request). A recovery finds the ground under the pelvis (along the
character's up, within `max_ground_distance`), checks that the STANDING capsule fits there (an overlap that is not the ragdoll's own
bodies; `allowed_penetration` 1 cm), and then enables the character at that point facing as the pelvis says (`facing`), and
starts the get-up. No ground or no room: nothing changes, a `Blocked` event, `blocked` in the result. A character that has a
ragdoll shares its collision group with it, so neither its own movement nor its queries (grip probes, the foot placer's rays)
meet its own bones.

### Limits

Passive only: no motors, no active ragdoll (a character that keeps its balance or holds on by its muscles). The animated bodies
follow the CLIP's pose, not the IK's (a hand moved by IK is not where its body is). The joint limits are hard but a violent start
(tens of m/s) overshoots them for a few steps; a hit of 200 N s at a chest opens joints by several centimetres. A pose is rigid: the
joints' opening does not show. The auto desc is for a biped with the knees and elbows of the default table (another rig brings
the table and may need a different `root_name`); capsules from the bind pose do not know a belly. The get-up place is a place
and a facing: choosing the clip (face up or face down), pose matching and the character's move there are the game's.

## Ropes

`kin/physics3d/rope3d.hpp`, `World3D::create_rope`, `rope_points`, `rope_closest_point`, `rope_tension`, `reel_rope`, ECS `Rope3D`,
`WorldPhysicsState3D::collect_ropes`: a chain of capsule segments joined by ball joints (point constraints), between two anchors or
hanging from one. A rope is ordinary bodies and constraints (in the rope's collision group: its segments never meet each other
unless `self_collision`), so grips, loads, queries and events already see it.

```cpp
kin::RopeDesc3D desc;
desc.start = kin::RopeAnchor3D::at({0, 6, 0});                 // a point of the world; ::on(body, offset) in that body's space; ::free()
desc.end = kin::RopeAnchor3D::on(crate, {0, 0.3f, 0});
desc.length = 4.0f;                                            // 0: the distance between two anchors (taut); longer sags
desc.mass_per_metre = 1.0f; desc.radius = 0.04f;
const kin::RopeHandle3D rope = world.create_rope(desc);
world.reel_rope(rope, 2.0f, 0.5f);                             // to 2 m at 0.5 m/s
world.rope_points(rope); world.rope_tension(rope, kin::RopeEnd3D::Start);
```

* **Why ball joints.** Point constraints between capsules are the most stable arrangement for the mass ratios of a climber on a
  rope (80 kg on 0.25 kg segments); swing-twist or hinge limits would add bend stiffness at a cost of iterations and cannot be
  re-anchored in place, which pay-out and reel need. A rope has therefore no bend or TWIST stiffness: a box lying across one rope
  rolls it like a roller and walks off it in time (a bridge is two or three ropes between low walls, or boards).
* **Solver iterations are what holds the length.** Jolt's iterations are per island; the rope's constraints ask for more
  (`velocity_steps` 128, `position_steps` 32 by default). Stretch of a 4 m rope of 16 segments under a load (last 4 s of 10 s, this
  machine): 10 kg 1 %; 50 kg 3 %; 150 kg 9 % at the defaults. For an 80 kg load: world default (10, 2) a quarter and more, 24 and 4
  a sixth (final 17 %, worst 26 %), 64 and 16 5 % (worst 9.5 %), 96 and 24 3 %, 128 and 32 2.4 % (worst 4.6 %), 24 and 4 with
  the world sub-stepped 4 times 0.8 %. Fewer, longer segments help the same way (8 segments at 64 and 16: 1 %). A rope that carries
  nothing is fine with 16 and 4. The cost is in "Cost".
* **Creation.** Anchored ends need a length (or two anchors for a taut rope); a free end hangs along gravity (or
  `hang_direction`), a rope longer than the gap sags as a parabola of that length (`sag_direction`); the segments are cut at equal
  arclength; `segments` or `segment_length`; `collide_world` false lets it through static bodies; `break_tension` makes every
  joint and anchor breakable at that force.
* **Length.** A rope has a fixed number of segments; `set_rope_length` or `reel_rope(target, speed)` stretches them all alike (their
  capsules and the joints' anchors in place; the chain is re-laid along its own polyline, bodies teleported, velocities kept).
  Paying out is smooth. REELING IN a body anchored at the moving end is done by carrying it: a chain of light segments cannot
  lift a heavy load by constraint correction (the solver moves the light segments and not it: a 40 kg crate rose a third of
  the way in 5 s, a crate on the floor not at all), so while the rope is reeled in the anchored body is KINEMATIC and put
  where the chain's end is each step, and dynamic again (at rest) when the length is reached, the reel is stopped or it turns
  to pay out; the state (hoisting) is in the hash and the saved state. A body that was kinematic already is left alone.
  Mass per metre is for the length the rope was made at.
* **Tension.** `rope_anchor_force` / `rope_tension(end)`: the anchor constraint's impulse of the last collision step over its length,
  smoothed over a quarter of a second (a single step's reading wanders by a tenth: the solver's residual and a load's bounce),
  positive when the rope pulls its anchor toward itself. A rope that is asleep keeps its last reading. At rest it reads the weight
  within about 10 %.
* **Sampling.** `rope_points` (segments + 1 joints with the direction there), `rope_segments`, `rope_closest_point(world point)`
  (segment, `t` along it, `along` the whole rope, point, tangent, distance), `rope_point_at`, `rope_segment_of(body)`.
* **Grips.** `GripKind3D::Rope`: a hand finds a rope within its reach (the closest point of the reach to the rope, within the hand's
  tolerance and the fan), up to three candidates a rope (the closest, one hand's width up it, one down it) so that two hands take two
  places. The anchor is a segment's body and a SHARE of the segment's length from its middle, so that it keeps its place along a
  rope that changes length. Climbing moves every hand along the rope at `climb_speed` (its direction is the move command's along
  the rope; a hand stops at the end), onto the next segment's body as it passes. While hanging the character's feet are driven
  from the hands as at a wall, with a quick critically damped spring (no pendulum of its own: the rope swings) and a rope's radius
  further out; `jump_off` leaves with the speed it has (the swing) and the up kick, away from nothing; letting go is letting go.
  **The weight is on the rope.** A rope is light, so a force alone would not make it swing: the character's mass is ADDED to the two
  segments nearest the hand (by distance along them) for the step (`World3D::set_added_mass`), gravity then does the rest, the
  rope's own period is the pendulum's, and tension is the weight. Taking hold in the air gives the rope the momentum of the
  character (an impulse of its mass times the speed difference). A rope with someone on it does not go to sleep.
* **Entities.** `Rope3D` on an entity (`start`, `end` as `RopeAttach3D::at(point)`, `on(entity, offset)` or `free(offset)`; the fields
  of the desc; `target_length` and `reel_speed` each step; `render`), made after the ragdolls in ascending entity id order (an
  anchor entity without a body yet makes it wait); `points()`, `closest_point`, `tension(end)`, `state` written back; `RopeEvents3D`.
  `collect_ropes(renderer, scene)` draws every rope with `render` as a round tube (`sides` 3 to 16, `render_radius`, a material
  and a tint; one mesh a material, made again each call): no new shader or pass, so it is lit, shadowed and TAA'd as any mesh, and
  off it draws and costs nothing.

### Limits

No bend or twist stiffness (above). A long rope under a heavy load stretches and reels in slowly (the numbers above); a rope can
be pulled through a thin static body at speed (no continuous collision unless `continuous`); a character's capsule that hangs
beside a rope collides with its segments if it is pushed against them. Soft bodies were not used: Jolt's soft bodies were not
tested for determinism here, and a rope of them would not be bodies that grips, loads and events already understand.
Tension on a sleeping rope is its last reading. The viewer's keys (G, H, Q, E) are not exercised by any test (no display).

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
character 4 to 5 microseconds a step; `kin_physics3d_bench climbing`, table in the plan ("P2d"). A ragdoll of 17 bodies costs about 0.01
ms a step animated (kinematic bodies following the clip), about 0.1 ms while it falls and lands (0.03 to 0.05 ms with 4 workers),
and 0.004 ms asleep; a rope of 20 segments costs about 0.22 ms a step awake at the default iterations (0.04 at 16 and 4), nothing asleep;
`kin_physics3d_bench ragdolls` and `ropes`, tables in the plan ("P2e").

## Not yet

Active (motor-driven) ragdolls; soft bodies; ropes with twist stiffness; per-contact material callbacks; continuous collision
between dynamic bodies beyond Jolt's linear cast; scaled or mutable shapes (swap with `set_shape`).
