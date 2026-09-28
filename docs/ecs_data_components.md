# ECS Data Components

Runtime data components are schema-defined components registered through
`EcsComponentRegistry`. They are intended for editor-authored data, scene and
prefab JSON, server inspection, and future Lua/data systems.

## Registering

```cpp
world.components().data("Health")
    .field_i32("hp", 100)
    .field_i32("max_hp", 100)
    .field_string("faction", "neutral")
    .field_color("tint", kin::Color::rgba(255, 255, 255, 255));
```

Data fields require defaults. Supported V1 kinds are `Bool`, integer and float
scalars, `String`, `Vec2f`, `Vec2i`, `Color`, `EntityRef`, and `AssetRef`.
Entity refs can be serialized by authored id and remapped during scene/prefab
instantiation.

## Runtime Behavior

Kin stores data component field values in `EcsComponentRegistry` and gives each
data component a Flecs presence id. This means:

- `add()` initializes field values from schema defaults
- `has()` / query plans work through Flecs presence
- `snapshot()` and `patch_field()` read and write Kin-owned field storage
- component add/remove/change events and dirty flags are emitted through Kin
- entity destruction clears data component storage
- schema migration preserves same-name/same-kind fields and defaults new fields

Raw Flecs calls can see the presence id, but cannot read or mutate data fields.
Use `world.components()` for data component values.

## Scenes, Prefabs, Queries

Scene documents, prefab assets, prefab overrides, world snapshots, and runtime
query plans use data components through the same metadata APIs as native
components. Systems can name data components in `reads` and `writes` metadata for
scheduling diagnostics, but typed C++ native systems do not receive data fields
as typed parameters in V1.

## Limits

V1 does not include full schema version policy, editor display hints,
relation/pair metadata, or cooked asset formats.
