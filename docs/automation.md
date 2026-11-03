# Gameplay automation and replay

Kin's driven scene server supports reusable JSON and Lua gameplay scenarios,
bounded conditions, session recordings, failure traces and API discovery. The
Workshop includes [JSON](../examples/2d_workflow/tests/workshop.json) and
[Lua](../examples/2d_workflow/tests/workshop.lua) examples that exercise gameplay,
semantic UI, world queries, room loading and reset.

## Run a scenario

Build with `KIN_ENABLE_SCENE_SERVER=ON` (the development default). Lua scenario
files additionally require scripting and the `kin_scenario` compiler. The Python
runner needs Python 3.9+ and uses only its standard library. The Workshop's asset
import step separately needs Pillow from `tools/requirements-imports.txt`.

```sh
cmake -S . -B build -DKIN_BUILD_EXAMPLES=OFF -DKIN_BUILD_2D_WORKFLOW=ON
cmake --build build --target kin_2d_workflow kin_scenario
python3 tools/automation/kin_test.py run examples/2d_workflow/tests/workshop.json \
  --out /tmp/workshop-run --trace-world --game build/bin/kin_2d_workflow
python3 tools/automation/kin_test.py run examples/2d_workflow/tests/workshop.lua \
  --out /tmp/workshop-lua --lua-compiler build/bin/kin_scenario \
  --game build/bin/kin_2d_workflow
```

Place all runner options before `--game`; the rest is the game's executable and
arguments. Use the usual `.exe`/configuration subdirectory on Windows. The runner
starts a fresh process with driven stdio, a seed, and the output directory as its
working directory. Each output directory must be empty. `--content` and `--pack`
paths are resolved before launch; resolve other game-specific paths yourself.
Screenshots are confined to the working directory by the server.

A minimal JSON file:

```json
{
  "schema": "kin.scenario/1",
  "name": "Fire once",
  "seed": "7",
  "requires": ["ui"],
  "steps": [
    {"call": "ui.action", "params": {"selector": {"key": "fire"}}},
    {"wait": {"observe": "scene.current", "path": "/state/shots", "value": 1, "within": 4}},
    {"always": {"observe": "scene.current", "path": "/state/paused", "value": false, "ticks": 3}}
  ]
}
```

The runner validates the whole plan against the discovered request schemas before
executing its first action. Required capabilities fail explicitly when absent.
Unknown methods, misspelled fields, unavailable providers and malformed selectors
fail instead of becoming empty observations. Exit codes are 0 for success, 1 for
test/process failure and 2 for configuration errors. `result.json` and `junit.xml`
are suitable for CI. Bootstrap/configuration errors can occur before a result
bundle exists; stderr and the CLI exit status remain authoritative.

## Simulation time and conditions

Each step has exactly one operation: `call`, `tick`, `expect`, `wait`, `always` or
`capture`, with an optional `name`. Calls accept `params`. A capture uses a safe
filename stem and saves a PNG. A tick advances 0..1,000,000 fixed simulation steps.

Conditions specify an `observe` RPC, optional `params`, a JSON Pointer `path`
(empty means the whole result), an `op` and usually `value`. Operations are `eq`
(default), `ne`, `lt`, `le`, `gt`, `ge`, `approx` with nonnegative `tolerance`,
`exists`, `absent` and `contains`. Equality distinguishes booleans from numbers;
missing fields differ from JSON null. Pointer escapes are `~0` and `~1`.

`expect` checks now. `wait.within=N` checks now, then after each of at most N
single simulation ticks; success at tick N is accepted. `always.ticks=N` checks
now and after every tick, stopping at the first violation. Zero checks only the
current state. Actions are issued once; only read/layout observations are retried.
Conditions never use wall-clock sleeps. `--timeout` (15 seconds by default) is a
separate deadline for a protocol response and catches hung/crashed processes.

Lua files build the same data through `test{...}`, `call(method, params)`,
`expect{...}`, `wait_until{...}`, `always{...}`, `tick(n)` and `capture(stem)`.
The compiler uses Kin's Lua sandbox and a 500,000-instruction budget. It rejects
holes, mixed object/array tables, functions, nonfinite values and excessive
nesting. Omit empty optional arrays: an empty Lua table becomes a JSON object.
Use decimal strings for seeds above 2^53-1 to avoid Lua/JSON floating-point loss.

## Discover and select

`server.schema` returns `kin.automation/1`, version 1, with method request/result
schemas, effects and driven/inspector availability. Result schemas describe
stable envelopes; game-authored state and component fields remain open objects.
`server.capabilities` returns the current mode, fixed dt, RNG key, renderer,
scene providers and per-method availability. Re-query it after scene changes.

`ui.query` accepts exact `id`, `key`, `role`, `label`, `value`, `visible`,
`enabled`, `blocked`, `checked` and `focused` predicates. `within` recursively
matches an ancestor, up to 16 scopes. Queries include disabled/hidden elements
unless filtered. `ui.action` accepts either the existing `id` lookup or a
structured selector; a structured selector must match exactly one actionable
element. Ambiguity and disabled/hidden/blocked targets are errors. Actions take
effect on the next tick. Prefer stable semantic keys over changing labels.

