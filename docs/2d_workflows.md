# 2D runtime workflows

These services are also available through [both Lua hosts](lua_2d_workflows.md).
The [2D Workshop](../examples/2d_workflow/README.md) combines them in a playable
example with an automated playthrough.

These C++ services build on Kin's input maps, prefab recipes, job system and
path server. Components are reflected, so scene files and entity scripts can
set their fields. Call service update/pump methods explicitly from your game.

## Input contexts and rebinding

Include `kin/platform/input_router.hpp`. Push the lowest priority context first:

```cpp
kin::InputRouter router;
router.push({.name = "gameplay", .map = gameplay_map});
router.push({.name = "menu", .map = menu_map, .enabled = false, .modal = true});
// After pumping Input, once per simulation step:
router.set_enabled("menu", menu_open);
router.update(input);
if (router.action("gameplay", "jump").pressed) jump();
const float movement = router.axis("gameplay", "horizontal");
```

Layers run from top to bottom. `consume` claims the physical controls producing
actions/axes; aliases within one layer still work together. Digital stick
directions and analog axes claim the same control. `modal` suppresses every
lower layer. Suspending a held action produces a release; resuming while held
does not invent a press. Routing reads the step's presses (`binding_pressed`, as
`Input::pressed` does) and does not change the underlying Input.

Each layer keeps the map it was pushed with as `defaults`. Change its map with
`set_map(name, map)` (a rebind, loaded overrides): `revision()` moves on every
push, remove and set_map, so a game can copy a layer's map into Input only when
it changed. Input's own actions, and automation's `input.action`, then press the
keys the layer reads:

```cpp
if (router.revision() != mirrored) {
    mirrored = router.revision();
    input.set_map(router.layer("gameplay")->map);
}
```

InputRebinder::begin(input, action, timeout) starts capture. Call update(input,
dt) once per simulation step until Captured, Cancelled or TimedOut: it reads the
step's presses, so a press in a frame that ran no step still counts. Controls
held (or pressed this step) when it begins must be released first; Escape
cancels. Capture supports keys with Shift/Ctrl/Alt, mouse buttons and gamepad
controls. conflicts(map) lists other actions with that exact binding;
apply(map, Reject|Replace|Allow, error) installs it in place of the action's
bindings on the same device (a key replaces its keys; a controller button bound
to it stays). Replace removes it from conflicting actions. Axis editing uses
InputMap directly.

save_input_overrides(defaults, effective, path) saves differences in
`kin.input-overrides/1` JSON, written whole or not at all (`write_user_file`,
which also keeps it in a browser's storage). load_input_overrides(defaults,
effective, path, error) applies them to current defaults; invalid files leave
effective unchanged. `input_profile_path(profile, error)` is where a game keeps
a named profile: `game_data_dir(id) / "input" / "<profile>.json"`, beside
SaveStore's saves, once the game has said its id with
`kin::set_user_data_game_id(id)` (kin/platform/user_data.hpp). The Lua
`input_contexts.save/load` use it, and fail until the id is set.

## Players and controllers

app.gamepads() exposes the inventory polled by App. devices() contains every
controller; players().state(player) is that player's independent input. A fresh
Start press joins the first free slot (`set_join_on_start(false)` turns that
off), or use players().join(device_id, player) and leave(player). There are four
slots by default; standalone GamepadPlayers accepts a capacity and synthetic
inventories for replay/tests. Newly connected controllers are looked for about
once a second (every 60 polls); a controller's name and identity are read once,
when it opens.

Disconnected players retain their slots and receive neutral input. A new device
ID with a unique matching GUID/serial identity reconnects without taking a live
player's device. Identical controllers without serials can be ambiguous; use an
explicit leave/rejoin when necessary.

rumble_player(player, low, high, milliseconds) and rumble(device, ...) return
false for unavailable/unsupported haptics. Strengths are clamped to 0..1;
non-finite values are rejected. Zero duration stops the motors.
gamepad_button_label(family, button) supplies Xbox/PlayStation/Nintendo face-button
names for the family a state reports (`GamepadState::family`). Legacy Input uses player zero after
joining, otherwise the first pad. Feed each player's state into a separate Input
for independent player maps/contexts.

## Prefab pooling

Include `kin/prefab/prefab_pool2d.hpp`:

```cpp
kin::PrefabPool2D bullets{world, prefab, {.capacity = 128,
    .on_release = [](kin::EcsWorld&, const kin::PrefabInstance&) {
        // Release game-owned subscriptions/resources here.
    }}};
if (auto lease = bullets.acquire({.pos = muzzle})) {
    const auto* instance = bullets.resolve(*lease);
    // Use instance->root now; keep the lease across frames.
    bullets.release(*lease);
}
```

