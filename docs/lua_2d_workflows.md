# Lua 2D workflows

Both `ScriptEngine` and `EntityScripts` bind these tables automatically. Their
services (`ScriptWorkflows2D`) are the world's: one per world, shared by every
Lua host bound to it (`ScriptWorkflows2D::shared`), so a scene script and the
entity scripts of one world see the same input contexts, pools and tweens.
`ScriptEngine::call_update` and `EntityScripts::update` step them before calling
Lua; with both hosts on a world they still advance once per simulation step.
Workflow mutations belong in ordinary update/start callbacks; registered ECS
systems reject them so a failed system cannot escape its command transaction.

## Property tweens

```lua
local move = tweens.to(actor, 'Transform2D.pos', {x=120,y=40}, .3, 'easeout')
local glow = tweens.to(actor, 'RectRenderer.color', {r=255,g=180,b=60,a=255}, .3,
                       {delay=.1, repeats=1, yoyo=true})
task.run(function()
    task.await(move)                                            -- one after another
    task.await(tweens.to(actor, 'Transform2D.pos', {x=80,y=40}, .2))
end)
-- move:state() is 'running', 'completed' or 'cancelled'; move:cancel(); move:error()
```

`tweens.to(target, property, to, seconds[, easing][, options])` starts an ECS
tween ([kin/ecs/tween.hpp](animation_system.md)), as an entity script's
`self:tween` does, with the same options: `delay`, `repeats`, `yoyo`,
`relative`, `easing`. Targets can be host entity userdata, entity-script `self`,
or an entity name. Properties use the described component fields and their
value kinds; easing names are `easings()`'s (`linear`, `easeout`, `backout`...),
`easeout` when none is given.
The world's services step every tween once per simulation step.

The handle is for `task.await`: sequences are tasks that await one tween after
another; a parallel step starts several tweens and awaits each. A tween is
`cancelled` when its handle cancels it, when another tween of the same field
replaces it or `self:stop_tween(s)` stops it, or when its entity or component
goes (destruction, pool reuse); awaiting a cancelled tween fails the task.
`tweens.of(entity, id)` is the handle of a tween `self:tween` started. Handles
hold no Lua callback, so replacing a Lua VM leaves none into the old one.

## Input contexts and rebinding

```lua
input_contexts.push('game', 'key fire Space\naxis move + key:D\naxis move - key:A\n')
input_contexts.push('menu', 'key accept Enter\n', true) -- modal, consumes by default
input_contexts.enable('menu', false)
local fire = input_contexts.pressed('game', 'fire')
local move = input_contexts.axis('game', 'move')
```

`push(name, input_file_text [, modal [, consume]])` installs a layer (a name in
use is an error). `remove`, `enable`, `held`, `pressed`, `released`, `axis`
follow InputRouter; an unknown context name is a Lua error.
Changes take effect at the next workflow step. Raw `input` remains available.

`rebind.begin(action [, timeout=10])` starts capture using the latest input frame.
`state()` returns `idle`, `listening`, `captured`, `cancelled` or `timed_out`.
`candidate()` returns a binding token; `conflicts(context)` returns action names.
`apply(context [, 'reject'|'replace'|'allow'])` installs a captured candidate or
raises an error; the binding replaces the action's bindings on its own device
only. `cancel()` stops capture. Initially held controls are ignored until
released, and Escape cancels.

`input_contexts.overrides(context)` returns the JSON differences from the pushed
defaults. `apply_overrides(context, json)` validates before applying.
`save(context, profile)` and `load(context, profile)` use the game's own
`input_profile_path(profile)`, `user_data_dir("kin")/<game id>/input/<profile>.json`
beside its saves, and fail until the host set the id
(`kin::set_user_data_game_id`). Profile names contain only letters, digits, `_`
and `-`. No arbitrary file-writing path is exposed.

## Prefab pools and staged scenes

```lua
pools.create('bullets', 'prefabs/bullet.kinprefab', 64)
local lease = pools.acquire('bullets', 40, 80) -- nil when full
if lease then
    local entity = lease:entity()
    -- checked host entity; stale after release/reuse
    lease:release()
    assert(not lease:alive())
end
```

`active(name)` counts current leases; `destroy(name)` destroys a pool.
Creating the same name/recipe/capacity again is idempotent, including after an
entity-script reload. Changing a pool requires explicit destruction first.
Recipes are rooted in the host's content directory. Capacity is
1..`PrefabPool2D::max_capacity` (65536); a pool that cannot be made is a Lua error.
Keep the lease while its entity is in use. A lease dropped by Lua GC is released
at the next workflow step; finalizers never mutate a running ECS transaction.

```lua
local loading = preload.scene('rooms/cave.kinscene')
-- subsequent updates: loading:state(), :progress(), :error()
if loading:state() == 'ready' then loading:commit() end
-- loading:cancel() preserves the active scene
```

States are `loading`, `uploading`, `ready`, `failed`, `cancelled`, `taken`.
The service pumps jobs and uploads at most two textures per step. Commit returns
false until ready and can only succeed once. Dropping an uncommitted request
cancels it at the next step, including when its Lua VM is replaced. Commit
queues the prepared scene for
native `take_scene()` after Lua returns. ScriptEngine's `SceneContext` overload
queues a SceneManager replacement automatically; frame-only and entity-script
hosts consume `take_scene()` explicitly. The native ScenePreload2D API remains
available for custom preload binders, resource limits and scene update functions.

## Controllers and asset memory

Player indices are one-based in Lua. `controllers.devices()` returns
`{id,name,connected}` entries. `join(device, player)` and `leave(player)` manage
ownership. `state(player)` supplies connection/device, sticks, triggers and
button bits; an unavailable player is neutral. `rumble(player,low,high,ms)`
returns whether the request was accepted. `label(player,button_index)` uses
zero-based native GamepadButton values and that player's device family.

`asset_memory.budget(cpu_bytes, gpu_texture_bytes)` applies two independent
budgets; zero is unlimited. `report()` and `trim()` return `{cpu=...,gpu=...}`
with resident/budget/pinned bytes and an over-budget flag. `trim()` also reports
eviction counts. `residency()` returns deterministic CPU/GPU row arrays with
paths, bytes, reference counts, dependencies and retaining chains. Accounting
scope and estimates follow [the native memory APIs](assets.md#memory-budgets-and-residency).

## Native host setup

CPU-only hosts get tweens, contexts and pools automatically. Renderer-dependent
services require configuration. ScriptScene supplies its renderer, gamepads and
scene Assets2D cache. Custom hosts configure the world's services, made by the
first host that needs them or by the game:

```cpp
auto workflows = kin::ScriptWorkflows2D::shared(world, content_root);
workflows->configure(&renderer, app.gamepads(), &assets, &jobs);
// The scene script and entity scripts of `world` use these. A host may also
// hand in its own: ScriptSceneBindings{.world=&world, .workflows=workflows}.
```

Omitted jobs are created lazily on the first preload. A missing Assets2D service
is created lazily for memory APIs when a renderer is available; provide the game's
cache to inspect its actual resources. World, renderer and borrowed services
outlive the workflow service and its Lua states. Pending loads are cancelled when
the workflow service closes. `bind_lua_workflows2d(lua, provider)` also works with
LuaScript; entity names are supported, while userdata requires the host's checked
`kin.entity`/`kin.unwrap_entity` Lua registry bridges.
