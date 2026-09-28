# Agent Interface

How to drive a Kin game from an automated agent or test harness without a
display, deterministically, and with structured state to assert on.

This is the reference for the headless and server features. For the testing
workflow, see [testing.md](testing.md); for the design philosophy, see
[manifest.md](manifest.md).

Every game built on `run_scene_app` gets this interface from the shared runtime
flags.

## Run Modes

| | One-shot headless run | Server |
|---|---|---|
| Flag | `--headless` / `--report` | `--server` |
| Lifetime | Runs N frames, prints a report, exits | Long-lived; processes commands until shut down |
| Use | Scripted assertions, CI smoke tests | Interactive play, exploration, agent loops |
| Time | Fixed frame count | Advances on `sim.tick` (driven) or wall clock (realtime) |

Both modes run headless by default when requested, use a fixed timestep, and take
an explicit `--seed`.

## Headless Runs

| Flag | Effect |
|---|---|
| `--headless` | Run headless. Defaults to 1 frame. |
| `--frames=N` / `--frames N` | Run exactly N fixed-timestep frames. |
| `--seed=N` / `--seed N` | Seed the run's root RNG key (`SceneContext::rng`, default 0). |
| `--report[=PATH]` | Write the JSON run report to PATH (`-` for stdout). Implies headless. |
| `--list-actions` | Print available input actions and bindings, then quit. |
| `--game-info` / `--list-info` | Print game metadata, then quit. |
| `--profile-render` | Run 600 frames and print a render timing table. |
| `--profile` | Run a profiling pass. Defaults to 600 frames unless `--frames` is set. |
| `--profile-lines` | Enable compiled-in Kin profile line/block capture. |
| `--profile-json=PATH` | Write `kin.profile/1` JSON to PATH (`-` for stdout). |
| `--profile-text=PATH` | Write a text profile summary to PATH (`-` for stdout). |

Runs are deterministic by construction: fixed timestep, explicit frame count, and
explicit seed. Exit code `0` means a clean run. Exit code `1` means a scene called
`ctx.report->fail("reason")`; the run stops early in that case.

`--report` emits a structured snapshot at the end of the run:

```json
{
  "schema": "kin.run_report/1",
  "status": "ok",
  "seed": 7,
  "frames": 5,
  "scenes": [
    { "name": "Play", "state": { "score": 3, "game_over": false } }
  ]
}
```

Each scene contributes assertable state by overriding `Scene::write_report`:

```cpp
void write_report(kin::JsonWriter& json) const override {
    json.field("score", _score);
    json.field("game_over", !_alive);
}
```

## Server Mode

`--server` runs the game as a long-lived JSON-RPC command server. An agent starts
the process once and drives it with commands.

```text
--server-transport=stdio|http      # default stdio
--server-mode=driven|realtime      # default driven
--port=N                           # HTTP only, default 8080
--seed=N                           # deterministic root RNG key
```

Driven mode advances time only when the client calls `sim.tick`, which makes the
command log a replayable scenario. Realtime mode steps at wall-clock pace and
drains commands between frames.

### stdio

Requests and responses are newline-delimited JSON:

```text
request:   {"id": <any>, "method": "<name>", "params": { ... }}
response:  {"id": <echoed>, "result": { ... }}
           {"id": <echoed>, "error": {"message": "..."}}
```

Example:

```powershell
$cmds = @(
  '{"id":1,"method":"game.info"}',
  '{"id":2,"method":"sim.tick","params":{"count":40}}',
  '{"id":3,"method":"scene.current"}',
  '{"id":4,"method":"server.shutdown"}'
) -join "`n"
$cmds | .\build\bin\ecs_systems_demo.exe --server --seed=7
```

### HTTP

`--server-transport=http --port=N` exposes the same methods over HTTP. The
request path is the method, the request body is the params object, and the
response body is the result object directly.

```bash
ecs_systems_demo --server --server-transport=http --port=8137 &

