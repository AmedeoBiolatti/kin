# ECS Prefabs

Kin prefab assets use the `kin.prefab/1` JSON schema and are built on top of
the ECS metadata pipeline. Flecs still stores runtime entities and components;
the prefab bridge uses Kin entity identity, component metadata, and retained ECS
events for editor-facing behavior.

Instantiation compiles each asset into internal Flecs prefab entities, creates
runtime entities with `IsA` links to those templates, then materializes authored
components through Kin metadata so overrides, snapshots, events, dirty flags,
and data-component payloads keep the same public behavior.

## Asset And Runtime Identity

Prefab entity ids are asset-local ids:

```cpp
kin::PrefabAsset prefab{
    .id = "panel",
    .root = "root",
    .entities = {{.id = "root", .name = "Panel"}},
};
```

By default, instantiating a prefab does not assign world authored ids. This
allows the same prefab asset to be spawned repeatedly without id collisions.

Use `PrefabSpawn::authored_id_prefix` when an editor workflow needs stable
world-local authored ids:

```cpp
kin::PrefabInstance instance = kin::instantiate_prefab(
    world,
    prefab,
    {.authored_id_prefix = "panel-a"},
    world.components());
```

Runtime authored ids become `<prefix>/<prefab_entity_id>`, such as
`panel-a/root`. Collisions are rejected before the prefab mutates the world.
`PrefabInstance::by_authored_id` remains an asset-local id to runtime entity map.

Use `PrefabInstanceRegistry` when editor/runtime code needs persistent instance
records:

```cpp
kin::PrefabInstanceRegistry instances;
kin::PrefabInstance instance =
    instances.instantiate(world, prefab, spawn, world.components(), overrides);
```

Tracked records store the instance id, source prefab id, root, spawned entities,
prefab-local id map, and overrides.

## Components And Overrides

Prefab assets can author native and data components registered with
`world.components()`. Instantiation adds components through
`EcsComponentRegistry` and patches fields through component metadata, so
component events and dirty flags are emitted.

Overrides use the same component/field shape as prefab entities and are applied
after base prefab fields:

```cpp
kin::PrefabOverrideSet overrides;
overrides.entities["root"] = {{
    .name = "Transform2D",
    .fields = {{.name = "pos", .value = kin::Vec2f{10.0f, 20.0f}}},
}};
```

`EntityRef` fields in prefab JSON can reference prefab-local entity ids. During
instantiation, those refs are remapped to the spawned runtime entities.

Validation reports unknown components, unknown fields, field type mismatches,
duplicate component or field entries, unknown override targets, invalid parents,
parent cycles, missing roots, and duplicate prefab entity ids.

## Scene Bridge

Prefab assets can be built from source-like scene documents:

```cpp
kin::PrefabBuildResult built = kin::prefab_asset_from_scene_document(
    document,
    {.prefab_id = "panel", .name = "Panel"},
    world.components());
```

If `PrefabFromSceneOptions::root` is empty, Kin infers the root only when the
scene document has exactly one root entity. `scene_document_from_prefab_asset()`
converts a prefab back to a `SceneDocument` while preserving prefab-local ids,
names, hierarchy, registered components, and field values.

## Limits

V1 does not serialize raw/unregistered Flecs components. Nested prefabs, prefab
instance handles, override diff tracking, asset GUID ownership, live prefab
syncing, cached compiled-template reuse across instantiations, and Lua prefab
authoring are follow-up layers.
