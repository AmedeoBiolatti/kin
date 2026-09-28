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
description, units, readonly/hidden flags, and numeric ranges. Native and data
component builders expose chainable methods after `field(...)`.

## Relation Metadata

`world.relations()` registers named Flecs relation ids and exposes pair
add/remove/has plus snapshots:

```cpp
world.relations().add(source, "targets", target);
auto pairs = world.relations().snapshots(source);
```

V1 supports named relation plus entity target pairs. Advanced relation payloads,
wildcards, pair queries, and relation JSON round trips are follow-up work.
