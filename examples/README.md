# Interactive performance examples

Two asset-free native applications share workload code with `kin_bench`.
They are examples, playable/interactive demos, and reproducible performance
fixtures. Neither requires downloaded assets, an account, or a network service.
Run Observatory uses system fonts with the engine's bitmap fallback.

## Build and run

From the repository root (Release recommended):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DKIN_BUILD_EXAMPLES=ON -DKIN_BUILD_BENCHMARKS=ON
cmake --build build --config Release --target signal_siege run_observatory kin_bench
```

Run `build/bin/signal_siege` or `build/bin/run_observatory`. On Windows use
`.exe`; multi-configuration generators may place them in `build/bin/Release/`.
`KIN_BUILD_EXAMPLES` defaults to ON when kin is the top-level project and is
independent of `KIN_BUILD_GAMES`.
Set it OFF to omit both apps and their optional `kin_bench` cases.

## Signal Siege

A top-down arena survival game. Survive 90 seconds against three enemy classes:
fast chasers, slow armored units, and ranged orbiting units. Enemy speed grows
over six wave phases. Killed enemies respawn to sustain the configured load.
Collect green cores to repair the player and improve weapon fire rate; ten
cores unlock a second projectile. Dash grants brief immunity.

It opens on a title screen over an autoplaying arena: **Play**, **Watch
autoplay** or **Quit** (arrows or W/S and Enter, or the mouse). A run that ends
fades to a results screen (time survived, kills, cores, upgrades) whose counters
roll up; Enter retries, Escape returns to the title. The best run is kept between
sessions with kin's `SaveStore`. Screens change with kin's scene transitions
(iris, dissolve, wipe, crossfade; a plain fade on the software backend).

- WASD: move; mouse: aim; hold left mouse: fire.
- Space: dash (1.5-second cooldown); P: pause; R: restart with the same seed.
- B: toggle invincible autoplay; Escape: back to the title (quits when the run was
  started from the command line); F1: engine timing overlay.
- U: open/close the Power Grid (simulation pauses); Escape returns from the grid.
- On the grid, click a square or use Left/Right to select, then click Install or
  press Enter. Connected prerequisites must be installed first. Three starting
  cores let you choose one branch immediately; collected cores fund more nodes.
  There are **36 purchasable upgrades**, six per connected branch: Mobility,
  Weapons, Salvage, Ballistics, Defense, and Recovery. Effects include movement,
  dash duration/speed/cooldown, damage, fire cadence, spread volleys, projectile
  speed/lifetime, attraction, armor, maximum health, regeneration and core repairs.
  Tier costs are 3, 5, 6, 8, 10 and 12 cores. All upgrades reset with R.
  Six always-visible tabs select a branch. Its six connected nodes fit in two
  rows on small windows and one row on wide windows; no panning is needed.
  Left/Right chooses an upgrade; Up/Down or the wheel switches branch. Main Enter
  and keypad Enter install, with immediate success or failure feedback.
  No exclusive branches or permanent purchases.
- Its text is in English, French, Japanese and Arabic (`examples/content/lang/*.kinlang`,
  reloaded as they are edited; packed into `signal_siege.kinpak` when it ships). It starts in the system's language; **LANGUAGE**
  on the title cycles them and is kept with the best run. `--locale=ja`,
  `--pseudo-locale` and `--fail-on-missing-text` work as in any kin game
  ([localization](../docs/localization.md)).
- `--power-grid`: start on the upgrade screen, also usable for screenshot runs.
- `--mute`: no sound.

Headless, `--benchmark`, `--screenshot` and `--power-grid` runs skip the title and
start an autoplaying arena, as before. They and scene-server runs are silent and
never read or write the saved best run.
- Default: 300 enemies. `--stress`: 1,200. `--enemies N`: up to 100,000 (a stress
  test; see below).

Uses ECS transforms/renderers, a cached movement query, a spatial grid for
projectile collision, bounded projectile/effect/pickup storage, camera culling,
render-queue sorting, and a HUD. The playfield is 3,072 x 2,048 world units.
Both apps run simulation at 120 Hz; UI interactions run during rendering.

Its look is built at startup with no asset files (`arena_art.cpp`), and shows off
several kin systems:

- **Sprites** rasterized from signed-distance shapes at twice their on-screen
  size and shaded as hard-surface metal: an outline, a bevelled rim lit from the
  top left, panel seams, rivets and glowing parts (the player's canopy and
  nozzles, the enemies' eyes and visors). A star, an armored plate and a gyro ring
  stand for the three enemy classes; cores are faceted gems. Enemies are drawn by
  the ECS render system through `TextureRenderer`, each over a soft drop shadow.
- **A steel deck** of four tile variants (plain, grate, hazard paint, patched),
  chosen per tile by a hash, with hazard strips along the arena's edge and reactor
  housings whose fans turn with simulation time.
- **2D lighting** (`LightLayer`) of the environment: a dim ambient, the player's
  light and a flashlight cone that follows the aim, amber reactor lamps, impacts,
  and orbiters about to fire.
- **Particles** (`ParticleSystem`) from the arena's events: hit sparks, kill
  bursts in each enemy's colour, core pickups, dashes, damage, and twin thruster
  plumes, drawn additively.
- **Post-processing** on the GPU backend: bloom, a vignette and a light grade.
  The Power Grid screen turns it off to keep its text crisp; the software backend
  presents without it.

Colour keeps the screen readable at 300 enemies: cyan and white are only the
player (the ship, drawn over everything with a ground ring, and its streaking
shots); enemies are warm, solid shapes drawn at full colour after the lighting
(red stars, orange plates, purple rings) with no glow; enemy bullets are the one
round, white-cored hot pink-red thing; cores are small green gems that blink
before they expire; an orbiter about to fire shows a closing ring. The floor stays
neutral steel, and only shots, cores and effects glow.

The art only reads the simulation: collisions, randomness and checksums are the
same with or without it, and the `kin_bench` arena cases, which never attach it,
measure the plain render path. Effects advance with simulation time, so pausing
freezes them, and there is no camera shake or full-screen flash to disturb aim.
On the GPU the look costs well under a millisecond a frame; SDL's software
renderer, used headless, takes about 28 ms a frame at the default load.

Sound is synthesized at startup (no audio files) and played with kin's
`AudioEngine`: shots, hits, kills, pickups, dashes and damage, positioned around
the player so distant fights are quieter, plus wave, win, lose and menu cues and
a looping ambient drone. The clips are registered with `AudioEngine::add_clip`.

The HUD is built from ui2 widgets: glass panels, hull and dash bars (the hull bar
shifts colour and flashes when hit), a countdown with wave progress, rolling kill
and core counters, an "upgrades ready" notice, a `TargetReticle`, and a wave
banner animated with kin animation property tracks. The controls fade out after
the opening seconds and return while paused.

The arena keeps a fixed 1,280 x 800 logical view, letterboxed to fit the window.
Its HUD uses native-resolution system text and automatic display scaling;
smaller windows wrap controls instead of shrinking the font. Pause/end panels
scale with the HUD, while movement, aiming, and the visible playfield remain
independent of UI scale. `--ui-scale 1.5` optionally overrides automatic scaling
for this app too.

## Signal Breach

Signal Siege seen from inside, first person, on kin's experimental 3D
renderer (built with `-DKIN_EXPERIMENTAL_3D=ON`; see
[the plan](../docs/3d_plan.md)). The same three enemy kinds fly round a deck
of floors, ramps, ledges and pillars; survive 90 seconds and collect cores.
Signal Siege itself draws in 3D with `signal_siege --view 3d`.

- Click to play: the mouse is captured and looks around.
- WASD: move; left mouse: fire; Space: jump; Shift: dash.
- Esc: pause and let the mouse go (again: quit); R: restart; B: autoplay.
- `--sensitivity DEG` (degrees a pixel, 0.08), `--enemies N`, `--benchmark`.
- `--latency-probe`: times pushed mouse motion until the camera turns and the
  frame is presented (printed at exit).
- `--no-shadows`, `--no-bloom`, `--no-lights`: leave a feature off (to measure
  it); `--mute`.

Its gun and crates are glTF models in `content/models`, made by
`tools/models/make_breach_models.py` (Python, no packages needed); run it
again while the game runs and they load again.

## Sponza Walk

A viewer for the experimental 3D renderer (`-DKIN_EXPERIMENTAL_3D=ON`): fly a
camera round Sponza (scale: metres) with every renderer feature switchable
from a panel. Sponza is not in the repository (Crytek's licence): fetch it
with `python3 tools/reference/fetch.py` into `$KIN_REFERENCE_CACHE` or
`~/.cache/kin/reference`, or pass `--model PATH`. It needs the SDL_GPU backend.

- Click the picture to fly: WASD, mouse look, Shift fast, Q or Ctrl down, E or
  Space up, wheel changes the speed. Esc (or Tab) lets the mouse go and shows
  the panel; Esc again quits. 1 and 2 jump to the reference scene's `nave` and
  `roof` cameras (it starts at `nave`).
- The panel (scrolls with the wheel; sections fold) has a switch or slider for
  the sun (angles, intensity, L animates it), fill light, hemisphere and ground
  ambient, an environment (HDRIs from the cache), point lights (K drops one at
  the camera), shadows (map size, cascades, distance, area, and a switch that keeps the map while nothing moves, `--set shadow_cache=on` to turn it on), ambient occlusion,
  contact shadows (distance, thickness, intensity, steps), temporal anti-aliasing
  (history kept, sharpen, jitter scale and samples, a freeze-jitter debug switch and a
  reset-history button), levels of detail (on, error in pixels, bias, force a level; the meshes' levels
  are built at load, `--lod-levels N`, default 4, 1 builds none; `--preset lod`; `--lod-clusters` builds clusters instead, cut by the same error bound, `--preset clusters`; the cut runs in compute where the device can, with a switch and a read-back of its counts in the panel, `--preset clusters-gpu`; occlusion culling of clusters behind others, a switch in the panel and `--preset occlusion`, which pays only where much is hidden), MSAA, background, culling, wireframe, field of view, HDR, exposure, bloom,
  and every material feature (textures, normal maps, metallic-roughness maps,
  emissive, cut-out, double-sided, shadows, unlit, roughness and metallic
  overrides). F3 shows the stats (frame, CPU and GPU time, draws, triangles,
  shadow casters per cascade (or that the map was kept from the frame before), occlusion and contact-shadow work, the video memory of the opt-in features' targets), F12 or P saves a PNG of the
  picture to the current directory, F2 prints the settings and a fragment for a
  reference scene (`tests/reference/scenes/*.json`).
- The "Character (skinned)" section spawns a procedural skinned character (three joints; clips Sway, Bend, Wave and a 1D blend of Sway
  and Wave by a slider) as ECS entities (`SkinnedMeshRenderer`, `Animator3D`) in a row in front of the camera (1 to 64), with speed and size;
  changing the clip is a state change, which crossfades. The stats show skinned copies, draws and joint matrices. `--preset character` and
  `--preset character-all` (with occlusion, contact shadows, TAA and four cascades). The panel's click path is untested here (no display).
- The "Character IK" section (the Person rig: 25 joints, procedural, same clip names; toes and a tail among them) poses the same
  entities by inverse kinematics (`Animator3D::ik`): a hand to a target (sliders for its position from the left shoulder, which way the
  elbow points, weight, fade, how much the arm may stretch; a red ball marks the target), the head looking at the camera (weight,
  smoothing), foot placement on the floor and on a step under the left foot (height slider), joint limits (knee hinges with a twist
  limit at the hip, the arm folds to 150 degrees), foot locking and toes (with "drifts side to side" to see the feet stay put), and a
  FABRIK tail to a green ball (sliders for its position). `--preset ik`, `ik-reach`, `ik-tail`, `ik-walk`, `ik-all`. What the panel does
  to the constraints is `sponza_walk/ik_rig.hpp`, driven by a unit test (`kin_skeleton_tests`); the panel's click path is untested here
  (no display).
- The "Physics (Jolt: kin/physics3d)" section turns on `WorldPhysicsState3D` over the viewer's ECS world: a floor at y = 0 (Sponza's own
  collision is not used: the viewer holds no triangles of the model) and boxes thrown from the camera (key B or the button: count, size,
  speed), stepped at 60 Hz, with the debug lines on or off (shapes green while awake and blue once asleep, contacts as red crosses with
  orange normals). `--preset physics` throws 30 boxes along the nave when the switch comes on. The panel's click path is untested here
  (no display).
- The "Character controller (kin/physics3d, P2c)" section puts the controller's test level (`tests/character_level.hpp`: ramps of 20, 35
  and 50 degrees, a hump, stairs of 15 cm, a 60 cm ledge, a low tunnel, boxes, a platform that moves and one that turns, a trigger) in
  front of the camera and a skinned person on it, an ECS entity with a `CharacterController3D`, a locomotion `Animator3D` (idle, walk and
  run blended by ground speed, a fall pose in the air) and the foot placer on the physics floor. WASD move it relative to the camera, Space
  jumps, Shift sprints, Ctrl crouches (the model is squashed: it has no crouch clip), R puts it back at the start; the camera follows
  behind. Sliders: walk speed, jump speed, steepest slope, highest step, camera distance, all live. The level is built on the physics
  floor only: Sponza's own walls are not collidable (the viewer holds no triangles of the model), so the level runs through them.
  `--preset controller` plays a scripted walk through the same command struct instead of the keys, which is what a headless run does
  (`--frames 400 --eye -14,2,0 --target 0,1,0`); the keyboard path itself is not exercised by any test here (no display).
- The "Climb (kin/physics3d, P2d)" section puts the climb level (`tests/climb_level.hpp`: a 4 m wall with a ledge, an overhang, a smooth
  wall with four holds and a lip, a platform that moves, a crate on a pedestal, a wall of ice, a ledge under a low roof) in front of the
  camera with the person on it: the same entity as the controller's, with traversal on, a two-bone arm per hand on the grips, the foot
  placer, and a stamina bar under the picture (the bar, like the stats, is drawn by a headless run only with `--hud`). The camera's forward is
  where the hands reach; the left and right mouse buttons (or Q and E) hold with the left and the right hand; WASD walk, or while it hangs
  shimmy along the surface and climb up it; Space jumps (jumps off while it hangs), F pulls up, X lets go, R puts it back. Sliders: reach,
  stamina, camera distance; the debug lines show the grips (yellow reaching, green held, orange slipping), the hang place and what a hand
  that is asked to hold and holds nothing can see. `--preset climb` plays a scripted climb of the first wall (both hands, 4 m up, a
  pull-up over the ledge) through the same command structs, which is what a headless run does (`--frames 420`); the keyboard and mouse
  path itself is not exercised by any test here (no display). It replaces the controller's level when both are on.
- The "Ragdoll and rope (kin/physics3d, P2e)" section: `rag` puts a row of people (`rag_count`, each an entity with a `Ragdoll3D` made
  from its skeleton at its rest pose) in front of the camera on the physics floor with the Sway clip. G knocks them down (the ragdolls
  start, blending in over `rag_blend`, an impulse of `rag_hit` at the chest) and, the next time, gets them up (each is put where its pelvis
  lies and the clip is blended back over a second); H throws a ball along the view, and what it touches starts that person's ragdoll with
  the ball's momentum at the bone. The debug lines are the bones' capsules (green awake, blue asleep). `rope` hangs a rope from a beam
  (`rope_length`) made 25 degrees out from the vertical, drawn as a tube, with a person under its end: Q and E (or the mouse buttons)
  take hold with the left and the right hand, W and S climb, Space jumps off; stamina is a slider. `--preset ragdoll` and `--preset rope`
  play both on a clock (the keys are not pressed by a headless run: `--frames 270` shows people lying, `--frames 420` the rope with the
  person on it); the keyboard path itself is not exercised by any test here (no display), and the rope is thin to see at the viewer's
  scale (the picture is of a person on a swing in Sponza's nave).
- `--set key=value` and `--preset name[,name]` set the same switches at start
  (`--list-settings`, `--help`); `--camera nave|roof`, `--eye X,Y,Z --target X,Y,Z`.
- Without a display: `KIN_RENDER_BACKEND=gpu sponza_walk --headless --frames 8
  --screenshot out.png --preset ao` (or `--preset contact`, `--preset taa`, with `--spin 0.4` for a moving camera and enough `--frames` to settle) renders from the start camera and writes the
  picture (`--hud` puts the panel in it, `--spin DEG` turns the camera a frame,
  `--print-settings` prints the settings). `ctest -R sponza-walk` does this and
  checks the picture is not blank; it skips without Sponza or a GPU.
- Ambient occlusion in a window of 1.5 million pixels or more (1920 x 1080 and up) runs over a linear depth chain (cheaper there, the same picture to
  0.2 levels); the stats line (F3) says "(depth chain)" when it did. `KIN_AO_CLASSIC=1` runs the old path, `KIN_AO_CHAIN=1` forces the new one, `KIN_CONTACT_CLASSIC=1`
  blurs the contact shadows twice. (Not tried with the panel's controls here: the click path cannot be exercised without a display.)
- It makes the pipelines and targets of every switch once when it starts (`Renderer3D::warm_up`), so that turning one on for the first time
  does not hitch, and again when a switch that needs it (map size, MSAA, HDR) is turned; the engine does nothing of the kind by itself.
- The code is `sponza_walk/settings.hpp` (all state, applied to the `Scene3D`
  each frame) and `sponza_walk/main.cpp`; a new feature is a field and one row
  in `Controls::build`.

## Run Observatory

A simulated LLM training tracker. Search experiments by name, model, or status;
click a row to inspect it; sort by loss; pause/resume the selected run or the
global telemetry stream. Scroll the run table with the mouse wheel. Space
toggles telemetry when the search field is not active.

The dashboard shows four summary cards, a virtualized table, three loss curves,
and bounded event logs. Only visible rows are drawn, while the complete run
dataset remains available for filtering and sorting. Telemetry updates at 4 Hz.
Default: 1,000 runs; `--stress`: 10,000; `--runs N`: up to 100,000.
This is simulated data, not a connection to real training infrastructure.

Display scaling is detected automatically and refreshed when resizing or moving
between monitors. Text is rasterized at the current pixel density, controls and
hit areas scale together, and charts share the available height with the logs.
Resize the window, or use `--width 960 --height 640`. The minimum content area is
640 x 480 density-independent units, so the pixel minimum grows with scaling.
An optional `--ui-scale 1.5` override is available for screenshots and testing;
normal use needs no scale or resolution configuration.
`--benchmark --scenario idle|live|scroll|churn` enables scripted workloads:

- `idle`: freeze telemetry and display the existing dataset.
- `live`: update active runs, histories, and logs.
- `scroll`: live telemetry plus continuous scripted table scrolling.
- `churn`: scrolling, search changes, sorting, and selection changes.

## Reproducible benchmarks

Run from the repo root. Reports and screenshots belong in ignored `out/`.
Headless mode automatically uses arena autoplay and tracker scripted scenarios.
`--benchmark` enables the same behavior in an ordinary visible window.

```sh
build/bin/signal_siege --benchmark --stress --profile --frames 1200 --seed 7 --report out/bench/arena-state.json --profile-json out/bench/arena-profile.json
build/bin/run_observatory --benchmark --stress --scenario churn --profile --frames 1200 --seed 7 --report out/bench/tracker-state.json --profile-json out/bench/tracker-profile.json
build/bin/kin_bench --suite examples --iterations 120 --warmup 5 --json out/bench/examples.json
```

Runtime `--profile` includes update, rendering, and presentation timing using
SDL's headless software renderer. A visible run is necessary to evaluate the
GPU backend and display pacing. `--vsync` requests synchronized presentation;
default presentation is unsynchronized. A profile run does not measure physical
key-to-screen latency; use the separate `input` suite for software input timing.

`kin_bench` includes:

| Case | One measured sample |
| --- | --- |
| `arena_1200_encounter` | Reset seed 7, 120 autoplay steps, collect and sort one view |
| `arena_5000_encounter` | Same encounter with 5,000 enemies |
| `arena_100000_encounter` | Same encounter with 100,000 enemies |
| `tracker_idle_10k` | Draw an idle 10,000-run dashboard through the null backend |
| `tracker_scroll_10k` | Advance telemetry and scrolling, then draw the dashboard |
| `tracker_churn_10k` | Change search/sort/selection, advance telemetry, then draw |

Arena numbers are **milliseconds per encounter**, not per frame. Tracker
numbers are CPU preparation/render-dispatch costs, excluding GPU presentation.
Live tracker cases evolve across samples; keep warmup and iteration counts
identical for comparisons, and use at least 120 iterations to cover telemetry
updates. Churn deliberately changes filters every benchmark sample, more often
than the interactive scripted scenario. Seeded run reports include checksums
for repeatability on the same build/platform. Floating-point simulation is not
promised to be bit-identical across architectures.

### Stress test

`--enemies 100000` packs the arena; the enemies converge on the player, so most
of them end up on screen. Run it on the GPU backend with the example's own
profiler zones (`--profile-lines`):

```sh
KIN_RENDER_BACKEND=gpu build/bin/signal_siege --benchmark --mute --enemies 100000 --frames 1200 --seed 7 --profile --profile-lines --profile-text=-
```

The zones split a frame: `example.arena.step` (with `.move`, the movement pass,
spread across worker threads from 16,384 enemies, and `.shots`),
`example.art.floor`, `.gather` (finding the enemies near the view, once),
`.lighting` (lights, drop shadows, the light map), `example.arena.propagate` and
`.collect_sprites` (the ECS sprite path), `example.art.enemies_flush`,
`.effects`, and `example.hud`, where the frame's sprite batch is first flushed.
The run report's `visible_enemies` counts the enemies drawn. Results match a
single-threaded run exactly.

## Capture and verify

```sh
build/bin/signal_siege --headless --stress --frames 360 --seed 7 --screenshot out/bench/arena.png --capture-frame 360
build/bin/run_observatory --headless --stress --frames 120 --seed 7 --screenshot out/bench/tracker.png --capture-frame 120
build/bin/run_observatory --headless --width 3840 --height 2160 --ui-scale 2 --frames 120 --screenshot out/bench/tracker-4k.png
cmake --build build --config Release --target kin_example_tests
ctest --test-dir build -C Release -R kin-examples --output-on-failure
```

Capture frame numbers count rendered frames. The runtime's first frame enters
the scene without updating it, so a run of N frames performs N-1 simulation
steps. Screenshots are captured before presentation. The tests cover seeded
replay, combat, bounds/capacity, filtering/sorting, pause behavior, virtualized
row counts, and mouse/text actions in a real headless UI without simulation.
Scale-change tests cover 100%, 125%, 150%, and 200%, including clicking and
scrolling. On a desktop session, `kin_example_tests --native-display` checks
automatic scaling and pointer mapping using the selected native renderer.
Use `kin_example_tests --native-game` for the game's native HUD and restored
aim-coordinate checks; it also accepts an optional screenshot path.
On X11, prefix that command with `SDL_VIDEO_X11_SCALING_FACTOR=2` to emulate a
200% display content scale; append a PNG path to capture the result.
When capturing headlessly without `--frames`, the app runs through the requested
capture frame automatically.
