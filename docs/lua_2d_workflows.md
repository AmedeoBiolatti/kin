# Lua 2D workflows

Both `ScriptEngine` and `EntityScripts` bind these tables automatically.
Services belong to one live world, and must be stepped once per simulation frame.
`ScriptEngine::call_update` and `EntityScripts::update` perform that step before
calling Lua. Do not step a shared service twice through two hosts in one frame;
choose one owner or use separate services for their respective worlds.
Workflow mutations belong in ordinary update/start callbacks; registered ECS
systems reject them so a failed system cannot escape its command transaction.

## Property tweens

```lua
local tween = tweens.to(actor, 'Transform2D.pos', {x=120,y=40}, .3, 'easeout')
tween:parallel():to('RectRenderer.color', {r=255,g=180,b=60,a=255}, .3)
tween:wait(.2):to('Transform2D.pos', {x=80,y=40}, .2)
-- tween:pause(), :resume(), :restart(), :cancel(), :paused(), :finished(), :cancelled()
```

Targets can be host entity userdata, entity-script `self`, or an entity name.
Properties use the described component fields and their exact value kinds.
Easing uses Kin's `step`, `linear`, `easein`, `easeout`, `easeinout` names.
Build the sequence before its first step. Removal of any required component,
destruction or pool reuse cancels it before further writes. Lua callbacks are
not retained in native tweens, so replacing a Lua VM leaves no callback into it.

## Input contexts and rebinding

```lua
input_contexts.push('game', 'key fire Space\naxis move + key:D\naxis move - key:A\n')
input_contexts.push('menu', 'key accept Enter\n', true) -- modal, consumes by default
input_contexts.enable('menu', false)
local fire = input_contexts.pressed('game', 'fire')
local move = input_contexts.axis('game', 'move')
```

`push(name, input_file_text [, modal [, consume]])` installs/replaces a layer.
`remove`, `enable`, `held`, `pressed`, `released`, `axis` follow InputRouter.
Changes take effect at the next workflow step. Raw `input` remains available.

`rebind.begin(action [, timeout=10])` starts capture using the latest input frame.
`state()` returns `idle`, `listening`, `captured`, `cancelled` or `timed_out`.
`candidate()` returns a binding token; `conflicts(context)` returns action names.
`apply(context [, 'reject'|'replace'|'allow'])` installs a captured candidate or
raises an error. `cancel()` stops capture. Initially held controls are ignored
until released, and Escape cancels.

`input_contexts.overrides(context)` returns the JSON differences from the pushed
defaults. `apply_overrides(context, json)` validates before applying.
`save(context, profile)` and `load(context, profile)` use
`user_data_dir("kin")/input/<profile>.json`; profile names contain only letters,
digits, `_` and `-`. No arbitrary file-writing path is exposed.

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
Recipes are rooted in the host's content directory. Capacity is 1..4096.
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
scene Assets2D cache. EntityScripts and custom hosts can provide them explicitly:

```cpp
auto workflows = std::make_shared<kin::ScriptWorkflows2D>(world, content_root);
workflows->configure(&renderer, app.gamepads(), &assets, &jobs);
entity_scripts.set_workflows(workflows);
// Or ScriptSceneBindings{.world=&world, .workflows=workflows}.
```

Omitted jobs are created lazily on the first preload. A missing Assets2D service
is created lazily for memory APIs when a renderer is available; provide the game's
cache to inspect its actual resources. World, renderer and borrowed services
outlive the workflow service and its Lua states. Pending loads are cancelled when
the workflow service closes. `bind_lua_workflows2d(lua, provider)` also works with
LuaScript; entity names are supported, while userdata requires the host's checked
`kin.entity`/`kin.unwrap_entity` Lua registry bridges.
