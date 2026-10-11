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

## Not yet

The character controller; grab and climb; ragdoll; ropes; soft bodies; per-contact material callbacks; continuous collision
between dynamic bodies beyond Jolt's linear cast; scaled or mutable shapes (swap with `set_shape`).
