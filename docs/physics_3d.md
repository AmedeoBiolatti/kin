# 3D physics (experimental)

`kin/physics3d` is rigid body physics for kin's experimental 3D work, on
[Jolt Physics](https://github.com/jrouwe/JoltPhysics) 5.5.0 (MIT). It exists only with
`KIN_EXPERIMENTAL_3D` (on by default; the build without it never fetches Jolt). The Box2D module
(`kin/physics`, 2D) is separate and unchanged. The API may change in any release; the plan and
measurements are in [3d_plan.md](3d_plan.md) ("P2a - Jolt core - results").

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

## Not yet

Raycasts, shape casts, overlaps, contact events and sensor reports (P2b); constraints; debug
drawing; ECS components (`RigidBody3D`, `Collider3D`); the character controller; ragdoll; ropes.
