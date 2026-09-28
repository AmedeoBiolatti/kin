# ECS Entities

Kin provides an entity identity layer on top of Flecs for editor/server
workflows. Flecs remains the storage backend; Kin records optional authored ids,
handle generations, creation order, and lifecycle diagnostics.

## Creating And Looking Up Entities

Use `world.entities()` when an entity needs editor-facing identity:

```cpp
kin::EcsEntity root = world.entities().create({
    .name = "Root",
    .authored_id = "root",
    .enabled = true,
});

kin::EcsEntity child = world.entities().create({
    .name = "Child",
    .authored_id = "child",
    .parent = root,
});
```

Authored ids are optional and world-local. Non-empty authored ids must be unique.
`world.entity(name)` remains available and creates a Kin-registered entity with
no authored id.

Lookup helpers:

```cpp
world.entities().find_by_id(runtime_id);
world.entities().find_by_authored_id("child");
world.entities().find_by_name("Child");
```

Names are display-oriented. Flecs requires unique names in a scope, so Kin may
suffix duplicate runtime names during creation; use authored ids or handles for
stable editor identity.

## Handles And Lifecycle

`EcsEntityHandle` stores a runtime id plus Kin generation. It resolves only while
the matching entity is alive and still registered with that generation:

```cpp
kin::EcsEntityHandle handle = world.entities().handle(entity);
bool ok = world.entities().valid(handle);
kin::EcsEntity resolved = world.entities().resolve(handle);
```

Lifecycle operations:

```cpp
world.entities().enable(handle);
world.entities().disable(entity);
world.entities().rename(entity, "New Name");
world.entities().reparent(child, parent);
world.entities().detach_parent(child);
world.entities().destroy(handle);
```

Reparenting rejects hierarchy cycles. Destroying through the registry invalidates
handles and removes authored-id/name indexes for the destroyed hierarchy.
Entity lifecycle operations emit retained ECS events through `world.events()`;
see `docs/ecs_events.md`.

## Cloning

`world.entities().clone(source)` copies the source entity and, by default, its
descendants. Registered component values are copied through
`EcsComponentRegistry`; raw/unregistered Flecs components are not copied in V1.

```cpp
kin::EntityCloneResult clone = world.entities().clone(source, {
    .deep = true,
    .parent = new_parent,
    .authored_id = "source.copy",
});
```

Clone defaults avoid preserving authored ids to prevent duplicates. Child
authored ids can be preserved only when they do not collide in the target world.

## Snapshots And Scenes

Entity snapshots include authored id, registration state, handle generation, and
creation sequence in addition to runtime id, name, enabled/alive state, parent,
children, and registered component snapshots.

Scene document entity ids are authored ids. `scene_document_from_world()` emits
authored ids when present and deterministic generated ids for entities without
authored ids. `instantiate_scene()` validates duplicate ids, unknown parents,
components, fields, and field kinds before mutating the world.

Prefab asset entity ids are local to the prefab by default. `instantiate_prefab()`
can opt into world authored ids with `PrefabSpawn::authored_id_prefix`, producing
ids such as `<prefix>/<prefab_entity_id>` while still returning the prefab-local
id to runtime entity map.

## Limits

V1 is a world-local editor identity layer. It does not yet provide prefab
instance handles, play/edit world diffing, or global asset GUID ownership.