```json
{"method":"ui.action","params":{"selector":{"key":"fire","role":"button","within":{"key":"control_room"}}}}
```

`world.query` accepts `scene`, `limit` (default 256, maximum 4096) and a selector
with `id`, `authored_id`, `name`, `enabled`, `all`/`none` component-name arrays.
It returns registered components and matching entities, `count` before the limit,
and `truncated`. Unknown component names are errors. Entity/component IDs,
generations, creation sequences, reflected 64-bit fields and entity references
are decimal strings in this endpoint. Prefer authored IDs across resets.

```json
{"method":"world.query","params":{"selector":{"authored_id":"player","all":["Transform2D"]},"limit":8}}
```

`automation.snapshot` collects frame/failure status and every scene's report/UI.
`world:true` adds registered ECS snapshots. Existing `world.snapshot` retains its
original format, including numeric IDs. The windowed inspector exposes these
read queries without enabling input mutations; UI queries use its latest layout.
`sim.reset` clears queued/held input and frame/report history, preserves the input
map, rebuilds scenes and optionally reseeds. Seeds above 2^53-1 must be strings.

## Record and replay

Every Python run writes a flushed `commands.jsonl` session. It records requests
before sending them, responses, checkpoints, steps, failed assertions and stderr
logs. A crash therefore retains the last in-flight request. Seed, executable
SHA-256, command arguments, declared dependencies and runner identity accompany
the recording. Scenario `dependencies` are relative to the scenario file;
`--dependency PATH` adds further files/directories. Existing `--content`/`--pack`
arguments are included automatically. Directory hashing excludes dot paths and
`__pycache__`; explicitly list any such inputs that matter. Script arguments,
system libraries, environment variables and implicit file reads are not inferred.
The output directory must be outside declared dependency directories.

```sh
python3 tools/automation/kin_test.py replay /tmp/workshop-run/commands.jsonl \
  --out /tmp/workshop-replay --game build/bin/kin_2d_workflow
python3 tools/automation/kin_test.py record --seed 7 --out /tmp/manual-session \
  --dependency path/to/content --game build/bin/kin_2d_workflow
```

`record` proxies manually entered JSON-RPC lines on stdin, echoing responses with
the caller's ID. Finish with `server.shutdown` or EOF. Games can also write a
native recording with `--automation-record session.jsonl` (driven mode only), or
`ServerContext::start_recording`. Native recordings include seed/RNG, dt,
game version, requests, results and per-tick reports/UI; they do not hash the
executable/content. Supply `--seed` when a native header has no raw seed.
These surfaces record scene-server commands, not physical window/controller input.

Replay checks initial RNG/fixed dt, native game ID/version where supplied, and
Python build/content identities, then issues the recorded requests in order.
Changed identities/arguments require explicit
`--allow-identity-change`. End-of-run hashing also detects declared content
changed during a run. Captures get new paths inside the replay directory.
Response comparisons exclude timings, capability/schema discovery, raw world
snapshots and capture filenames. Native recordings also compare the report/UI
checkpoint at each tick command's end. `--no-compare` disables these response
comparisons; recorded assertion/protocol failures still fail. Incomplete sessions never pass
silently. Keep generated input files under declared dependencies where needed.

Replay is an observable regression check, not a promise of cross-platform,
bitwise simulation determinism. Async job completion, external I/O, device state
and unreported game state can differ. Prefer bounded waits in reusable scenarios
and expose meaningful counters/state through `Scene::write_report`.

## Failure evidence and CI

Each run writes `schema.json`, `capabilities.json`, `stderr.log`,
`commands.jsonl`, `result.json`, `junit.xml` and an offline `trace.html` with a
filterable timeline. Failures additionally request `state.json`, `ui.json`,
`world.json` (including engine world events) and `failure.png`. `failure.json`
contains the failed condition, bounded state diff, stderr tail and unavailable
artifact reasons. Crashes/timeouts cannot supply fresh in-process snapshots.
Lua stack errors, asset failures and job information appear when the scene's
report, world or log providers expose them; the runner cannot reconstruct hidden
application state.

Default checkpoints cover scene reports/UI. Add `--trace-world` to collect ECS
state at each boundary for component-level diffs, at higher storage cost. The
final failure snapshot requests world state either way. Diffs are capped at 100
fields and identify their coverage. The HTML retains the last 2,000 events,
linking large records as separate JSON artifacts; the complete JSONL is bounded
at 256 MiB. Raw stderr is capped at 64 MiB and each protocol line at 16 MiB.

`kin-automation`, `kin-automation-python` and `kin-workflow-automation` cover the
native API, process driver and real Workshop/compiler/recording workflow. The
Workshop test is registered when its target, scene server and Lua compiler are
enabled. Desktop and GPU CI install import dependencies and upload Workshop
failure traces. The runner does not start the read-only inspector or an HTTP
server; use the [agent interface](agent_interface.md) for those interactive modes.
