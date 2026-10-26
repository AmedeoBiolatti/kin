# ECS Editor Workflow

This layer collects small editor-facing ECS helpers that sit above world,
component, system, scene, prefab, and scripting metadata.

## Play/Edit Sessions

`PlayWorldSession` clones an edit world into an isolated play world through a
scene document:

```cpp
kin::PlayWorldSession session;
session.start(edit_world, {.configure_play_world = register_components});
session.pause();
session.step(1.0f / 60.0f);
kin::SceneDocument result = session.apply_document();
session.discard();
```

The configure callback must register the same component metadata in the play
world before the scene document is instantiated. V1 returns a scene document for
apply workflows rather than mutating the edit world automatically.

## Reload Coordinator

`EcsReloadCoordinator` is a lightweight orchestration surface for editor reload
steps:

```cpp
kin::EcsReloadCoordinator reload;
reload.migrate_data_schema(world, "Health", fields);
reload.reload_script_scene(scene);
reload.validate_prefab(prefab, world.components());
```

V1 delegates to existing schema migration, script reload, and prefab validation
APIs, returning structured diagnostics.

## Display Metadata

`ComponentFieldDescriptor` includes optional inspector hints: label, category,
description, units, readonly/hidden flags, numeric ranges, for an asset
reference the kind of asset it names (`asset_type`, e.g. `"Image"`), for a
field naming an entry of such an asset (a sprite, a cue) that field (`entry_of`),
and for a string field that takes only some values those values (`choices`,
e.g. `{"static", "dynamic", "kinematic"}`; the field's setter still decides what
it takes). Native and data component builders expose chainable methods after
`field(...)`; a native one also describes a member of a member
(`.field("gravity_scale", &PhysicsBodyComponent::def, &PhysicsBodyDef::gravity_scale)`).

## Kin's Components in Scenes

`register_builtin_editor_components` describes kin's own components, so scene
files and tools can set them: transforms, rect/line/texture/sprite renderers,
audio emitters, and:

- **2D physics.** `PhysicsBodyComponent` (`type`: static, dynamic or kinematic;
  velocities, damping, `gravity_scale`, `fixed_rotation`, which side leads)
  and `PhysicsColliderComponent`, described as its first collider (`shape`:
  box or circle; `half_extents`, `radius`, `offset`, `density`, `friction`,
  `restitution`, `sensor`, `category_bits`, `mask_bits`). A collider is made
  by the first field set (`{}` has none), and a field changed later makes its
  fixture again. `PhysicsVelocity`, `PhysicsContactQueue` and
  `PhysicsOverlapSet` (no fields: an entity that collects its contacts) too.
  A game steps them with a `PhysicsWorld` and `update_physics_world`; for a
  world in points, `PhysicsWorldDef::units_per_meter` says how many make a
  metre (Box2D wants bodies of 0.1 to 10 metres, and moves none more than 2
  metres a step). Instances of a prefab get bodies of their own.
- **2D particles.** `ParticleSystemComponent` (`asset`: a particle catalog;
  `effect`, `rate`, `emitting`, `burst_at_start`, `offset`, `sprites`; and
  `active`, read-only, the particles alive) and `ParticleFieldComponent`
  (`asset`, `field`, `sprites`): see [Assets named by components](assets.md#assets-named-by-components-2d).
- **Tilemaps.** `TilemapComponent` (`asset`: a tilemap, square cells or
  hexagons, of a tileset file; `layer`, `order`, `visible`; and `cells`,
  read-only, its map's cells once read): drawn from where the entity is.
- **Sprite animations.** `SpriteAnimator` (`animation`: an animation of the
  catalog its entity's `SpriteRenderer` names, `entry_of("SpriteRenderer.asset")`;
  `speed`, `playing`; and `frame` and `finished`, read-only): its sprite's
  frames shown in turn by `update_sprite_animators(world, dt)`. An `entry_of`
  of the form `Component.field` names another component's asset.
- **Animators.** `Animator` (`asset`: an animation file; `animation`, an
  animation of it, or `machine`, a state machine of it, `entry_of("asset")`;
  `speed`, `playing`; and `time`, `finished` and `state`, read-only): its animation's tracks set the entity's fields, and
  its children's, in `update_animators(world, dt)`.
- **Cameras.** `CameraComponent` (`current`, `zoom`, `smoothing`, `limited`,
  `limit_min`, `limit_max`): what the game shows, looking at where its entity
  is. `EcsScene` draws through the first current one; `update_cameras(world, dt)`
  moves those that smooth.
- **Shapes, clips and text.** `ShapeRenderer` (`shape`: `none`, `rect`,
  `rounded_rect`, `ellipse`, `polygon` or `svg`; `asset`, `size`, `radius`,
  `sides`, `fill`, `stroke`, `stroke_width`, `offset`, `tint`, `layer`,
  `order`, `y_sort`, `visible`), `ClipGroup` (`shape`: `none`, `rect`,
  `rounded_rect` or `ellipse`; `size`, `offset`, `radius`, `layer`, `order`,
  `y_sort`) and `TextRenderer` (`text`, `font`, `size`, `color`, `offset`,
  `pivot`, `wrap`, `align`, `outline`, `outline_color`, `layer`, `order`,
  `y_sort`, `visible`). See [Rendering](rendering.md).

## Relation Metadata

`world.relations()` registers named Flecs relation ids and exposes pair
add/remove/has plus snapshots:

```cpp
world.relations().add(source, "targets", target);
auto pairs = world.relations().snapshots(source);
```

V1 supports named relation plus entity target pairs. Advanced relation payloads,
wildcards, pair queries, and relation JSON round trips are follow-up work.
