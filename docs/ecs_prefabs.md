# ECS Prefabs

Kin prefab assets use the `kin.prefab/1` JSON schema and are built on top of
the ECS metadata pipeline. Flecs still stores runtime entities and components;
the prefab bridge uses Kin entity identity, component metadata, and retained ECS
events for editor-facing behavior.

Experimental 3D uses these same assets and utilities. Register with
`register_builtin_editor_components(world)` (includes 3D in a 3D build), load and
instantiate normally, then explicitly call `resolve_render3d_assets(world, assets)`
on the renderer thread. The authoring and validation paths require no renderer.
Place 3D instances with `Transform3D` overrides; `PrefabSpawn::pos` remains the
2D convenience. See [3D scenes and prefabs](rendering_3d.md#authored-scenes-and-prefabs)
for a complete example and the initial supported components.

Instantiation compiles each asset into internal Flecs prefab entities, creates
runtime entities with `IsA` links to those templates, then materializes authored
components through Kin metadata so overrides, snapshots, events, dirty flags,
and data-component payloads keep the same public behavior.

## Saving Linked Instances

`build_scene_asset(world)` is the checked save path. It preserves each scene-loaded
prefab's asset reference, root name/parent and only the fields differing from its
source defaults. Unchanged fields continue to inherit later edits to the prefab.
It preserves scene name/script/background, supports parents inside other prefab
instances, and remaps local and global entity references. JSON override keys are
written in a stable order.

```cpp
auto saved = kin::build_scene_asset(world);
if (saved.ok()) kin::save_scene_asset(*saved.scene, path);
else { /* show saved.diagnostics; leave the file intact */ }
```

One immutable reflected baseline is shared by all instances of a source loaded
together; it contains no GPU/physics/animation runtime handles. Saving uses that
baseline without rereading the source file or instantiating a temporary world.
Whole-instance deletion is represented by omission. Child additions, subtree removal,
component additions/removals, renames and internal reparenting are saved as minimal
field/structure overrides. Automatically disambiguated names are recognized. Plain
`remove<T>()` on a private copy still restores inherited defaults; use
`remove_prefab_component` for an actual editor removal (see below). Dangling references,
orphaned members, external child parents and changed instance authored IDs produce
diagnostics. Root display-name changes and root parenting remain supported.

To explicitly unpack instances, use `SceneSaveOptions{.preserve_prefabs = false}`.
Runtime entities without authored IDs are excluded unless `include_runtime_entities`
is true; checked saves reject references to excluded targets. The convenience
`scene_asset_from_world()` uses the checked path and throws `std::runtime_error` on
failure. This applies to scene-loaded prefab instances with source metadata;
free-standing `instantiate_prefab` entities are ordinary snapshots unless loaded
through a scene. Failed scene instantiation rolls back newly created entities.

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

## Composition

A `.kinprefab` can contain the same `prefabs` references as a `.kinscene`, alongside
its own entities. Both keep their existing `/1` schema. For example:

```json
{
  "schema": "kin.prefab/1", "id": "campfire", "root": "campfire",
  "entities": [{"id": "campfire", "components": {"Transform3D": {}}}],
  "prefabs": [{"id": "effect", "asset": "prefabs/campfire-effect.kinprefab",
               "parent": "campfire"}]
}
```

The referenced root becomes local ID `effect`; its `smoke` child becomes
`effect/smoke`, recursively. Parent links and local entity references are remapped.
Composed roots default to the containing prefab's root when `parent` is omitted.
References may use `name`, 2D `pos`, ordinary field overrides and structural overrides.
Local IDs must remain unique after expansion; file and parent cycles are rejected.
The containing asset must declare its own root entity. Existing multiple-root legacy
assets can still instantiate directly; disconnected nested members attach to the
referenced root when composed.

`parse_prefab_asset` parses declarations without file I/O. `load_prefab_asset` resolves
the graph into an immutable CPU expansion. Writing a loaded asset preserves its
declared references; `resolved_prefab_asset` and `scene_document_from_prefab_asset`
expose the expanded hierarchy. Reload after editing composed declarations; parsed
unresolved compositions are rejected by instantiation. Direct flat C++ recipes remain
supported.

Scene loading and the default AssetManager loader use their common content root for
references. Direct loading defaults to the outer file's directory; pass
`PrefabAssetLoadOptions{.content_root=content_root}` explicitly for root-relative paths.
Folders and mounted packs use the same API. All nested sources are exposed for explicit
watching; SceneAssetRuntime watches nested disk edits and SceneSession3D includes them
in transactional reloads and pack-remount detection. No background polling is added.

Nested instances retain their links in the containing source file. Scene saves link
the outer instance and target nested members by their namespaced local IDs; they do
not create independently editable inner-instance records.

## Structural overrides

The optional `structure` block sits beside the existing `overrides` map in any scene
or composed-prefab reference. Both use the same parser, writer and CPU recipe editor:

```json
{
  "id": "campfire", "asset": "prefabs/campfire.kinprefab",
  "structure": {
    "remove_entities": ["effect/smoke"],
    "remove_components": {"effect/glow": ["PointLight3D"]},
    "parents": {"effect/sparks": "campfire"},
    "names": {"effect/flames": "Bright flames"},
    "add_entities": [{"id": "extra", "parent": "campfire",
                      "components": {"Transform3D": {}}}]
  }
}
```

Entity additions are complete local recipes, with a unique ID and a parent inside the
instance. Parent/name edits apply before subtree removal, so reparenting can rescue a
child. Removing the root, detached additions/reparenting, empty names, unknown or
duplicate removals, cycles, patches to removed entities and dangling local references
are errors. Field patches may add components; removing a component then patching it
recreates it with defaults rather than inherited values. No source file is modified.

`apply_prefab_overrides` returns an edited CPU recipe or diagnostics without changing
its input. Live editor changes use the existing registry/entity APIs. To add a child
to a linked instance, create/reparent it first, then adopt a stable local identity:

```cpp
auto child = world.entities().create({.name="Extra"}).set(kin::Transform3D{});
world.entities().reparent(child, instance_root);
if (!kin::link_prefab_entity(world, instance_root, child, "extra")) { /* ID/parent error */ }
kin::remove_prefab_component(world, child, "Transform3D");
auto saved = kin::build_scene_asset(world);
```

Adoption assigns scene ID `instance.extra` and does not absorb unrelated scene
children automatically. Actual component removal materializes reflected native/data
state and detaches that entity's template inheritance before removal, preserving
remaining native runtime resources. Plain `remove<T>()` keeps its previous semantics.
Rebind/rebuild affected rendering and physics explicitly after live structural edits;
transactional scene reload is the safe whole-scene replacement path. Detached child
roots/cross-instance child parents and raw/unregistered components require explicit
unpacking. Checked saves never silently discard orphaned linked members.

For existing 3D fields, an activated `SceneSession3D` coordinates validated asset
rebinding and ordinary rigid-body replacement through the same registry patches;
multi-field transactions use `session.edit`. These edits save as normal linked
overrides. See [live field editing](rendering_3d.md#live-field-editing) for budgets,
state preservation and the remaining structural/controller limitations.

## Limits

V1 does not serialize raw/unregistered Flecs components. Asset GUID ownership,
automatic live prefab synchronization, independent nested-instance editing and Lua
structural editing helpers remain separate work; Lua field patches use the registry.

Direct prefab loading defaults to 8 MiB per source, 10,000 expanded entities, eight
composition levels and 128 unique sources. Source byte/entity limits accept zero for
unlimited; depth/source counts remain positive caps. SceneAssetRuntime carries its
source/expanded-entity limits through resolution, including structural additions, plus
`max_prefab_depth` / `max_prefab_sources` (8/128). SceneSession3D keeps its bounded
scene defaults. These are authoring bounds rather than a total memory allocator.

The compiler reuses identical definitions and structural variants within a world.
It retains generations referenced by live entities, reclaims unused generations before
new compilation, and rejects more than 128 simultaneous compiled definitions/variants.
This includes ordinary prefab sources. Field-only instance overrides share a template.
Templates contain authored component defaults; immutable save baselines contain only
reflected CPU values. There is no background compile or extra owning GPU asset cache.