curl -s localhost:8137/game.info
curl -s -X POST localhost:8137/sim.tick    -d '{"count":3}'
curl -s -X POST localhost:8137/input.mouse -d '{"x":320,"y":306,"button":"left","mode":"press"}'
curl -s localhost:8137/world.snapshot
curl -s localhost:8137/server.shutdown
```

Success is `200`, bad request/params is `400`, and unknown method is `404`.
Replies carry `Access-Control-Allow-Origin: *`; `OPTIONS` preflight is handled.

## Methods

| Method | Params | Returns |
|---|---|---|
| `game.info` | none | Metadata: id, title, version, window, tags, fields. |
| `scene.stack` | none | All scenes with index, name, flags, and `has_world`. |
| `scene.current` | none | Top scene name plus its `write_report` state. |
| `scene.actions` | none | Available input actions and bindings. |
| `world.snapshot` | `{scene?, animations?}` | Flecs world JSON plus animation snapshots unless `animations:false`. ECS scenes only. |
| `animation.diagnostics` | none | Unresolved-reference diagnostics from the scene's animation library. |
| `ui.snapshot` | `{scene?}` | Unavailable after legacy `kin/ui` removal; use reports or `world.snapshot`. |
| `view.screenshot` | `{path?}` | Render the current scene and save a PNG. Returns path and dimensions. |
| `sim.tick` | `{count?}` | Advance N fixed frames, default 1. Returns frame counter and failure flag. |
| `sim.reset` | `{seed?}` | Rebuild the scene stack from the game's factory; zero frame/report; optionally reseed. |
| `input.action` | `{name, mode?}` | Queue an action for the next tick. `mode`: `press`, `hold`, or `release`. |
| `input.mouse` | `{x, y, button?, mode?, wheel?, space?}` | Move the pointer; optionally click or scroll. |
| `input.text` | `{text}` | Deliver text input for the next tick. |
| `server.status` | none | Frame counter, fixed dt, scene depth, failure flag. |
| `ping` | none | `{"pong": true}`. |
| `server.shutdown` | none | `{"bye": true}` and stop the server. |

`ui.snapshot` intentionally returns an error:

```json
{"message":"ui.snapshot is unavailable after legacy kin/ui removal; use reports or world.snapshot"}
```

## Input

Injected input (`input.action`, `input.mouse`, `input.text`) is queued and applied
on the next `sim.tick`, after `begin_frame`, so the scene observes it that frame.
A `press` is a one-frame tap that is auto-released afterward; use `hold` and
`release` for sustained input.

`input.mouse` coordinates default to game logical space. Pass `"space":"window"`
to send raw window-pixel coordinates, which is useful for native-pixel overlays
such as the F1 debug panel.

## What A Scene Exposes

The server reads a scene through optional hooks:

| Hook | Powers | Notes |
|---|---|---|
| `Scene::write_report(JsonWriter&)` | run report, `scene.current` | Override to expose assertable game state. |
| `Scene::world() -> EcsWorld*` | `world.snapshot` | `EcsScene` returns its world automatically. |
| `Scene::animation_library()` | `animation.diagnostics` | Return the scene's animation asset library. |

Perception by game style:

- ECS games expose entities and components through `world.snapshot`; ECS-authored
  `ui2` component state appears there too.
- Immediate-mode `ui2` should be driven with input commands and expose test state
  via `write_report`.
- Plain non-ECS state is exposed by implementing `write_report`.

## Driving A Menu

Prefer semantic actions when the game exposes them:

```text
scene.actions
input.action {name:"accept", mode:"press"}
sim.tick {count:1}
scene.stack
```

For pointer-driven menu tests, keep button coordinates in `write_report` state or
use fixed coordinates from the game's logical layout, then send `input.mouse`.

## Architecture

- `run_scene_app` parses flags and delegates to either the headless run or
  `run_scene_server`.
- `run_scene_server` owns the App, Window, Renderer, SceneManager, and transport
  loop.
- `ServerContext` holds the live engine objects and the per-tick input queue.
- `ServerContext::step()` advances exactly one fixed frame.
- `dispatch_server_command(ctx, game, method, params)` is transport-agnostic and
  is unit-tested directly in `tests/server_tests.cpp`.
- JSON is hand-rolled through `JsonWriter` and `parse_json`.

## Screenshots And Episodes

`view.screenshot` renders the current scene headless and writes a PNG through the
renderer backend. This is pixel-level perception for visual state.

`sim.reset {seed?}` starts a fresh deterministic episode without restarting the
process. It rebuilds the scene stack from the factory supplied to `run_scene_app`
and zeroes the frame counter and run report:

```cpp
const auto build_scenes = [](kin::SceneManager& scenes) {
    scenes.push(make_initial_scene());
};
kin::SceneManager scenes;
build_scenes(scenes);
return kin::run_scene_app({ .window = ..., .game = &game,
                           .reset_scenes = build_scenes }, scenes);
```

Without a `reset_scenes` factory, `sim.reset` returns an error.

## Not Yet Implemented

- Render targets / multi-pass capture. Screenshots use the default backbuffer.
- A retained scene-graph diff or event stream. State is read by polling reports
  and snapshots between ticks.
