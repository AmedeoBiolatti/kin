# Lua development

Kin's Lua hosts share instruction limits, VM heap accounting, profiling and
simulation-driven tasks. Entity scripts also support transactional reload of
their instance data. These tools complement [gameplay scenarios](automation.md)
and [development sessions](hot_reload.md).

## Editor definitions and checks

Build `kin_lua` and generate definitions for the **same engine build** the game
uses. Optional APIs and reflected component fields come from the live bindings:

```sh
cmake --build build --target kin_lua_api
```

This creates `build/lua-api/scene`, `build/lua-api/entity`, `build/lua-api/test` and
`build/lua-api/scenario` (gameplay scenario files, `scenario{...}`), each containing
`kin.lua`, `api.json` and a sample `.luarc.json`. Add the appropriate directory to
your editor's Lua workspace library. Use one host per script workspace; entity
and scene scripts expose different entity APIs. For example, a project
`.luarc.json` can contain:

```json
{
  "runtime.version": "Lua 5.4",
  "workspace.library": ["build/lua-api/entity"],
  "workspace.checkThirdParty": false
}
```

Definitions include parameter and return types, native userdata methods, and
component-specific overloads for `get`, `component`, `set` and `patch`.
Scene `get("Transform2D")` retains its legacy `{x, y}` representation;
`component("Transform2D")` exposes reflected fields such as `pos` and `rotation`,
plus `commit()`. Entity script `get` uses reflected fields.

Run syntax and API checks without starting a game:

```sh
python3 tools/kin_lua.py --native build/bin/kin_lua --host entity \
  check examples/lua_testing/content/actor.lua
```

The checker recognizes native globals, callback parameters, simple local
aliases, chained native calls, literal arguments and literal component patches.
It reports unknown members/components/fields, argument counts, obvious type
mismatches and writes to reflected readonly fields. It tokenizes Lua, skipping
comments and strings. Dynamic dispatch, reassigned globals, arbitrary table
inference and custom native bindings remain runtime-checked; this is not a full
Lua type checker. Generated definitions supply richer editor analysis.
`kin_lua check file.lua ...` performs syntax checking alone. Pass
`--components path/to/project.kincmp` before `generate` / `check` to add a
project component schema to the native inventory. Native `kin_lua api` accepts
the same option.

## Entity state and lifecycle

Each script file still owns one VM and each entity owns a `self` table. Reload
now prepares a fresh VM and copies every affected instance before activation:

```lua
function on_start(self)
    self.version = 1
    self.speed = 100
    self.steps = 0
end

function on_reload(self)
    -- Pure data migration; on_start is not called again.
    self.version = 2
    self.speed = 200
end

function on_stop(self, reason)
    -- disabled_or_removed, destroyed, replaced, or restart.
    -- self:alive() is false when destruction already happened.
end
```

`on_reload` is optional; without it, existing instance data carries over.
New instances continue to run `on_start`. Lua integers retain their integer
representation; finite floats, strings, nested tables, shared table references,
checked entity references and supported workflow handles survive. Cycles,
closures, raw pointers and unsupported userdata reject the edit. Limits are 32
levels, 10,000 copied values and 4 MiB of strings per copied root.

Candidate top-level code and migration cannot call live engine services
(`world`, `input`, `ui`, `save`, ... raise an error) or mutate entities; the
service tables themselves are the same plain tables a fresh VM has. Any parse,
budget, copy or migration failure keeps the old VM,
instance data and tasks. All instances of one file activate together. Module
edits trigger the same file-level transaction. Independent script files reload
individually; this does not add a multi-file transaction to legacy polling.
The scene host's `reload_state` / `on_reload(saved, scene)` contract remains
described in [development sessions](hot_reload.md). Should a rolled-back scene
script's systems fail to register again, that script halts
(`ScriptEngine::halt`): its systems stay removed and every call fails with the
reason, which the host reports as a script error.

Automatic polling uses `EntityScriptsConfig::check_frames`; native hosts can
call `EntityScripts::reload(asset)` explicitly, including with polling disabled.
The world must outlive its script host. `restart()` invokes stop hooks and clears
instances. Removal, disabling and lifetime replacement are observed on the next
update. VM destruction cancels tasks and releases Lua references; teardown code
needing live host services belongs in an explicit stop/restart before the world
is destroyed.

## Limits, memory and profiling

`LuaRuntimeLimits` defaults to 50 million instructions per entry and a 64 MiB Lua
heap per VM. Zero disables the corresponding limit. Configure rule scripts with
`LuaScriptOptions::limits`, entity scripts with
`EntityScriptsConfig::runtime_limits`, scenes with
`ScriptSceneConfig::runtime_limits`, or native scene engines through their
constructor / `set_runtime_limits`.

Loads, callbacks, Lua ECS systems, predicates and task resumes use the shared
instruction/debugger hook. Nested calls share the caller's remaining budget. Task resumes also share a
step budget.
After budget exhaustion, `pcall` cannot keep an infinite loop running. Reload
preparation is capped at one million instructions (or the configured limit, if
lower): a scene script's load and `on_reload` together, an entity script's load,
then each instance's migration (its self and `on_reload`) on its own. Explicit
GC steps also install the hook, although Lua itself disables hooks while running
`__gc` metamethods. Use lifecycle hooks for scripted cleanup and keep finalizers
short; instruction limits cannot interrupt `__gc` or a blocking native function.

The allocator counts the Lua-managed heap, including native binding setup.
Allocation limits are enforced during protected Lua execution and compilation,
where Lua can report allocation failure safely. Host argument pushes are
accounted and checked at protected function entry. Native binding setup, C++ objects
behind userdata, textures and other engine assets have separate ownership and
budgets.
Native GC/VM teardown can allocate to release binding metadata even when a
rejected VM exceeded its heap limit; those cleanup allocations are accounted
without enforcing the execution limit.

