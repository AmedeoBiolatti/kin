# Interactive performance examples

The experimental 3D [industrial yard](industrial_yard/README.md) adds a shared Kin/Godot Forward+ workload for independent
objects, PBR material variation, distributed motion, real skinning, transparency and overlapping spot shadows. Its serial
runner enforces RAM limits and measures a complete 1,440-frame interior/exterior route at 720p and 1440p, with feature controls.

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
- The "Sky and fog (P3a)" section: a procedural sky behind the model (seen through Sponza's open roof and its arches; `--camera roof`
  looks up), haze, the sun disc, stars and a moon; fog by distance and height with its colour taken from the sky and the sun's lobe;
  a time of day (hour, day of the year, latitude, play with a speed) that drives the sun, the moonlight, the ambient light, the sky's
  moon and the fog together; and "Sky lights the scene", which makes the ambient light and the reflections from the sky (the panorama
  is made again as the sun moves). `--preset sky`, `dusk`, `fog`, `night`, `day-night` (the last plays a day: `--frames 400`).
  A run with no display: `sponza_walk --headless --preset dusk --camera roof --screenshot dusk.png`.
- The "Spot lights and shadows (P3b)" section: spot lights dropped at the camera with J (they shine the way it looked; a small marker
  shows each), a flashlight carried with the camera (intensity, inner and outer angle, radius, shadow), a ring of spot lamps hung over
  the model's middle (count, intensity, angles, radius) and a ring of point lamps (cube shadows), the shadow maps' size, how many the
  most important of the lights get (spots up to 8, point lights up to 2), the bias, the normal offset and the filter's width, and the
  moon's shadow (the fill light's own cascades, faded with the moon's height under the time of day). The stats show the lights, the
  maps drawn and kept, and the memory. `--preset spots`, `flashlight`, `moon`, `lamps`. A run with no display:
  `sponza_walk --headless --preset flashlight --screenshot torch.png`. The panel's click path is untested here (no display).
- The "Particles and weather (P3c)" section: rain, snow, dust motes and mist as volumes round the camera (intensity sliders; wind speed,
  direction, gusts and turbulence), with the weather dimming the light and thickening the fog, rain splashing on the model's floor,
  soft particles (fading near surfaces: costs the depth pre-pass) and fires: `Fires in the model's middle`, or F drops one at the camera
  (each a flame, sparks and a flickering warm light). The stats show the sprites, batches and the CPU time. `--preset rain`, `snow`,
  `storm`, `fire`. A run with no display: `KIN_RENDER_BACKEND=gpu sponza_walk --headless --preset storm --screenshot storm.png`. Rain falls
  through Sponza's covered parts (the weather has no roof). The panel's click path is untested here (no display).
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

## Blockfield

A Minecraft-style **creative voxel sandbox** and continuous 3D workload (`KIN_EXPERIMENTAL_3D=ON`, SDL_GPU). It has seeded hills,
trees, underground passages, first-person walking/jumping and free flight, mining and placing six block types, selection outlines,
and optional saved world edits. It uses original procedural geometry and textures, with no downloaded assets. This first slice
does not implement survival, crafting, mobs, water simulation or multiplayer.

```sh
KIN_RENDER_BACKEND=gpu ./build/bin/blockfield --world /tmp/blockfield.world
KIN_RENDER_BACKEND=gpu ./build/bin/blockfield --benchmark --headless --frames 1200 --distance 3 \
    --report /tmp/blockfield-report.json --metrics /tmp/blockfield.csv --screenshot=/tmp/blockfield.png
```

Click to capture the mouse; Esc releases or captures it. WASD moves, Space jumps, Shift sprints, F switches flight, Space/Q flies
up/down, left mouse mines and right mouse places the selected block. Keys 1–6 select grass, dirt, stone, sand, wood or leaves;
`+`/`-` changes view distance, PgUp/PgDn changes camera FOV, R returns to spawn, F5 saves when `--world FILE` was supplied. Placement inside the player's body
is refused. Bedrock is protected. Saved edits reload with the same `--seed N`; the seed defaults to 1. Edits remain when their
chunks unload and are bounded to 16,384 changed cells. Saves contain terrain edits, not the player's location or physics state.

The world streams **16×64×16 block columns**. Greedy meshing removes internal faces and merges rectangles of equal material;
neighbor halos prevent duplicate faces at chunk seams. There is one meshing worker and one job in flight. GPU upload and Jolt
collision creation happen on the scene thread. Only the nearest 3×3 columns have colliders, built from the exact visible mesh;
walking pauses until these are ready. Edits invalidate the containing chunk and only the affected boundary neighbors. Distant
chunks and collision shapes are evicted as the player travels. Render distance is 1–32 columns in each horizontal direction
(default 8, at most 289 resident columns). Camera FOV is vertical, 45–150 degrees via `--fov N` (default 90).
Angles above 120 are available for stress testing; perspective distortion becomes pronounced.
The HUD and scene report show both settings. Wider angles reveal more of the resident world; increasing distance loads more columns.

`--mesh-budget-mib N` bounds resident vertex/index data (64 MiB by default, maximum 256). Near-player changes take priority over
distant scenery; rejected chunks apply backpressure instead of continuously rebuilding. This is **not a total RAM or VRAM cap**:
the renderer, physics scratch space, buffers being retired by the GPU and the single pending mesh consume additional memory.
The HUD and scene report expose the chunk limit, resident mesh bytes, blocked/dirty chunks, build/eviction counts and edit counts.

`--benchmark` renders **every frame**. After a distance-dependent warm-up, a fixed flight path crosses chunk boundaries, returns
over previously visited terrain and repeatedly changes blocks. The JSON report contains CPU update, scene preparation, render
submission and available GPU frame p50/p95/p99/max, plus drawn triangles and draw calls. GPU values come from the engine's delayed
device timer; zero samples means timing was unavailable. Headless mode waits for its single meshing job at fixed points to make
the workload reproducible, so CPU update times there include those waits. Windowed play polls the worker instead. CSV rows include
startup as well as steady-state frames; aggregate timing samples exclude warm-up and are bounded to 6,000 samples per series.
Change one workload setting at a time and run sequentially. Larger distances may hit the mesh cap before filling the requested view.

`kin-blockfield` checks greedy merging, winding, hidden faces and chunk seams, negative coordinates, picking, bounded edits,
save/restore and actual Jolt collision rebuilding after mining. The existing scene server supports input-driven tests, screenshots,
reports and resetting the world. All voxel code is in `examples/blockfield/`; no engine API change is required for this slice.

The input-driven GPU smoke test also exercises jumping/landing, mining, placement, player overlap rejection, block selection,
F5 save/reload and streaming eviction. It uses a fresh save file and writes screenshots and its final state to the supplied directory:

```sh
ctest --test-dir build -R '^kin-blockfield$' --output-on-failure -j1
python3 examples/blockfield/smoke.py build/bin/blockfield /tmp/blockfield-smoke
```

Without explicit `--frames`, a benchmark chooses at least 900 measured frames beyond warm-up. For explicit frame limits, warm-up
is `max(180, (2*distance+1)^2+60)` frames: distance 32 needs at least 5,185 total frames for that measured interval.
For a bounded wide-view run:

