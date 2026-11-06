# Agent Interface

How to drive a Kin game from an automated agent or test harness without a
display, deterministically, and with structured state to assert on.

This is the reference for the headless and server features. For the testing
workflow, see [testing.md](testing.md); for the design philosophy, see
[manifest.md](manifest.md).

For JSON/Lua scenarios, bounded conditions, recording/replay and failure traces,
see [gameplay automation](automation.md).

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
| `--frames=N` / `--frames N` | Run exactly N fixed-timestep frames; a windowed run instead runs in real time (frame rate cap included) and quits after N rendered frames. |
| `--seed=N` / `--seed N` | Seed the run's root RNG key (`SceneContext::rng`, default 0). |
| `--max-fps=N` / `--max-fps N` | Cap a windowed run at N frames a second (0: uncapped), over the game's own setting. Headless runs are never paced. |
| `--report[=PATH]` | Write the JSON run report to PATH (`-` for stdout). Implies headless. |
| `--services=offline` / `--services=memory` | Platform services from kin's offline backend, not Steam's, kept beside the saves or in memory only ([platform services](platform_services.md)). Headless runs always get them in memory. |
| `--list-actions` | Print available input actions and bindings, then quit. |
| `--game-info` / `--list-info` | Print game metadata, then quit. |
| `--profile-render` | Run 600 frames and print a render timing table. |
| `--profile` | Profile the run: windowed, until it quits or `--frames` N; with `--headless`, a 600-frame pass unless `--frames` is set. |
| `--overdraw-view` | Start with the overdraw view on (SDL_GPU): the screen shows how many times each pixel is shaded, as colours. Also a debug overlay toggle. |
| `--screenshot=PATH` | Save the last frame as a PNG when the run ends. |
| `--locale=TAG` / `--locale TAG` | Show the game in this language (see [localization](localization.md)), over its own choice. Headless and server runs without it use the base language. |
| `--pseudo-locale` | Show the pseudo-locale (`en-XA`): accented, longer, bracketed text, to find untranslated strings and layouts that will not fit. |
| `--fail-on-missing-text` | Fail the run if any text was looked up by a key the shown language lacks. The run report always lists them under `localization.missing_text`. |
| `--fail-on-text-overflow` | Fail the run if any ui2 widget's text did not fit its bounds. The run report always lists them under `ui_overflow`; with `--pseudo-locale` this finds layouts too tight for longer languages. |
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
the process once and drives it with commands. Shipping builds leave it out
(`KIN_ENABLE_AGENT_SERVER`; [shipping](shipping.md)), and `--server` fails there.

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

The server listens on `127.0.0.1` only, so other machines cannot drive the game.
Success is `200`, bad request/params is `400`, and unknown method is `404`.
Replies carry `Access-Control-Allow-Origin: *`; `OPTIONS` preflight is handled.

## Methods

