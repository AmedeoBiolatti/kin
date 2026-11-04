# Development sessions and safe 2D reload

Development sessions combine watched imports, stateful Lua replacement, live
authored scene/prefab patches and staged asset caches. A candidate revision is
prepared before the running game changes; accepted targets activate together
between updates. Rejected edits leave the active generation running.

## One-command Workshop development

```sh
cmake -S . -B build -DKIN_BUILD_EXAMPLES=OFF -DKIN_BUILD_2D_WORKFLOW=ON
cmake --build build --target kin_2d_workflow_dev
```

The target builds the game, performs an initial import, starts the importer in
watch mode, and launches the game with `--dev --content <imported directory>`.
Edit `examples/2d_workflow/content/game.lua` or the source art. The importer
publishes runtime files; the game prepares and activates a content generation.
Import/reload errors are printed while the previous version continues running.
Closing the game stops the watcher. Initial import/build failures prevent launch.

Use `--config Release` with a Visual Studio build. Reload is explicitly enabled
by `--dev`, independently of `NDEBUG`; optimized development builds work too.
Pillow is required for imports (`tools/requirements-imports.txt`). The launcher
itself uses Python 3.9+ and its standard library.

The CMake import helper generates `<target>-<config>.dev.json`. It can also be
run directly, with extra game arguments after `--`:

```sh
python3 tools/kin_dev.py --project build/examples/2d_workflow/kin_2d_workflow-Debug.dev.json \
  -- --seed=7
```

Other native games with a content/import manifest get a `<target>_dev` target.
They opt into transactional reload through `DevelopmentSession`; `ScriptScene`
and the Workshop supply this integration. Native C++ changes still require a
build and restart.

## Stateful Lua contract

Scripts export a `reload_state` table and implement `on_reload(saved, scene)`:

```lua
reload_state = {version=1, score=0}
local state = reload_state

function on_load(scene)
    -- First start only: construct entities, input contexts and pools here.
end

function on_reload(saved, scene)
    assert(saved.version == 1, 'unsupported state version')
    state = saved
    reload_state = saved
    -- Declare replacement ECS systems with scene:register_system if needed.
end

function update(ctx)
    -- Read/write state; normal scene, workflow and host APIs are active here.
end
```

Reload creates a fresh VM, reads the script and its required modules from the
candidate snapshot, copies state and runs the migration hook under an instruction
budget. `on_load` is not rerun. State supports primitives, nested tables and
checked native tween, prefab-lease and preload handles. Native jobs/leases retain
their ownership across VMs. Lua functions, coroutines, arbitrary userdata, pointers
and excessive/cyclic tables are rejected. Store authored entity IDs and resolve
them during normal gameplay instead of persisting Lua entity wrappers.

Migration can modify its copied data and declare ECS systems. Live world access,
workflow mutations and custom host APIs are unavailable while preparing. System
declarations are validated and installed only on activation; existing systems
continue running after a rejected edit. Host binding callbacks register APIs and
must not perform gameplay side effects themselves.

Validation covers parsing, migration, declarations and bound assets. Errors in
later gameplay code do not reverse already completed gameplay effects. The
Workshop keeps its development window alive, reports the error and accepts a
corrected edit; ordinary test runs retain their failure behavior.

`ScriptEngine::prepare_reload` and `prepare_script_reload` provide this contract
to native hosts. Existing legacy script reload policies remain available outside
a development session; entity-script instances using their legacy polling path
still restart. Arbitrary Lua stacks and closures are not migrated.

## Authored scene and prefab patches

`SceneAssetRuntime::prepare_patch` expands and validates the next scene and its
nested prefabs, then compares authored values against the previous source.
Unchanged authored fields retain their current gameplay values. Changed fields
replace live values; additions/removals and parent/name changes are applied by
authored ID. Surviving entities keep their IDs and compatible native runtime
state. Runtime children of deleted authored entities are detached rather than
deleted. Reflected constraints and reference targets are checked before commit.
Read-only fields remain owned by the runtime.