```sh
KIN_RENDER_BACKEND=gpu ./build/bin/blockfield --benchmark --headless --distance 24 --fov 110 --mesh-budget-mib 256 \
    --report /tmp/blockfield-wide.json --metrics /tmp/blockfield-wide.csv --screenshot=/tmp/blockfield-wide.png
```

A benchmark tests the chosen workload, rather than certifying the engine's capacity: greedy voxel terrain is relatively cheap to draw and
does not exercise animation, complex material stacks or large particle systems.

For CPU profiling, add `--profile --profile-json /tmp/blockfield-profile.json` to an explicitly bounded benchmark. The runtime
JSON records per-frame CPU scopes, presentation and available GPU samples; filter its `samples` by `frame > warmup_frames` from
the scene report to exclude loading. Raw profile events grow with the frame count, so keep runs bounded and sequential. CPU scopes
can nest and GPU work overlaps the CPU; their times are not additive. The CPU profiling section of `docs/3d_plan.md` records the
distance-32 findings and a `perf` sampling command, with shared geometry buffers as the first measured optimisation target.

Kin now shares GPU buffers for small static meshes, including these chunks. To compare the original allocation path, set
`KIN_SHARED_MESH_BUFFERS=0` before launching; the choice applies when each mesh is created. The report's `shared_mesh_*` fields
show reserved GPU page bytes, live/retired ranges, page counts and reuse. Pool reservation includes page padding, so it can exceed
the sample's resident mesh bytes. Dedicated meshes, view targets and driver allocations are outside these pool counters.

For a comparison with Godot, `blockfield_render_compare` generates a versioned binary snapshot using the existing seeded
terrain and greedy mesher, then loads one chunk at a time into Kin. The Godot project in `blockfield/godot/` reads the same file,
reversing index winding for Godot's clockwise front faces. Both draw one mesh per chunk, opaque unlit vertex colours, the same
frame-indexed flight path, vertical FOV, near/far planes and 1280×720 offscreen target, followed by a full-size 2D copy. Shadows,
sky, fog, AA, LOD, occlusion culling, HUD, physics, terrain edits and streaming are absent. Engine frustum culling stays enabled:
Kin uses bounding spheres and Godot uses tighter bounds, so submitted draw/triangle counts can differ despite matching images.
This compares rendering a common scene; it does not compare complete voxel games or the existing streaming benchmark's timings.

