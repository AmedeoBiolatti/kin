# ECS Events

Kin ECS records editor-facing changes in a retained per-world event log. The log
is designed for polling from tools and scene-server snapshots; callback
subscriptions are a later layer.

## Event Log

Use `world.events()` to inspect and manage events:

```cpp
const std::vector<kin::EcsEvent>& events = world.events().snapshot();
std::vector<kin::EcsEvent> recent = world.events().since(last_sequence);
kin::EcsDirtyState dirty = world.events().dirty_state();
world.events().clear_dirty();
world.events().clear();
```

Events have a monotonic sequence, frame number, kind, source, optional entity,
component id/name, field name, system id, and message.

## Dirty State

Dirty flags summarize the retained log for editor refresh decisions:

- entity hierarchy changes mark `world_dirty` and `hierarchy_dirty`
- entity enable/disable marks `world_dirty`
- component add/remove/change marks `world_dirty`, `components_dirty`, and `render_dirty`
- system add/remove/enable/disable/error marks `world_dirty` and `systems_dirty`

`clear_dirty()` resets dirty booleans without removing events. `clear()` removes
events and clears dirty state while keeping event sequence numbers monotonic.

## Sources

Kin APIs emit `EcsEventSource::KinApi` events from `world.entities()`,
`world.components()`, and `world.systems()`. Components registered through
`world.components().native<T>()` also install Flecs observers for raw
`add<T>()`, `remove<T>()`, and `set<T>()` changes. Observer events use
`EcsEventSource::FlecsObserver`.

Kin component APIs suppress their matching Flecs observer event and emit one
Kin event instead. `patch_field()` includes the field name; raw Flecs observer
changes do not.

## Scene Server

`world.snapshot` includes events in the Kin payload by default:

```json
{
  "kin": {
    "events": {
      "dirty": {},
      "items": []
    }
  }
}
```

Use `{ "events": false }` to omit the event payload.

## Limits

V1 does not provide callback subscriptions or persistent audit storage. Raw Flecs
entity structural changes are not fully tracked; the supported raw bridge is
registered component observers.