Supply the loaded runtime through `ScriptSceneConfig::scene_runtime` to combine
its scene/prefab changes with the scene's Lua and asset reload. It must be the
runtime that owns that scene's instantiated authored entities. The Workshop also
uses `PreloadedScene2D::prepare_reload` for its current room. Ordinary preloaded
scenes remain immutable unless the host explicitly enables this development path.

Within a development session, `ScriptScene::reload_script` queues a revision;
check generation/status after advancing. Legacy scene-file polling defers to the
session so it cannot rebuild the live world independently.

Changes requiring a new component schema or an owner's custom transactional
field policy need that owner's reload target or a restart. This API does not
migrate arbitrary native resources. Existing pooled leases keep their runtime
identity; this path does not rebuild every prewarmed pool's prefab topology.

## Content transactions and custom hosts

`DevelopmentSession` captures a bounded immutable content tree and accepts named
prepare callbacks returning `DevelopmentChange`. Preparation must only build and
validate candidate state. `activate` changes ownership/state between steps;
`rollback` undoes an activation if another target fails. `finalize` performs
no-fail cleanup after all activations succeed. Rollback/finalize must not throw.
The session and its callbacks run on the owning thread.

`Assets2D::for_snapshot` creates an isolated cache retaining the revision and
custom binders, budgets and residency settings. Validate it against a preview
world before exchanging live caches. SDL image/font/audio streams can read
snapshots, and custom GLSL compilation materializes the immutable source tree
including relative includes. Queued preloads retain their source snapshot until
their read completes. Old resources survive through their remaining owners.

Import publication creates `.kin-import-publishing` until every output and the
report have been published. Development capture refuses an incomplete import;
a process lock serializes simultaneous configure/build/watch importers and is
released by the OS if a publisher exits.
A failed publication stays blocked until a successful import repairs it. Direct
disk saves settle across two content checks, at least 150 ms apart. A rejected
revision is retried after a source change or an explicit request. Virtual edits
are retained until disk content actually changes.

Snapshots are limited to 4,096 files / 64 MiB. Hidden files/directories are omitted
and symlinks are rejected. Development watching requires loose source content;
shipping packs remain immutable. Content fingerprints identify revisions and are
not cryptographic signatures. Preparation is synchronous and can briefly stall
on large decodes or shader compilation.

## Automation, tests and replay

Start a driven server with `--dev --server`. `server.capabilities` exposes
`development` and method availability. `dev.status {scene?}` reports generation,
attempt, pending/status/error, activation tick, active revision and source hashes.
`dev.reload` queues one request for the next simulation step:

```json
{"id":1,"method":"dev.reload","params":{"changes":{"scripts/tuning.lua":"return {speed=200}"}}}
{"id":2,"method":"sim.tick","params":{"count":1}}
{"id":3,"method":"dev.status"}
```

Without `changes`, the request captures disk content. With `changes`, it applies
relative-path edits to the active snapshot: strings are UTF-8 content,
`{"hex":"..."}` is binary data and null deletes a file. Source files are not
modified. `expected_revision` optionally rejects stale editor requests. RPC
requests record exact changed bytes, bounded at 4 MiB per revision; disk watching
supports larger changes within the snapshot limit.

A queued response is not activation success. Wait for the next generation or
check the resulting rejected status with the scenario runner's bounded conditions.
`automation.snapshot` includes development status for failure traces. Driven mode
never automatically watches disk; reloads happen at recorded command boundaries.
Windowed `--dev --inspect` also allows these explicit development commands.

The [scenario runner](automation.md) records reload inputs and uses the returned
exact edits during replay. Virtual edits preserve normal executable/content
identity checks. For reproductions involving physical disk edits, keep the
original initial content available; replay still verifies initial identities.
The change payload records the activated inputs, not arbitrary external reads.

`kin-development`, `kin-development-python` and `kin-workflow-development` test
transaction rollback, state/system migration, authored patches, the launcher,
real gameplay leases/jobs, rejected edits, repeated activation and replay.