Keep the standard Godot 4.7.2 Linux editor and `linux_release.x86_64` export template in
`$HOME/kin-bench/godot-4.7.2/`. Only that template is needed; discard the full template download after extracting it. The runner
copies the small project onto the Linux disk, imports it without opening the editor UI, exports a release build using the custom
template, and hard-links identical executable copies to save disk space. Generated geometry stays outside the project/export pack.
The tested editor/template pair is from the [official builds](https://github.com/godotengine/godot-builds/releases/tag/4.7.2-stable).

```sh
cmake --build build --target blockfield_render_compare --parallel 1
python3 examples/blockfield/run_render_compare.py --kin "$PWD/build/bin/blockfield_render_compare" --smoke
python3 examples/blockfield/run_render_compare.py --kin "$PWD/build/bin/blockfield_render_compare"
```

The Linux runner uses only the Python standard library and `/usr/bin/time`. It runs one process at a time, refuses to start below
2 GiB available RAM or 2 GiB free disk, and terminates a run above 1.5 GiB RSS, below 1 GiB available RAM, or after 300 seconds.
Snapshot generation holds only one chunk on the CPU and caps source mesh data at 256 MiB. Each renderer loads chunk arrays
incrementally and periodically submits uploads. Defaults are three alternating repeats of distance 24/FOV 150 and distance
32/FOV 110 and 150, each with 300 warm-up and 900 measured frames. `--frames`, `--repeats`, `--base` and `--godot-dir` are adjustable.
Results, logs, screenshots, snapshot SHA-256 hashes and peak process RSS go under `$HOME/kin-bench/results/` (or `smoke/`).
Avoid a RAM-backed `/tmp` for these snapshots and Godot's shader caches.

Use `frame_interval_ms` for a common wall-clock render-loop interval, including submission, queue backpressure and engine work;
it is an uncapped offscreen workload, not presented gameplay FPS. Kin's `kin_fence_ms` is a host fence-completion timer, whereas
Godot's `godot_gpu_timestamp_ms` uses GPU timestamps for the 3D viewport. They have different timing boundaries and must not be
compared as equivalent GPU frame times. Godot's CPU setup/viewport measurements also have different boundaries from Kin's
scene-build/submit measurements. Godot `--headless` disables GPU rendering, so only import/export use that flag; actual benchmark
runs use Vulkan with `RenderingServer.force_draw(false)` and automatic drawing disabled, avoiding display refresh pacing.

On 2026-10-14, three alternating repeats on an RTX 4080 Laptop GPU (driver 580.178.04), Kin Release and Godot 4.7.2 release
produced the following at distance 32 / vertical FOV 150 (about 1.49 million submitted triangles in Kin, 1.48 million in Godot):

| Renderer | Median loop interval | 95th percentile | Peak process RSS |
| --- | ---: | ---: | ---: |
| Kin SDL_GPU Vulkan | 0.83 ms | 0.87 ms | 200 MiB |
| Godot Mobile Vulkan | 0.49 ms | 0.63 ms | 541 MiB |
| Godot Forward+ Vulkan | 0.73 ms | 0.90 ms | 574 MiB |

All 27 large runs completed; all six screenshot pairs had identical foreground masks, and Forward+ matched over 99.999% of
pixels. Godot import/export peaked at 703 MiB RSS. The retained binaries, snapshots and results occupied about 767 MiB under
the benchmark directory, plus about 14 MiB of Godot caches/user data. These short, uncapped trials are a rendering baseline;
CPU/GPU clocks were not locked. Mobile is faster on this simple material workload, while Kin uses much less process RAM and
has comparable timings to Forward+. Lighting, shadows and streaming still need separately matched workloads before drawing
broader conclusions about engine capacity. Full local measurements and machine/build metadata are in
`$HOME/kin-bench/results/comparison.json`.

To stress triangle throughput while keeping geometry storage bounded, use a multiplier sweep:

```sh
python3 examples/blockfield/run_render_compare.py --kin "$PWD/build/bin/blockfield_render_compare" --triangles --smoke
python3 examples/blockfield/run_render_compare.py --kin "$PWD/build/bin/blockfield_render_compare" --triangles
```

This reuses the distance-32 snapshot after validating its SHA-256. A new top-down view frames the entire field at vertical FOV
90, with no frustum rejection in Kin; all Godot instance bounds fit the camera. Both engines submit all 3,195,550 source triangles
at 1×. Defaults test 1×, 5×, 10×, 20× and 100× (3.20, 15.98, 31.96, 63.91 and 319.56 million submitted triangles per frame).
Copies form disjoint tiles in a grid with `ceil(sqrt(copies))` cells per side; uniform scaling keeps the complete tiled field
inside the same footprint and camera frame. Copies are not coincident overlapping geometry.

Kin uses `Scene3D::draw_instanced`, and Godot uses one `MultiMesh` per source chunk. Vertex/index buffers remain shared;
each engine issues 3,504 instanced main-pass draws at every multiplier, and instance transforms scale with the copy count.
Every measured frame is checked against the exact expected triangle count. This tests geometry throughput and instancing,
including Kin's per-frame instance uploads, with fixed draw-call count and simple opaque materials. It does not multiply unique
mesh storage, independent scene-object count, shader complexity, texture memory, or pixel count. At higher multipliers, triangles
occupy fewer pixels and many are smaller than a pixel; the counters describe submitted triangles rather than shaded pixels.

The stress sweep uses 300 warm-up plus 300 measured frames per run, three alternating repeats, the same memory/time limits,
and separate outputs under `$HOME/kin-bench/triangle-sweep/`. Small smoke runs exercise 1×, 5× and 100× on 25 source chunks.
Use `--scales 1 5 10 20 100`, `--frames N` and `--repeats N` to change the sweep. Because the complete field is now submitted,
its 1× baseline differs from the earlier moving-camera/frustum-culling comparison; compare multipliers within this sweep.
The triangle counter describes main-pass submissions. Godot Forward+ retains its default depth prepass, which draws geometry
again; Mobile has no depth prepass, and Kin's optional depth-prepass features are disabled here. These are the tested renderer
defaults, not identical internal pass counts. See Godot's
[depth-prepass setting](https://docs.godotengine.org/en/stable/classes/class_projectsettings.html#class-projectsettings-property-rendering-driver-depth-prepass-enable).

Shader probes on the same machine (2026-10-14) isolate the unlit workload's GPU overhead. Each probe keeps the original mesh
buffers, 128-byte instance records, CPU renderer, transforms and draw counts; only embedded `mesh.vert.spv`/`mesh.frag.spv`
change. Temporary probe executables live under `$HOME/kin-bench/shader-analysis/`; the default shaders and benchmark executable
were restored byte-for-byte after creating the probes. Three alternating repeats produced these median render-loop intervals:

| Shader path | 20× / 63.91 million triangles | 100× / 319.56 million triangles |
| --- | ---: | ---: |
| Kin general mesh shader | 13.30 ms | 65.63 ms |
| General shader with `glslc -O` | 13.25 ms | — |
| Remove unused vertex work, retain general fragment shader | 10.47 ms | — |
| Minimal unlit, untextured vertex/fragment probe | 9.49 ms | 46.97 ms |
| Godot Mobile reference | 9.91 ms | 49.04 ms |

At 20× each run used 300 warm-up and 180 measured frames; at 100× it used 300 warm-up and 120 measured frames. All native probe
screenshots match the general shader's pixels exactly. The unlit probe reduces loop time by about 29% at both scales, reaching
roughly Mobile's performance; GPU clocks were not locked, so its small apparent lead is not a general engine ranking.

Kin's general vertex shader transforms normals and exports normal/world-position/UV data even when the material is unlit.
The fragment shader skips its lighting branch at runtime for these draws, but still samples a default white base texture.
The probes demonstrate the value of dedicated unlit variants and shader input masks. Godot's Mobile shader has compile-time
`NORMAL_USED`, `UV_USED` and `MODE_UNSHADED` feature branches. Godot also uses packed normals/separate vertex streams and
48-byte MultiMesh transforms in this scene; Kin uses 36-byte interleaved vertices and 128-byte instance records. Packing/splitting
vertex data and retained instance buffers are additional opportunities, whose separate performance gains have not been measured.
At 100×, Kin prepares/uploads about 42.8 MiB of instance records each frame; Godot retains the unchanged copy transforms.

These probes implement only this scene's unlit, untextured material. A production path needs correct textured/alpha/emissive
behaviour and appropriate fog/skinning/velocity/custom-shader fallbacks. The prototype is not a global replacement for the
general mesh shaders. `glslc -O` makes the SPIR-V smaller but changes this timing by only 0.4%, within run-to-run variation;
static SPIR-V instruction counts also include unused or runtime-skipped branches and are not GPU machine-code counts.
Full measurements, probe hashes, source observations and pixel checks are in `$HOME/kin-bench/shader-analysis/investigation.json`.

The production implementation now uses dedicated unlit colour-only, textured and masked shaders in Kin, enabled by default.
Set `KIN_UNLIT_SHADERS=0` before renderer creation for a same-binary reference. Fog, skinning, temporal AA and custom shaders
use the general path; emission, blending, HDR and MSAA are supported by the specialization. New paired runs reduced frame time
by about 27.5% at 20× and 29.8% at 100× (median paired p50 reductions), with exact Kin screenshot matches. Median run p50s were
10.99→7.97 ms at 20× and 58.99→40.40 ms at 100×; Mobile measured 8.13 / 41.28 ms. Clocks drifted during the longer sweep, so
these results support roughly Mobile-level performance for this unlit scene. Kin's peak process RSS stayed below 423 MiB;
shared vertex/index data and the 36/128-byte vertex/instance layouts are preserved. Full details and reproduction are in
`$HOME/kin-bench/unlit-specialization/findings.json` and `docs/3d_plan.md`.

### Lighting, materials and sampling

Blockfield now has playable quality presets:

```sh
KIN_RENDER_BACKEND=gpu ./build/bin/blockfield --quality pbr
KIN_RENDER_BACKEND=gpu ./build/bin/blockfield --quality high --distance 8 --mesh-budget-mib 64
```

`basic` retains the existing daylight/sky/fog rendering. `pbr` adds shared albedo/ORM/normal textures, four point lights, one spot,
HDR and 4× MSAA. `high` uses eight points, two spots, requests shadows from one point and two spots, TAA, half-resolution ambient occlusion
and 16-step contact shadows. The coloured lamps follow the player; terrain remains a dielectric material and daylight/sky
reflections remain active. The mesh budget and streaming/collision limits apply as before. Reports identify the quality preset
and actual light, shadow and AA work. `smoke.py --quality pbr|high` exercises gameplay and checks those GPU counters.

For separately measured Kin/Godot workloads:

```sh
python3 examples/blockfield/run_render_compare.py --kin "$PWD/build/bin/blockfield_render_compare" --lighting --smoke
python3 examples/blockfield/run_render_compare.py --kin "$PWD/build/bin/blockfield_render_compare" --lighting --frames 180 --repeats 3
```

The default uses the existing distance-32 snapshot, 3.20 million main-pass triangles, 3,504 draws and one tiled copy. Steps add
HDR, lit materials, three metallic/roughness families, albedo/ORM/normal/emissive maps, image-based illumination, eight moving
points and two moving spots, sun shadows, local shadows, then separately 4× MSAA, 2× render resolution and TAA. A final Kin
case adds half-resolution AO and contact shadows to TAA. Material maps occupy 64 KiB; the generated 128×64 linear RGB panorama
uses 96 KiB before renderer filtering. Geometry stays shared. The 2× resolution case renders 2560×1440 and downsamples to the
same 1280×720 output. These are real-time raster sampling options.

Godot Mobile TAA and the unmatched Godot contact-shadow case are recorded as unsupported in `unsupported.json`. Mobile light
counts above eight of either type are rejected. The renderers use their own BRDFs, normal mapping, environment filters, shadow
atlases/filtering and tonemapping; compare workload cost and inspect quality, rather than assuming identical pixels. In particular,
local shadow maps have different layouts and the non-IBL ambient lighting differs. Fine procedural detail intentionally exposes
aliasing, and texture maps have linear filtering without mipmaps in this benchmark. MSAA mainly addresses geometry edges;
TAA and the resolution test also expose surface-detail sampling cost.

Individual Kin runs accept `--material unlit|lit|pbr`, `--lights 0..16`, `--spots 0..16`, `--samples 1|2|4|8`,
`--resolution-scale 1|2`, `--shadow-resolution 512|1024|2048`, and `--hdr`, `--environment`, `--normal-map`, `--shadows`,
`--local-shadows`, `--taa`, `--ao`, `--contact` with values `0|1`. Choose TAA or MSAA; requests that cannot run are rejected.
Use `--copies 1 --camera detail` for a close inspection view (its frustum culling means the whole-field triangle assertion does
not apply). CSV/report counters confirm the requested lights, samples, shadow passes and compute dispatches actually ran.
Results and peak RSS go under `$HOME/kin-bench/lighting-materials/`; the sequential memory guards still apply.

Kin now packs shadow casters into flat lists and addresses small meshes within already-bound GPU pages. Both changes are
engine-wide and preserve the same shading and geometry. `KIN_MESH_PAGE_BINDINGS=0` disables page-addressed draws for diagnosis
while retaining shared storage and the flat shadow gather. Paired before/after measurements and exact capture comparisons
are recorded in `docs/3d_plan.md` and `$HOME/kin-bench/shadow-binding-opt/`.
The subsequent 90-run Kin/Godot rerun is saved separately in `$HOME/kin-bench/lighting-after-cpu-opt/`. At 3.20 million main
triangles, local-shadow frame medians are 3.09 ms (Kin), 6.03 ms (Mobile), 7.36 ms (Forward+); with MSAA 4× they are
4.35 / 5.72 / 6.97 ms. These compare this draw-heavy workload with each renderer's own lighting/shadow implementation.



Kin reuses renderer draw/task layout for unchanged scene commands; `KIN_PREPARED_SCENES=0` restores rebuilding that layout.
The Grove harness already retains its scene, so this targets engine preparation rather than the sample's command builder.
`--rebuild-scene` deliberately invalidates it every frame. CSV/report fields `prepared_reused`, `prepared_draws`,
`prepared_cache_bytes` and `prepared_layout_bytes` distinguish the small new key/stamps from existing reused arrays.
Visibility, lighting and shadow work still update each frame. See the prepared-layout measurements in `docs/3d_plan.md`.
Ordinary fixed-size batch edits now reuse that layout too; `KIN_PREPARED_BATCH_EDITS=0` restores invalidation on content
edits. Group validity changes still rebuild the layout. GPU uploads, bounds and camera/light selection always refresh.

For a shared spot-shadow path, `--point-shadows 0` keeps point illumination but disables its shadows. With
`--shadows 0 --local-shadows 1 --lights 4 --spots 1`, both engines render one perspective spot map. Godot uses
one slot per quadrant in a smaller atlas with 32-bit depth; its slot width still matches `--shadow-resolution`.
Reports expose point/spot shadow requests, spot projection/nominal cone FOV/range, near plane and depth bits. Kin's benchmark-only
`--shadow-near 0.025` matches Godot's fixed local-shadow near plane (Kin's default remains 0.05). Filtering and bias
remain engine-specific. The lighting sweep includes `spot-shadow-only`; matching near planes requires this extra Kin option.

Godot reports actual shadow draw calls and triangles, alongside the requested light settings. Its positional atlas uses
four slots per quadrant: an omni needs two adjacent slots, so one-slot quadrants silently omit point shadows. The lighting
sweep includes a point-shadow-only case (also in `--lighting --smoke`) and rejects requested shadows that produce no work.
`--shadow-resolution N` sets the atlas slot width; Godot renders point cube faces at `N/2` before its dual-paraboloid
conversion, while Kin renders perspective cube faces at `N`. Projection, filtering and culling differ between engines;
matching recipe settings does not establish identical shadow quality. Earlier Godot local-shadow results used the invalid
one-slot layout and must not be treated as full-feature comparisons. See the dual-engine profile in `docs/3d_plan.md`.

### Ground-level grove stress scene

The rendering comparison also has a frame-indexed, 24-second ground tour through the seeded terrain. A `grove` preset adds
10,000 alpha-cutout plants, 512 layered transparent mist cards, PBR/IBL, HDR, four points / one spot, sun and local shadows.
Plants share one crossed-card mesh and a 128×128 texture; mist shares one quad and a 64×64 texture. Extra textures total
80 KiB. Placement, camera poses, geometry and texture bytes are baked once into a 3.32 MiB `.bfd` recipe used by both engines.
The recipe streams requested prefixes rather than loading the maximum population; terrain continues to load one chunk at a time.

Generate the recipe and preview the tour (60 FPS cap, Escape or window close exits):

```sh
./build/bin/blockfield_render_compare --generate-dressing "$HOME/kin-bench/blockfield-grove.bfd"
KIN_RENDER_BACKEND=gpu ./build/bin/blockfield_render_compare --preset grove --preview \
  --snapshot "$HOME/kin-bench/blockfield-d32.bfc" --dressing "$HOME/kin-bench/blockfield-grove.bfd" \
  --metrics "$HOME/kin-bench/grove/preview.csv"
```

Use `--foliage 0..100000` and `--transparent 0..20000` to vary the populations independently. Explicit flags override preset
values regardless of argument order. `--camera ground --dressing FILE` selects only the route; `--preset grove --foliage 0
--transparent 0` retains the preset lighting with both added populations disabled. Ground mode requires one terrain copy.
The route follows a clear lane and is baked above existing terrain/tree bounds. These are instanced plants and static
transparent cards; animation/crowds, physics, streaming and destruction remain separate workloads.

```sh
python3 examples/blockfield/run_render_compare.py --kin "$PWD/build/bin/blockfield_render_compare" --grove --smoke
python3 examples/blockfield/run_render_compare.py --kin "$PWD/build/bin/blockfield_render_compare" --grove --frames 300 --repeats 3
```

The full sweep tests the bare route, 10k/50k/100k plants, 512/5k transparent cards, the default combination and a 100k/5k
combination. Results are in `$HOME/kin-bench/grove/` (`smoke-grove/` for the smoke run). A separate Godot export preserves the
previous lighting package. Runs stay sequential under RAM guards; the terrain snapshot is reused. CSV includes requested
populations, camera frame, actual draw/triangle counts, frame p50/p95/p99 summaries and memory metadata. Native CSV also has
actual visible instance counts. Preview timings include its frame cap and are not benchmark results.

Ground scenes enable each engine's normal frustum culling. Main-pass submitted triangle counts can therefore differ across
renderers, unlike the uncropped full-field triangle test. Godot uses 64 spatial plant batches / eight mist batches, while Kin
merges consecutive identical-mesh/material instance draws while preserving cell bounds for CPU culling. Reports retain actual work rather than asserting equal triangle
submission. Lighting/shadow implementations differ as in the lighting sweep; no cross-engine pixel-equivalence claim is made.

`KIN_MASKED_DEPTH_PREPASS=1` opts into the stock masked-material main-depth experiment (off by default). The dense Grove
route then prepasses the visible plant copies into the existing main depth target and shades against it without rewriting
depth. It does not change camera/shadow selection or allocate another depth texture. CSV/report fields
`masked_depth_draws` and `masked_depth_instances` report the extra pass separately; other draw/caster counts keep their
meaning. TAA/MSAA/overlay/GPU-occlusion/custom-shader views and unsupported draws keep their previous paths. This can cost
CPU time, so measure it with the intended helper count; the recorded 100k four-helper gain does not apply to serial runs.

For external GPU captures, `--visible` uses an uncapped visible window with the same 300-frame warm-up, frame-indexed
camera/light route and exact `--measured` CSV count. It exits from the benchmark's own render count. Unlike `--preview`,
it does not skip warm-up or impose a 60 FPS cap. Presentation gives Nsight Systems frame boundaries: hidden SDL windows
can otherwise exhaust its per-frame GPU timestamp limit. Use hidden runs for the established throughput comparison;
visible capture intervals include window/compositor and profiler overhead.

`KIN_GPU_PASS_MARKERS=1` enables optional `kin3d.*` SDL debug groups without splitting submissions. For example, with
Nsight Systems already installed (output/scratch on disk, not a RAM filesystem):

```sh
KIN_RENDER_BACKEND=gpu KIN_GPU_PASS_MARKERS=1 TMPDIR="$HOME/kin-bench/tmp" \
nsys profile --trace=vulkan-annotations --sample=none --cpuctxsw=none --backtrace=none \
  --vulkan-gpu-workload=individual --output="$HOME/kin-bench/grove-capture" \
  ./build/bin/blockfield_render_compare --visible --preset grove --workers 4 \
  --snapshot "$HOME/kin-bench/blockfield-d32.bfc" --dressing "$HOME/kin-bench/blockfield-grove.bfd" \
  --foliage 100000 --transparent 0 --measured 1440 --profile-cpu \
  --metrics "$HOME/kin-bench/grove-capture.csv" --report "$HOME/kin-bench/grove-capture.json"
```

Create the disk scratch directory first. The bounded capture scripts in the GPU-profile result archive additionally run
the application under its own 512 MiB RSS / low-free-memory guard: Nsight session processes can be reparented, so the
outer profiler's process-tree peak is not the game's peak. Export the `.nsys-rep` to SQLite with `nsys export --type=sqlite`.
Match `VULKAN_DEBUG_API` label ordinals to native frame indices, join GPU markers by correlation ID, exclude loading/warm-up
and missing buffered tail samples, and use the enclosing `VULKAN_WORKLOAD` submission to account for dependent completion.
Do not add its duration to contained marker times. Results and controls are recorded in `docs/3d_plan.md` under
“Grove — external GPU pass profile”.

For Kin CPU diagnosis, `--profile-cpu` adds Linux caller-thread and process CPU clocks around `render()`, the caller's scene
submission CPU time, and a monotonic timestamp to each CSV row. These columns are zero when disabled. Process CPU includes
renderer/driver helper threads and can exceed elapsed time; the clocks are not GPU timestamps or additive frame phases.
`--workers 0..4` attaches that many renderer helpers (the caller also works); default zero preserves the serial benchmark.
This is an experiment control; choose a worker policy from measured workload costs. Shadow jobs now group small draws
into batches instead of scheduling each draw separately; the documented profile retains the earlier overhead measurements.
`KIN_SPATIAL_CULLING=0` before launch disables cell culling for a same-binary comparison. The grove caches conservative cell
bounds at load; its GPU draw batching and geometry stay unchanged. CSV also reports active cells, refined
camera/bounds/local-shadow copies and group-rejected candidates. Grove submits foliage using `InstanceBatch3D` shared CPU
handles and retains its GPU records within the renderer's 32 MiB / 128-batch live-table cap. `KIN_RETAINED_INSTANCES=0`
disables GPU retention; additionally set `KIN_GROVE_RETAINED=0` to use grouped streamed CPU records, and
`--rebuild-scene` to restore per-frame CPU submission. CSV/report fields
`retained_instances`, `retained_gpu_bytes`, `retained_upload_bytes`, `retained_index_bytes` and `instance_upload_bytes`
show the work removed. See `docs/rendering_3d.md` for edit semantics, fallbacks and what the memory cap counts.
`KIN_CACHED_INSTANCE_BOUNDS=0` keeps the per-render sphere transforms for a same-binary CPU-cache comparison, with GPU
retention still enabled. The CPU sphere cache is capped at 4 MiB and replaces the retained batches' transient sphere ranges.
`bounds_cache_bytes`, `bounds_scratch_bytes`, `bounds_cache_instances` and `bounds_cache_rebuilt` report its storage/work;
`bounds_instance_tests` includes actual cold/edited cache rebuilds.

Shared `--moving-instances N` animates a contiguous range of ground-route foliage using deterministic frame indices.
It requires `N <= --foliage` (and native `KIN_GROVE_RETAINED` enabled); zero keeps the original static scene. The range starts
near the middle of the grove. Kin keeps animation copies only for those records (two 72-byte records per moving instance).
Godot bulk-writes the affected MultiMesh buffers and records `motion_update_ms`, including its GDScript loop/API overhead.
Reports include `moving_instances`, `moving_first`, initial/current position sums and `foliage_batch_count`. Set
`KIN_INCREMENTAL_BOUNDS=0 KIN_PARALLEL_INSTANCE_PACKING=0` for the previous full-sphere-refresh/caller-only packing path.
The default refreshes edited spheres and packs large GPU tables on attached workers, keeping full GPU uploads for safe
buffer cycling. Prepared command layouts survive ordinary batch edits; use `KIN_PREPARED_BATCH_EDITS=0` to compare the old
invalidation behavior. Timings and limitations are in `docs/3d_plan.md`.
`KIN_CACHED_CAMERA_CULL=0 KIN_ACTIVE_CULL_COUNTERS=0` keeps the previous general camera classifier and full LOD/cascade
counter clearing. The default uses existing cached spheres for camera-only decisions and clears active counter prefixes;
these CPU changes add no per-copy storage or GPU buffer and preserve local-shadow selection.

Godot normally has one foliage MultiMesh per spatial cell (64 at 100k plants); Kin normally has one retained table with
64 cell bounds. `KIN_GROVE_CELL_BATCHES=1` before native launch instead creates one retained batch per cell, preserving the
source order and visibility bounds. Use this control to distinguish upload granularity from renderer differences. For the
1,000-record motion range in the 100k recipe, two cells change: Kin's cell control uploads 399,872 bytes/frame versus
12,800,000 in its normal layout; Godot submits 149,952 bytes of transform buffers (48-byte records versus Kin's 128).
Native layouts must match images, triangles and camera/light selection; draw counts differ. The Forward+ dynamic comparison
and its checks are recorded in `docs/3d_plan.md`.

`KIN_LOCAL_SHADOW_CULLING=0` restores the previous local-shadow gather for comparison. Fully contained single-level ranges
now skip per-copy classification; partial cells retain only unresolved frustum planes and skip proven radius tests.
`local_candidates`, `local_group_accepted`, `local_radius_tests`, `local_plane_budget` and `local_bucket_bytes` expose the
work/memory change. The plane budget is an upper bound before per-copy early exits; `local_instance_tests` excludes fully
accepted ranges.
`KIN_LOCAL_SHADOW_PACKING=0` separately restores full task counter arrays, duplicate accepted/upload copy lists and slot
lookup storage covering retained copies too. `local_task_bytes`, `local_slot_bytes` and `local_copy_bytes` report the
corresponding CPU vector payload capacities; they do not represent total RSS or GPU memory.
`KIN_SHADOW_REACH=0` restores eager sun/moon swept-sphere reach tests. The default prepares ray exit faces once,
computes endpoints lazily, reuses them across sun cascades, and skips camera/slice planes proven safe for a spatial cell.
Ordinary single-map draws keep the original main loop; grouped/cascaded sun draws use the prepared loop.
Outside cells still cast into view; compact local tasks retain their 32-byte size. Eight extra bytes per cell replace
repeated work without adding a per-instance cache. CPU cluster cuts use full reach frustums.
The comparison harness retains its static geometry/material commands in `Scene3D`, updating the camera and animated
light rig each frame, matching the persistent Godot scene's lifetime. This uses the existing API and the same vector
capacities, rather than a new engine cache. `--rebuild-scene` restores clearing and resubmitting all geometry each frame;
reports record `static_scene_commands`. Keep that setting consistent when comparing engine-only changes.
`KIN_CAMERA_RANGES=0` restores per-copy classification/packing inside camera-visible cells. The default fills accepted
single-level ranges together when no cascade/LOD/skin or uncached local bounds require refinement, and packs fully
visible pieces without per-copy visibility branches. CSV `camera_range_accepted` counts classified copies, not packing
alone; no new per-instance cache or task payload is allocated.
`KIN_LOCAL_SHADOW_SIMD=0` selects scalar local-shadow candidate tests. Supported SSE2 builds test four existing bounds
records together for ranges of at least 16 candidates, retain scalar tails/small draws and preserve accepted-copy order.
No new per-instance storage or larger task records are needed; unsupported/contraction-enabled builds use scalar tests.
`KIN_ADAPTIVE_STAGING=0` restores the GPU device's fixed 16 MiB shared-upload floor. The default can shrink after quiet
120-batch windows, down to 1 MiB, and regrows for large uploads. CSV fields `upload_staging_bytes`,
`upload_staging_allocations`, `upload_count` and `upload_batches` distinguish capacity, allocations and upload traffic;
capacity excludes SDL cycle backings and driver pools.

```sh
KIN_RENDER_BACKEND=gpu taskset -c 0-15 ./build/bin/blockfield_render_compare --preset grove \
  --snapshot "$HOME/kin-bench/blockfield-d32.bfc" --dressing "$HOME/kin-bench/blockfield-grove.bfd" \
  --foliage 100000 --transparent 0 --copies 1 --measured 180 --profile-cpu --workers 2 \
  --metrics "$HOME/kin-bench/grove-profile/manual.csv" --report "$HOME/kin-bench/grove-profile/manual.json"
```

Choose CPU affinity for the actual machine; 0–15 are this test laptop's P-core logical CPUs. The measured CPU bottlenecks,
worker trade-offs, upload counts and ranked engine proposals are recorded in `docs/3d_plan.md` under “Grove — CPU profile and
proposed engine work”.

## Projectile range

Build `projectile_range` with experimental 3D enabled. A barrel enemy fires small rocks
at a crate target. Three contact hits destroy the target; it respawns after a short
delay. Missed projectiles expire. An Update C++ ECS system handles firing/respawning;
a PostPhysics system handles fresh contact damage, lifetime and removal through the
existing command buffer and scene-generation endpoints. New shots participate in the
same physics step, and hit objects disappear before animation/collection.
The host only steps/renders and draws counters. R restarts; Escape exits.

```sh
cmake --build build --target projectile_range -j1
build/bin/projectile_range
build/bin/projectile_range --headless --frames=900 --report=range.json --screenshot=range.png
```

The example reuses Summit's free models/textures, explicitly preloads both object
prefabs during level loading and allocates a 16-slot projectile pool. It resets Projectile
state on every lease and returns shots through deferred despawn. Old checked handles
stay stale when an ECS slot is reused. Pending lifecycle requests are capped at 24;
physics uses a 1 MiB single-thread scratch pool. Idle slots keep shared art/ECS storage,
with native bodies removed until their next acquisition. See
[preloading and pools](../docs/rendering_3d.md#preloaded-prefabs-and-bounded-pools).

## Camp walk

Camp shares bounded targeting, current reach/visibility checks and checked
contact targets through `InteractionQuery3D` (`kin/scene/interaction3d.hpp`).
Native Collect/Push verb IDs map to keyboard/gamepad prompts; inventory changes
and crate impulses remain game code. Pickup selection and direct crate targets
use the same validation API without an editor or additional scheduler.

**E / pad X** starts a one-shot pickup reach over locomotion. Supplies are awarded
at the contact marker after fresh reach/visibility/checked-handle validation and
successful scene removal. Jumping or losing the target cancels the pickup.
Definitions, masks and marker ownership live in `survivor.kinanimator`; the compact
Pickup clip is generated by `tools/assets/add_summit_pickup.py`, preserving all
original model resources. Actions use one bounded slot per character.

Walk/Run markers in `survivor.kinanimator` trigger grounded footsteps and small
dust bursts during fixed steps. The event handler shares `kin/anim` dispatch;
16 events per animator per step, one short synthesized mono clip and eight audio
voices keep memory bounded. No sound files or editor are required.

Build `camp_walk` with experimental 3D enabled. This small playable scene uses the
shared scene/prefab/material/animation/physics pipeline and Summit's bundled free assets.
The C++ host loads the level, samples commands, steps/renders and draws UI. Player
movement, interaction and collection live in `camp_walk/gameplay.hpp` ECS systems;
the level and controller settings live in `summit/content/scenes/camp-play.kinscene`
and its referenced prefabs/recipes.

```sh
cmake --build build --target camp_walk -j1
build/bin/camp_walk
build/bin/camp_walk --headless --demo --frames=240 --report=camp.json --screenshot=camp.png
```

Find and collect three supplies to stock the camp. WASD moves, Space jumps, Shift
sprints, C crouches, E collects a nearby supply, F pushes a nearby dynamic crate,
R restarts, and Escape exits. F5 refreshes content; F6 demonstrates typed prop/smoke
configuration. Hold the right mouse button to orbit the player-following camera; movement follows
its horizontal orientation. The shared `CameraRig3D` sweeps for wall clearance,
recovers its arm smoothly and draws the player at the matching interpolated pose
without changing controller physics. Fixed-step visibility uses an independent
camera arm, so render frequency cannot change collection. Gamepads use the left stick to move, right stick to orbit,
A/South to jump, B/East to crouch, X/West to collect, Y/North to push, left-stick click
to sprint and Start to restart. The floor uses a
compound recipe, the two barrels share a convex hull, and the tent uses hollow mesh
collision. The decorative campfire is a shared prefab with flames, smoke, sparks and
a light. Its composed effect prefab has a 208-particle base cap; this scene replaces
the smoke child with a smaller plume; a separate 24-particle foot-dust emitter
keeps the complete scene at a 200-particle cap, retaining source links.
Supplies, the crate and a hand-held lantern are runtime prefabs. The lantern
uses native `JointAttachment3D` on `hand_R`; its bottle/box meshes and warm light
follow the final animated hand, including the player's interpolated render root.
It adds four cosmetic entities and no bodies; population is bounded at five
runtime prefabs/eight entities. A plain native `Supply` component and the
existing ECS system API rotate pickups without inspector metadata. Input consumes a
fixed-size `PlayerCommand` once per step, PrePhysics sets the existing character input,
and PostPhysics uses fresh poses for interaction and crate impulses. An eight-slot
scene-aware sensor overlap finds supplies within 1.25 m; player/camera visibility rays
select a reachable, visible bottle and highlight its tint/light in gold. Walls and
props block collection and crate pushing. If the bounded candidate buffer overflows,
selection clears and `query_overflows` reports saturation. The third supply is placed
in clear view beside the crate. Each interaction query retains one sphere; discovery uses fixed result storage.
Contact submits removal at the late command boundary; the following Animation task
awards successful removals before supply animation. Failed removals never award points.
If no new command is submitted, the following step is neutral. Falling off the platform
requests a restart; the host replaces the scene between steps and explicitly carries
progress counters while resetting commands and round ownership.
Failed edits/reloads retain the running camp. `--content PATH` accepts a folder or `.kinpak`.
`--demo` runs a deterministic move/jump/crouch route, resets the level, collects all
supplies, pushes the crate and checks stale-handle rejection, ownership counts,
collision updates and preservation of living particles. `--list-actions` exposes the
existing named keyboard/mouse actions. Rebind them through `InputMap`; example-local
`InputMapper::settings` configures gamepad buttons, dead zone and look sensitivity.
The shared platform `InputMap` still covers keyboard/mouse; this example does not add
a second engine input framework. Its CPU mapper tests cover rebinding/edge semantics,
and GPU gameplay tests drive identical routes with scripted, keyboard and synthetic
pad commands through the same systems.

## Summit

Summit now includes selected [Kenney Survival Kit](https://kenney.nl/assets/survival-kit) CC0 assets for camp equipment and
lower-approach scenery, a fitted Kenney survivor with authored animation, and two 1K ambientCG PBR materials. See
[content, credits and budgets](summit/content/README.md). Larger expedition props have collision from the shared placement
catalogue (hollow tents/fire pits, solid gear/rocks, trunk proxies). Use `--procedural-assets` for the original artwork;
physics stays the same. Grass, tree canopies and small soft props remain permeable.

The default `--scenery dense` adds a forested lower approach, rocks and ground cover: seed 1 has 8,025 scenery instances
(803,074 instance triangles before culling), sharing the existing meshes and textures. Immutable instance batches use 16 m
spatial groups; their CPU records total about 0.55 MiB. Trees and rocks have matching colliders. Use `--scenery compact`
to retain the original sparse approach for comparisons.

Summit's art integration now uses engine `Assets3D` handles, `Model::placement` and `ModelBatch3D`; atlas sharing, model-part
expansion and cell grouping are reusable engine services. See the [3D asset/placement API](../docs/rendering_3d.md#shared-assets-and-whole-model-placement)
for scene-owned resource lifetimes, bounded weak caches and a minimal usage example.

Interactive play now starts **The Beacon**, a compact authored expedition. Begin from the briefing with R / pad Y or the on-screen
button, follow the gold approach markers, then choose the orange direct climb or the blue cave passage. The branches reconnect at
ridge camp. The cave has a stone floor, walls and ceiling with matching collision; standing still inside restores health and stamina,
wind is quieter, and precipitation stops. Camps remain fall checkpoints. Continue over the final wall and the snowy ridge, hold
R / pad Y near the beacon for two seconds to restore it, then hold the same control on the marked extraction pad for one second.
The completion screen reports time, falls and whether the cave was discovered; Enter, pad A or **Try again** starts a fresh expedition.
Esc opens the existing pause menu. The HUD shows the current objective, direction and distance, route colours and contextual
interaction progress. Interact uses the configured bindings, including gamepad controls.

```sh
KIN_RENDER_BACKEND=gpu ./build/bin/summit
./build/bin/summit --script expedition --seed 1   # CPU: direct branch, beacon, extraction
./build/bin/summit --script shelter --seed 1      # CPU: cave branch, beacon, extraction
KIN_RENDER_BACKEND=gpu ./build/bin/summit --headless --script shelter --width 960 --height 540 --screenshot /tmp/summit/cave
```

The expedition simulation is selected with `GameConfig::expedition`; its layout lives in `expedition.cpp`. Its phase, interaction
holds, discovery flag and timing hash and save/restore with the player. Compact expedition saves use version 3 and dense saves
use version 4; both reject mismatched layouts and classic-course
states; recorded command logs identify the map so `--replay` reconstructs the correct simulation. This is simulation snapshot support;
an interactive save/continue screen is still future work. The new `kin-summit-expedition` tests complete both routes in three seeds,
replay their inputs, restore a partly repaired beacon, check interrupted and out-of-range holds, verify shelter collision/recovery,
and check HUD bounds. `summit-expedition-shots` captures the full cave run, including briefing, fork, shelter, beacon and completion.
Server mode's `scene.current` exposes progression, interaction progress, feet, health/stamina, timing and the briefing/result button
bounds, so the real UI can be driven and inspected through the engine's existing input and screenshot commands.

For full-frame performance measurements, use the native uncapped window loop rather than CPU scripts or screenshot runs:

```sh
python3 examples/summit/run_benchmark.py --output /disk/backed/scratch/summit-profile --pairs 3
# One run, with streamed CSV timing and metadata:
KIN_RENDER_BACKEND=gpu ./build/bin/summit --benchmark /disk/backed/scratch/dense.csv --script shelter --scenery dense --width 2560 --height 1440 --samples 4
```

The harness alternates compact/dense runs, verifies the CPU replay hash and route completion, watches for competing GPU/build
jobs, and caps game RSS at 512 MiB. It renders every frame while the bot advances at the normal 60 Hz simulation rate.
Benchmark runs ignore live game controls so shared-desktop input cannot pause the bot, capture the mouse or change rendering options.
Reports include traversal p50/p95/p99, long frames, startup/asset setup, draw counts and process-tree peak RSS. Startup and the
two-second rendering warmup are excluded from traversal timings. The whole level is preloaded; this does not test asset streaming.

### Original climbing course

Use `--classic` to play the original course. The existing `--script climb`, `fall` and `rope` runs still use this course and retain
their regression hashes and behaviour. Existing `--camp` and `--on-decor` diagnostic pictures also select the original course;
`--expedition --camp N` explicitly selects one of the expedition's three camps and skips the briefing for inspection.

A one-player climbing game on the experimental 3D renderer and physics (`-DKIN_EXPERIMENTAL_3D=ON`; docs/3d_plan.md, "P4"): a
procedural mountain of about 150 m made from a seed (`--seed N`), seven camps, and a route of walkable approaches, pitches to
climb on stamina, a chimney, a roof to traverse under, a smooth wall with a rope, and a summit. It needs the SDL_GPU backend
(`KIN_RENDER_BACKEND=gpu`) to draw; its simulation needs nothing.

- Click the picture to play (Esc opens the menu and lets the mouse go). WASD move, mouse look, Shift sprints (15 stamina a second), Space
  jumps (6 stamina; while it hangs, jumps off), the left and right mouse buttons (or Q and E) take hold with the left and right hand
  and hold while pressed, W and S climb and A and D shimmy while it hangs, F pulls up over a lip (it also happens by itself when both
  hands are on one and you press forward), X lets go of everything, R rests at a camp, V first person, F2 the next look, F3 a debug
  line, F4 hides the HUD, F12 or P a picture. Gamepad: left stick move, right stick look, A jump, LT and RT the hands, X pull up,
  B let go, Y rest, L3 sprint, Start the menu (D-pad or left stick moves in it, A accepts, B goes back; the controller path is not
  exercised by any test with a device: the mapper and the menu are, with made-up states). The bindings, dead zones and
  sensitivities are `summit::InputBindings` (`summit/input_map.hpp`), a text table that round trips; the controls screen of the menu
  shows what is bound now.
- The HUD (P4b; ui2 over the 3D picture, scaled with the window, the same arrangement at 16:9, 4:3 and small sizes): altitude with
  the best this run and a bar of the way up; the stamina bar (green, amber under 30 %, red and pulsing when nearly gone or spent; the
  part injury has taken off its end is darkened); the two hands (open, reaching, holding, slipping); health with an injury label; a
  "BRACE" warning that speeds up with the fall (from 65 % of the safe landing speed, through "hard landing", to "knock-out");
  a camp notice; knocked out and getting up; and the summit message with the run's time and falls. `--no-hud` (or F4) hides it,
  also in pictures. All of it is a `HudModel` built from the game's state (`summit/hud.*`), laid out by ui2's solver, and tested on
  the CPU at several sizes.
- The menu (Esc or the pad's Start; the game is paused): resume, restart from camp (a deliberate act made through the game's
  state hooks: it is the one place the presentation changes the game, and it becomes a command in P5), look, camera, master, effects
  and ambience volume, mouse sensitivity, invert look, the clock's speed (or frozen), the controls screen, quit. Keyboard (arrows or
  WASD, Enter or Space, Esc or Backspace), pad and mouse all work.
- Sound (`--mute`: the engine's null mixer, the same code with no output; a machine with no audio device falls back to it): all
  synthesised at start-up, no files. Wind that rises with altitude and gusts with the sim's wind, footsteps by ground (grass, rock,
  snow) at a cadence from the speed, grab and let-go thuds at the hand, effort breathing as stamina runs low, a gasp when it runs out,
  a whoosh that grows with the speed of a fall and a thump by the damage, a rope's creak, a fire's crackle at each camp (positional),
  chimes at camps and a sting at the summit. Positional sounds are heard from the camera by kin's 3D listener. The cues come from
  the game's events and state as a plain `SoundCue` list (`summit/audio.*`): the tests count them for the scripted runs.
- The air (`summit/environment.*`, each effect switchable: `--no-fx altitude,time,snow,lights,fires,puffs,wind`, `--fx storm`):
  fog density, haze and the sun's strength change with altitude (clearer and brighter higher up); a snow line at 55 % of the way
  up, where snow falls (P3c) and lies (the rock's colours); the time of day advances slowly over a run (`--time-speed H` game
  hours a minute, 0.25 by default; `--freeze-time`; `--hours H` the start); each camp has a fire (flames, sparks, smoke), a
  lantern and a light that comes up at dusk and in the dark; dust where it lands and breath in the cold; the wind carries dust and
  flakes. `--look storm` or `--fx storm` is a heavier, wetter look (no lightning).
- Settings: `summit/settings.*` keeps the look, camera, volumes, clock speed, effects and key bindings in the game's settings file
  (`~/.local/share/kin/summit/settings.json` on Linux; `kin::SaveStore`) in a windowed run, written when something in the menu
  changes. A missing, damaged or out-of-range file gives the defaults (a field at a time). Scripted and headless runs use the
  defaults (a picture does not depend on the machine's file) unless `--settings DIR` names a directory; `--no-settings` never reads
  or writes it.
- The model: stamina 150, which comes back at 25 a second after a second of rest; two hands cost 7 a second, one hand 12, moving 4 more,
  a grab 2, a pull-up 8; standing with the feet on the ground is rest. Falls: under 7 m/s is free, above it 7 health for each m/s,
  9 m/s (a drop of about 4 m) knocks out (a ragdoll for at least 1.5 s, then a 0.8 s get-up); 0 health is a respawn at the last camp
  with everything back. Hurt lowers the stamina cap (150 x (0.5 + 0.5 x health / 100)). Health returns slowly standing still and fast
  in a camp.
- Looks (`--look NAME`): `afternoon` (the default), `morning`, `noon`, `dusk`, `night`, `snow`, `storm`: where the clock starts and
  the air is at the foot (the P3 sky, time of day and particles), not a mechanic. The wind is simulation state (it wanders with the
  game's seeded RNG) and drives the weather's wind and the wind's sound. F3's line at the top centre is step, stamina, health, wind,
  hour, fog, snow and the 3D render's CPU ms.
- A scripted run by a route bot (the same code the tests use), no window or GPU: `summit --script climb --timing` walks and climbs to
  the summit in about 9500 fixed steps (2.6 minutes of game time) and prints the steps, the state hash and the wall time (about
  1 s); `--script fall` lets go near the top of the chimney once (a knock-out and a get-up) and climbs on; `--script rope` stops over
  the rope. `--expect-hash HEX --expect-steps N` fail the run unless it ends so ("same machine" determinism: the look and the
  facing use `std::sin` and `std::atan2`), `--record FILE` writes the commands, `--replay FILE` plays them back.
  With `--screenshot PREFIX` a script is drawn at key moments to `PREFIX_<moment>.png` (base, menu, controls, first wall, second
  pitch, chimney, overhang, rope, summit; `ragdoll` for `fall`), HUD included unless `--no-hud`: `KIN_RENDER_BACKEND=gpu summit
  --headless --script climb --width 480 --height 270 --samples 1 --screenshot out/climb` (lavapipe is enough: a few seconds); it
  ends by printing the sound cues the run made. One picture of a place: `summit --headless --camp 3 --look dusk --frames 60
  --orbit 200,-12,5.5 --screenshot x.png` (`--drop H` starts H metres up, for a landing). `--gpu-timing` windowed with `--frames
  400` prints the GPU time of the frame. `--debug-colliders` draws the colliders near the camera as wireframes (boxes grey, buttresses orange, stones cyan) and `--on-decor boulder:3` stands on the fourth boulder, for a picture. `ctest -R summit` runs the unit tests (`kin-summit`, the simulation on the CPU;
  `kin-summit-presentation`, the HUD, menu, sound, environment and settings), and the scripted runs.
- The code is `summit/level.*` (the generator, the planner), `rock.cpp` (the render meshes: chamfered, roughened rock coloured by
  strata, moss and snow; buttresses, scree and boulders from `decor_chunks`, which `game.*` makes convex-hull colliders of; the ground and the ranges), `game.*` (`Game::step` of commands,
  the hash, save and restore), `input_map.*`, `camera.*` (a spring arm, a first person view, a lying body framed whole), `script.*`
  (the bot, the command log files), `hud.*`, `audio.*`, `environment.*`, `settings.*` (all plain data and functions, tested on the
  CPU), `view.*` (the climber, the rope, camps, the sky; reads a `Game` and writes a `Scene3D`), `hud_draw.*` and `main.cpp`.
  `summit_sim` links no window or renderer: the view is the executable's.

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