Construction prewarms every slot (capacity 1..`PrefabPool2D::max_capacity`, 65536).
Acquire restores authored fields, hierarchy, names, local entity references and
spawn transform into the same entity IDs, as instantiate_prefab does for a new
instance; when a slot cannot be restored, error() says why (a duplicate authored
id, say) and the slot stays free.
Release removes runtime children, clears owned components and disables the slot.
Native OnRemove cleanup runs, including physics cleanup. New EntityLifetime
generations reset entity-script self/on_start and invalidate old Lua references.

Exhaustion returns nullopt; stale/foreign leases and reentrant mutations are
refused, with error(). The world must outlive its pool. Release actors instead of
destroying authored entities. Raw C++ EcsEntity handles keep their identity:
resolve a lease again before use. File-global script timers/subscriptions are
shared resources; cancel effects belonging to one spawn in the release hook.
Component destruction cannot infer ownership of external resources.

## Asynchronous scene preparation

Include `kin/scene/scene_preload2d.hpp`:

```cpp
kin::ScenePreload2D next{renderer, jobs, content_root, "scenes/level.kinscene",
    {.additional_assets = {"images/dynamic.png"},
     .register_components = register_game_components,
     .update = update_game_world}};
// Each render frame, on the renderer thread:
next.pump(2); // at most two decoded image uploads; zero uploads with pump(0)
if (next.state() == kin::SceneLoadState2D::Ready) {
    auto scene = next.take(); // install in your scene stack explicitly
}
```

Workers read content and decode images without touching the renderer/live world.
Pump creates a separate world, instantiates the scene and uploads images. CPU
entity construction and non-image asset binding remain synchronous on the renderer
thread. Serial job systems run CPU work during pump; use workers for background
loading. Keep the renderer/job system alive for the request/prepared scene.
Pass custom asset binders in options.asset_binders; they run during main-thread
preparation and remain installed on the taken scene.

The dependency scan follows asset-valued JSON fields, prefabs, sprite catalogs
and native text asset references. Add dynamic Lua/game loads in additional_assets.
Defaults limit preparation to 1024 files, 256 MiB of source bytes and 256 MiB of
decoded images. Reads work from .kinpak mounts. progress(), error() and cancel()
support loading screens. Failures/cancellation leave the active scene alone;
only a Ready request can be taken, once. Script hosting and scene-stack
transitions belong in the game's update/commit code.

Prepared scenes own an immutable MemoryContent snapshot of the scene and its
dependencies. Authoring edits cannot partially change that snapshot. Ordinary
Assets2D scenes continue to support live reload.

## Navigation agents

Include `kin/pathfinding/navigation2d.hpp`:

```cpp
kin::Navigation2D navigation{world, grid_snapshot, {.jobs = &jobs}};
actor.set(kin::NavigationAgent2D{.target = destination, .speed = 100, .radius = 8});
navigation.update(dt); // fixed step, before character/physics movement
for (const auto& event : navigation.events()) handle_navigation(event);
```

Agents follow paths, replan for changed targets/radii/lifetimes/grid snapshots,
and retry failed/stuck paths. Arrival uses the requested world point. Moving goals
replan at repath_interval while the agent follows its last valid path. Events
report Replanned (request submitted), Arrived, Stuck and Unreachable until the
next update; status/velocity also live on the component.

PathServers use conservative 1/8-cell radius buckets, share the supplied/default
job pool and have a total admission budget per step (requests_per_step). Results are delivered in
entity order on the next update. set_snapshot replaces the immutable grid and
forces replanning. Segment checks prevent crossing blocked cells; local steering
adds separation and predictive sidestepping. This is grid steering, not an RVO
crowd solver or a moving-obstacle navmesh.

With CharacterController2D, navigation supplies velocity for physics. Otherwise
it moves Transform2D in its parent's space (positions compose up the hierarchy,
as `current_world_transform` does); apply_motion=false exposes velocity
for custom movement. Grid steering does not discover physics obstacles: update
the grid or use character collision. Tune distances to world units. Invalid
numeric input stops the agent and reports unreachable. The world must outlive
the service.
Snapshots support up to four million cells, tile sizes of at least 1e-6 and
world extents of at most one billion units.

See [imports/watch mode](importing_2d.md), [accessibility](ui.md#accessibility),
[normal-mapped sprites](rendering.md#normal-mapped-sprites) and
[skeletal animation](skeleton_2d.md).
