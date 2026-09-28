# ECS Inspection

Kin ECS inspection builds editor-facing snapshots from Kin metadata while raw
Flecs access remains available for low-level debugging.

## World And Entity Snapshots

`EcsWorld::snapshot()` returns registered component metadata and deterministic
entity snapshots:

```cpp
kin::WorldSnapshot snapshot = world.snapshot();
```

Entity snapshots include runtime id, optional authored id, name, alive/enabled
state, Kin registration state, handle generation, creation sequence, parent id,
child ids, and registered component snapshots. Only components registered
through `world.components()` are included in Kin component snapshots, including
runtime data components.

## Scene Server

The `world.snapshot` command still returns raw Flecs JSON by default, and now
also includes a Kin metadata payload:

```json
{
  "scene": 0,
  "world": {},
  "kin": {
    "world": { "components": [], "entities": [] },
    "systems": [],
    "schedule": {}
  }
}
```

System entries include scheduling and execution diagnostics: batch index,
parallel eligibility, last execution mode, command-buffer stats, schedule
diagnostics, and structured parallel rejection diagnostics. The schedule payload
includes requested/effective execution mode, dependency edges, per-batch
execution mode, batch duration, command-buffer stats, and batch diagnostics.
The Kin payload also includes retained ECS events and dirty flags by default.

Use params to control payload size:

```json
{"raw": false}
{"kin": false}
{"events": false}
```

Animations remain controlled separately with the existing `animations` param.

## Runtime Query Plans

Name-based query plans are built from registered component metadata:

```cpp
kin::EcsQueryPlan plan = world.build_query_plan({
    .all = {"Transform2D"},
    .none = {"Hidden"},
    .reads = {"Transform2D"},
});

world.each(plan, [](kin::EcsEntity entity) {
    // deterministic entity order
});
```

V1 query plans support `all` and `none` component terms. Unknown component names
make the plan invalid and attach diagnostics.

## Scene Documents

`scene_document_from_world(world)` and `serialize_scene(world)` produce a
source-like scene document using registered native and data component metadata.
`instantiate_scene` validates all entity ids, parent ids, components, fields, and
field kinds before mutating the target world.

Scene document entity ids are authored ids. Entities without authored ids receive
deterministic generated document ids when serialized, and those ids become
authored ids when instantiated into a target world.

Prefab assets can be converted to and from scene documents with the prefab bridge
helpers. Prefab entity ids remain asset-local unless instantiation opts into an
authored id prefix.

This format is intended for editor/testing round trips. It is not the final
cooked prefab or release scene format.