| Method | Params | Returns |
|---|---|---|
| `server.schema` | none | Versioned `kin.automation/1` request/result schemas, method effects and mode flags. |
| `server.capabilities` | none | Current scene providers, available methods, renderer, fixed dt and RNG key. |
| `automation.snapshot` | `{world?}` | Frame/failure status, all scene reports/UI and optional registered ECS state. |
| `dev.status` | `{scene?}` | Opt-in development generation, revision, pending state, source hashes and last reload diagnostics. |
| `dev.reload` | `{scene?, changes?, expected_revision?}` | Queue a development reload at the next step; return exact revision edits for replay. Requires `--dev`. |
| `game.info` | none | Metadata: id, title, version, window, tags, fields. |
| `scene.stack` | none | All scenes with index, name, flags, and `has_world`. |
| `scene.current` | none | Top scene name plus its `write_report` state. |
| `scene.actions` | none | Available input actions and bindings. |
| `world.snapshot` | `{scene?, animations?}` | Flecs world JSON plus animation snapshots unless `animations:false`. ECS scenes only; without `scene`, the topmost scene with a world (under overlays such as transitions). |
| `world.query` | `{scene?, selector?, limit?}` | Filter by ID/authored ID/name/enabled and all/none registered component names; matching count, truncation and entities with decimal string IDs/64-bit fields. |
| `animation.diagnostics` | none | Unresolved-reference diagnostics from the scene's animation library. |
| `ui.snapshot` | `{scene?}` | Semantic ui2 tree from `Scene::ui_context()`: IDs, labels, roles, values, bounds and interaction flags. |
| `ui.query` | `{scene?, selector?}` | Exact semantic predicates with recursive ancestor `within` scope. |
| `ui.action` | `{scene?, id?, selector?, action?, text?}` | Queue `click` (default), `focus`, `set_text`, `increment`, `decrement` or `set_number` (`text` holds the number) for the next tick; choose exactly one of an ID lookup or a unique structured selector. |
| `view.screenshot` | `{path?}` | Render the current scene and save a PNG. Returns path and dimensions. |
| `view.scene` | `{path, content?}` | The top scene shows the scene file `path` in place of its own, as it reads its own (`Scene::view_scene`; a game says how). |
| `view.edit` | `{entities: [{id, components}]}` | The scene shown changed in place: each entity named (its id in the file) takes the fields given, written as the file writes them (`Scene::view_edit`; `SceneSession3D::edit_authored` does it for a 3D scene). An error when the scene can't, and the tool sends the file again. |
| `view.frame` | `{path?, width, height, x?, y?, zoom?, rotation?}` or `{path?, width, height, camera3d: {position?, rotation?, fov?, near?}}` | Draw the top scene at `width`x`height` through a camera centred on world (x, y), `zoom` screen units a world unit (`Scene::set_view_camera`); or, a 3D scene, through a 3D camera at `position` [x, y, z], turned by the quaternion `rotation` [x, y, z, w], `fov` degrees top to bottom (`Scene::set_view_camera3d`). Write its pixels to `path` as raw RGBA rows, top first. Returns path and dimensions. |
| `sim.tick` | `{count?}` | Advance N fixed frames, default 1. Returns frame counter and failure flag. |
| `sim.reset` | `{seed?}` | Rebuild scenes, zero frame/report, clear queued/held input while preserving its map; optionally reseed (decimal string above 2^53-1). |
| `input.action` | `{name, mode?}` | Queue an action for the next tick. `mode`: `press`, `hold`, or `release`. |
| `input.mouse` | `{x, y, button?, mode?, wheel?, space?, dx?, dy?}` | Move the pointer; optionally click or scroll. `dx`/`dy` (without `x`/`y` if wanted) move the mouse by that many window pixels, as relative mouse mode reports it: a first-person camera turns. |
| `input.text` | `{text}` | Deliver text input for the next tick. |
| `input.drop` | `{path, x?, y?, space?}` | Drop a file on the window at `x`, `y` (or where the pointer is), as from the desktop: `Input::take_dropped_files` has it at the next tick. |
| `input.key` | `{name, mode?, modifiers?}` | Queue a key by name (`Enter`, `Left`, `PageDown`, `A`, ...), with `modifiers` such as `["ctrl","shift"]` held alongside it. `mode`: `press`, `hold`, or `release`. |
| `input.touch` | `{x, y, id?, mode?, space?}` | A finger at `x`, `y` (logical UI space, or `space: "window"`): `mode` `down`, `move`, `up`, or `tap` (the default: down this tick, up at the start of the next). `id` tells fingers apart. Touch's time is the ticks' (frame × fixed dt), so a long press is a finger held down over enough ticks. |
| `input.pad` | `{player?, button?, mode?, left_x?, left_y?, right_x?, right_y?, left_trigger?, right_trigger?, connected?}` | Drive a player's game controller (`player` 0 to 7, default 0) as a real one: a `button` (`a`, `start`, `dpad_left`, ...) with `mode` `press`, `hold` or `release`, and sticks (-1 to 1, y up) and triggers (0 to 1), kept until changed. The game reads it as that player's controller (`Input::player_gamepad`), and the first player's (else the lowest driven) as `Input::gamepad()`. Driven once, it is plugged in until `connected: false` (or `sim.reset`). |
| `server.status` | none | Frame counter, fixed dt, scene depth, failure flag. |
| `frame.timing` | `{since?, reset?}` | Update, render and present wall time over the last 600 ticks (or frames drawn, inspected): `last`, `mean` and `max`, in ms. With `since` (a frame number; -1 for all kept), `history` lists each frame after it, oldest first: `frame`, `update_ms`, `render_ms`, `present_ms`, `interval_ms` (since the frame before began; 0 when ticked). `reset` clears the history after reporting. |
| `ping` | none | `{"pong": true}`. |
| `server.shutdown` | none | `{"bye": true}` and stop the server. |