`LuaScript::runtime().stats()`, `ScriptEngine::runtime_stats()` and
`EntityScripts::runtime_stats()` expose current/peak heap use, allocations,
denied allocations and per-callback calls, failures, elapsed time and allocated
bytes. Callback allocations include nested calls. GC metrics measure explicit
host steps; automatic GC time is included in its enclosing callback's time.
Simulation task steps perform incremental GC. Native hosts can request a bounded
step with `LuaRuntime::collect`. Every host records its calls in the active native
profile session (`kin/core/profile.hpp`, category `lua`). Timing data stays in
native diagnostics and test reports, outside deterministic gameplay APIs.

## Tasks and waits

All hosts expose managed tasks:

```lua
local job = task.run(function()
    task.wait(0.25)
    task.frames(2)
    task.await(tweens.to('Door', 'Transform2D.pos', {x=100,y=0}, 0.3))
    task.until_(function() return ready_to_continue end, 5)
end)

-- job:state(): pending, running, waiting, completed, cancelled, failed
-- job:error(); job:cancel(); task.cancel(job)
```

`task.await` waits for anything with `state()` and `error()`: tweens
(`tweens.to`, `tweens.of`), preload requests and other tasks. It finishes at
`completed` (`ready` for a preload); preloaded scenes still require an explicit
`commit()`. Cancellation/failure and timeouts fail the waiting task. Animation
completion can use `task.until_` with a checked animation-state predicate.

Tasks start on the next scheduler advance, in registration order, and resume at
most once per advance. Tasks created during an advance start the following one.
Zero-second waits still yield until a later advance. Seconds use simulation
`dt`; frame waits count simulation advances. There are at most 1,024 retained
jobs per VM. A task error is recorded and reported while other tasks remain
available.

`ScriptEngine::call_update` and `EntityScripts::update` advance their tasks.
Plain `LuaScript` hosts call `advance_tasks(dt)`. Entity callbacks automatically
scope new tasks to their entity (an entity script's `self:after`, `self:every`
and a `self:tween`'s `done` are such tasks); tasks created by those tasks
inherit the owner. Destruction, disabling, removal, pool reuse and explicit
restart cancel owned tasks. Scene exit cancels scene tasks. Standalone hosts can
use `set_task_owner(owner)` (a number; 0: none), `cancel_tasks([owner])` and the
`advance_tasks` owner-validation callback.

Accepted reload cancels old Lua tasks. Persisted task handles become cancelled
tokens in the candidate VM; recreate work in `on_reload`. Coroutine stacks and
closures are not migrated, and neither are an entity script's timers or tween
`done` callbacks (tasks too): the tweens themselves go on. Tween and preload
handles kept in reload state keep following their native work. Rejected edits
retain active tasks.

## Lua unit and component tests

```sh
cmake --build build --target kin_lua
build/bin/kin_lua test --root examples/lua_testing/content \
  --json build/lua-tests.json --junit build/lua-tests.xml \
  examples/lua_testing/tests/project.lua
```

```lua
local rules = require('rules')

test('armor reduces damage', function()
    t.equal(rules.damage(12, 5), 7)
end)

test('moving actor', function()
    local hero = t.spawn('Hero', {Transform2D = {}})
    t.attach(hero, 'actor.lua')
    t.input('right', true)
    t.step(2, 0.1)
    t.near(t.get(hero, 'Transform2D').pos.x, 20)
end)
```

Registration runs once for discovery and again in each isolated VM. Keep fixture
work inside test callbacks. Each case gets a fresh ECS world, input, module
cache, fixture random seed and temporary writable content copy. Input profiles
and other native user-data writes use a private directory for each case. Edits
never change the source project. Content fixtures are limited to 4,096 files / 64 MiB. No window
or renderer is created; use gameplay scenarios for rendering and full application
integration.

The `t` fixture provides:

| API | Purpose |
| --- | --- |
| `equal(actual, expected[, message])`, `near(actual, expected[, epsilon])`, `raises(fn[, text])` | Assertions, including nested data tables |
| `spawn(name[, components])`, `get(id, component)`, `patch(id, component, fields)` | Real reflected ECS components |
| `attach(id, script)`, `destroy(id)`, `alive(id)`, `enabled(id, bool)` | Entity lifecycle |
| `input(action, held)`, `step([frames[, dt]])`, `time()`, `frame()` | Input and controlled simulation; defaults to one step at 1/60 s |
| `seed(integer)`, `random()` | Repeatable random values |
| `edit(path, text)`, `reload(path)`, `errors()` | Entity reload and rejected-edit assertions |
| `scene(script)`, `reload_scene(script)`, `reload_error()` | Scene scripting and transactional reload |

`errors()` acknowledges the errors it returns. Unacknowledged entity script
errors fail the case even if the test callback returns normally. Test-task and
scene errors propagate through `step`, so they can be asserted with `raises`.
An unfinished task in the test VM also fails the case: advance it with `step`
or cancel it explicitly. Entity/scene tasks may remain active at fixture teardown.
Plain Lua tables/functions can stand in for a module's game-specific services.

Exit codes are 0 for success, 1 for failed tests/checks, and 2 for CLI/setup errors.
`kin_lua scenario file.lua` compiles a [gameplay scenario](automation.md) to its
JSON (exit 2 on errors).
JSON and JUnit reports include all cases; JSON also includes Lua runtime profiles.
`--instructions N` and `--memory BYTES` configure test VMs and fixture hosts.
`kin-lua-runtime` and `kin-lua-tooling` run these regression checks under CTest.
