# ECS Components

Kin provides a component metadata registry for editor/server inspection. Native
components use Flecs storage; runtime data components use Kin-owned field
storage plus a Flecs presence id. In both cases Kin records stable component
names, field metadata, snapshots, patch operations, and diagnostics.

## Registering Native Components

Use `world.components().native<T>()` for C++ components that should be visible to
Kin tooling.

```cpp
struct Transform2D {
    kin::Vec2f pos{};
    kin::f32 rotation = 0.0f;
};

world.components().native<Transform2D>("Transform2D")
    .field("pos", &Transform2D::pos)
    .field("rotation", &Transform2D::rotation);
```

Fields are explicit member pointers. C++ cannot form a pointer like
`&Transform2D::pos.x`, so V1 exposes `Vec2f`, `Vec2i`, and `Color` as first-class
field values. Subfield accessors can be added later if editor UX needs separate
`x` and `y` rows.

Supported V1 field kinds:

- `bool`
- `i32`, `i64`, `u32`, `u64`
- `f32`, `f64`
- `std::string`
- `Vec2f`, `Vec2i`
- `Color`

`EntityRef` and `AssetRef` are supported metadata values. Scene and prefab JSON
can store entity refs by authored id; instantiation remaps them to runtime ids.

## Registering Data Components

Use `world.components().data()` for runtime schema-defined components:

```cpp
world.components().data("Health")
    .field_i32("hp", 100)
    .field_i32("max_hp", 100)
    .field_string("faction", "neutral");
```

Data component fields require defaults. `add()` initializes an entity from those
defaults, and `patch_field()` updates Kin-owned field storage. Flecs stores the
component presence id so runtime query plans can match `all` and `none` terms.

Data schemas can be migrated in place:

```cpp
world.components().migrate_data_schema("Health", {
    {.name = "hp", .kind = kin::ComponentFieldKind::I32, .value = kin::i32{100}},
    {.name = "label", .kind = kin::ComponentFieldKind::String, .value = std::string{}},
});
```

Fields with the same name and kind preserve existing values. New fields use
their defaults, and removed or type-changed fields are dropped.

Field descriptors also carry optional editor display hints: label, category,
description, units, readonly/hidden flags, and numeric ranges. See
`docs/ecs_editor_workflow.md`.

## Metadata And Entity Operations

Components can be looked up by name or Flecs id:

```cpp
const kin::ComponentDescriptor* transform = world.components().find("Transform2D");
std::vector<kin::ComponentDescriptor> all = world.components().descriptors();
```

Registered components can be added, removed, and checked without raw Flecs calls:

```cpp
world.components().add(entity, "Transform2D");
bool has_transform = world.components().has(entity, "Transform2D");
world.components().remove(entity, "Transform2D");
```

Existing raw usage remains valid:

```cpp
world.component<Transform2D>("Transform2D");
entity.set(Transform2D{});
```

Raw Flecs usage can see data component presence ids, but data field values must
be read and written through `world.components()`.

## Snapshots And Patching

Snapshot one component:

```cpp
std::optional<kin::ComponentSnapshot> snapshot =
    world.components().snapshot(entity, "Transform2D");
```

Snapshot all registered components present on an entity:

```cpp
std::vector<kin::ComponentSnapshot> components =
    world.components().snapshots(entity);
```

Patch one field:

```cpp
world.components().patch_field(entity,
                               "Transform2D",
                               "pos",
                               kin::ComponentFieldValue{kin::Vec2f{10.0f, 20.0f}});
```

Successful native patches write the typed C++ component field and call
`entity.modified<T>()`. Successful data patches update Kin-owned field storage.
Component add/remove/patch operations emit retained ECS events through
`world.events()`, and registered native components install Flecs observers for
raw component add/remove/set changes.

Failures return `false` and set `world.components().last_error()` for:

- unknown component id/name
- component not present on the entity
- unknown field
- mismatched field value type
- duplicate component or field registration

## JSON And Scene Documents

Component field values can be written to JSON through
`write_component_field_value_json()` and converted from parsed `JsonValue` with
`component_field_value_from_json()`.

`scene_document_from_world(world)` and `serialize_scene(world)` use registered
component metadata to produce deterministic source-like scene documents.
`instantiate_scene(world, document, error)` validates unknown components, unknown
fields, field kind mismatches, duplicate entity ids, and invalid parent ids
before mutating the target world.

Scene entity ids are authored ids managed by `world.entities()`. Runtime-only
entities without authored ids receive deterministic generated document ids during
serialization.

## Relationship To Lua

`ScriptComponentRegistry` remains in place as the current Lua adapter. Runtime
data components now provide the metadata and storage layer that Lua can later use
through cached query plans and safe field accessors.

## Limits

Generated reflection, nested field accessors, relation/pair metadata, and full
schema versioning policies are follow-up layers.