Scenes that expose ui2 return their context from `Scene::ui_context()`;
`MenuScene` does this automatically. Other scenes return an explanatory error.
Semantic IDs are decimal strings so JSON clients preserve every bit. See
[UI automation](ui.md#automation) for the tree and supported controls.

## Inspecting A Game While It Is Played

`--inspect` runs the game in its window as usual (real time, the player's
input) and also answers the stdio server's requests between frames: a tool
such as an editor watches it while it is played. The log stays on stderr, so
stdout carries only the answers (and whatever the game prints itself).

```text
game --inspect
{"id":1,"method":"world.snapshot","params":{"raw":false,"events":false,"animations":false}}
{"id":2,"method":"sim.pause"}
{"id":3,"method":"sim.step","params":{"count":1}}
{"id":4,"method":"sim.resume"}
```

It answers what reads the game (`game.info`, `scene.stack`, `scene.current`,
`scene.actions`, `world.snapshot`, `ui.snapshot`, `animation.diagnostics`, `ping`),
`frame.timing` for the frames it draws (a profiler asks with `since`), and
`server.status` with `paused`. Three methods hold its updates while it keeps
drawing:

| Method | Params | Effect |
|---|---|---|
| `sim.pause` | none | Hold the scenes' updates. Returns the status. |
| `sim.resume` | none | Update again. |
| `sim.step` | `{count?}` | Run N more updates (default 1), then hold. |

Answers are written on a thread of their own, in order: a tool slow to read
a large one holds that thread, not the game.

`server.shutdown` quits the game. What drives a headless server (`sim.tick`,
`sim.reset`, `input.*`, `view.*`) is refused. When the
tool closes its end of stdin the game goes on, never left paused. Headless
runs and builds without the scene server ignore `--inspect`, with a warning.

### Debugging Its Scripts

An inspected game's Lua scripts (`LuaScript`, and so a scene's entity
scripts) stop where the tool asks. A script file is named as it is under the
content root, `/` between folders (`scripts/player.lua`).

| Method | Params | Effect |
|---|---|---|
| `debug.breakpoints` | `{files: {"scripts/a.lua": [3, 7]}}` | Set every breakpoint, replacing those before. Returns `{breakpoints}`, the lines set. |
| `debug.pause` | none | Stop at the next line any script runs. |
| `debug.state` | none | `{stopped}`; when stopped, `serial` (counts stops), `reason` (`breakpoint`, `step`, `pause`) and `frames`, innermost first: `function`, `file`, `line`, `locals` and `upvalues`. |
| `debug.continue` | none | Go on from a stop, to the next breakpoint. |
| `debug.step` | `{how?}` | Go on to the next line: `into` a call made there, `over` it (the default), or `out` of the function. |

A value is `{name, value, kind}`: `value` as text (`12.5`, `"text"`,
`table (3)`, a userdata as its `__tostring` gives it), `kind` Lua's type. A
table's `fields` follow, two tables deep and 64 fields each (`more` when some
are left out).

While a script is stopped the game is held inside it: it does not update or
draw. Only `debug.*`, `server.status` (`stopped: true`), `ping` and
`server.shutdown` are answered then; the rest wait, and are answered in order
once it goes on. `debug.breakpoints` and `debug.pause` are taken as soon as
they are read, before the next frame, so breakpoints sent as the game starts
stop its first update. Lines are watched only while a breakpoint, a pause or
a step is set, so a game whose tool sets none runs its scripts at full pace. If the
tool closes stdin the script goes on and its breakpoints are cleared.

## Input

Injected input (`input.action`, `input.mouse`, `input.text`, `input.key`, `input.pad`, `input.touch`, `input.drop`) is queued and applied
on the next `sim.tick`, after `begin_frame`, so the scene observes it that frame.
A `press` is a tap: held for that tick and released at the start of the next
one, which sees the release, as with a real button. A ui2 click completes on that
second tick. ui2 also resolves hover from the previous frame, so to click a
widget, move the pointer there and tick once before the `press`. Use `hold` and
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
| `Scene::ui_context() -> ui2::Context*` | `ui.snapshot`, `ui.action` | Return the scene's active ui2 context; `MenuScene` does so automatically. |

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

For semantic menu tests, read `ui.snapshot`, queue a `ui.action` by label/key/ID,
then tick once and check the scene report. These actions are deferred until a
tick; intermediate snapshots/screenshots do not activate controls. Only one may
be pending per step. `input.mouse` remains available for pointer-specific tests.

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
