# Changelog

All notable changes to kin are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and kin uses
[Semantic Versioning](https://semver.org/) — while the version is 0.x, minor
releases may change APIs.

## [Unreleased]

### Added

- `Renderer2D::draw_sprites(texture, sprites)` draws many quads from one texture
  in one call; the SDL_GPU backend draws them as one instanced batch
  (`SpriteInstance`, `sprite_instanced.vert`), other backends quad by quad.
  `RenderQueue` flushes hand it every run of consecutive same-texture sprites, so
  ECS sprites batch automatically. `Texture` handles compare with `==`.
- `AudioEngine::add_clip` registers an in-memory clip (synthesized, or from
  `make_memory_audio_clip`) under a clip id that cues name like a file-backed one.

### Changed

- Large sprite workloads use worker threads when given a `JobSystem`:
  `SpriteRenderOptions::jobs` prepares big `TextureRenderer` tables in parallel
  chunks (queued in row order, via the new `RenderQueue::append_sprites()`), and
  `Renderer2D::set_job_system()` lets the SDL_GPU backend fill large
  `draw_sprites()` batches in parallel. Results are identical to a
  single-threaded run. Signal Siege passes its arena's workers, and its art finds
  the enemies near the view, their shadows and the few showing effects in one
  (parallel) pass. At 100,000 enemies a GPU frame took about 13.6 ms against
  19.7 ms for main in the same session; at 10,000, 1.85 ms against 2.39.
- `RenderQueue` keeps plain sprites compactly (no per-sprite `RenderCommand` or
  texture reference) and flushes them straight into sprite batches; its radix
  sort skips key words that never vary. Collecting and flushing 100,000 ECS
  sprites takes about 9.5 ms on the CPU, down from 16.6, and Signal Siege at
  100,000 enemies about 15.9 ms a frame on the GPU, down from 20.2. `commands()`
  and `sort_commands()` still show every command (converting queued sprites on
  first use); `cull(view, first, last)` now counts submissions (`submitted()`),
  not positions. New `draw_texture_region()` queues part of a texture.
- Signal Siege draws its drop shadows with one `draw_sprites()` call. With the
  enemy atlas, a frame at 100,000 enemies takes about 19.7 ms on the GPU, down
  from 24.7, and 2.4 ms at 10,000, down from 2.9.
- `WorldRenderState::propagate_transforms` visits parents before their children
  (a cascaded query) and does no per-entity parent lookup, so a flat world of
  100,000 sprites propagates in about 0.13 ms; nested hierarchies are now updated
  correctly whatever order their entities were created in.
- `WorldRenderState::collect_*` cull each sprite as it is submitted, rotated ones
  by a bounding circle, and cull only the particles (and commands already in the
  queue) afterwards, instead of re-checking the whole queue. `RenderQueue::cull`
  takes an optional command range.
- Signal Siege's three enemy sprites share one texture (an atlas), so the
  y-sorted enemies batch into a few GPU draws instead of one per texture change
  (about 28,000 a frame at 100,000 enemies, now 21): a frame there takes about
  25 ms, down from 29, and 2.9 ms at 10,000 enemies, down from 3.3.
- Signal Siege scales to 100,000 enemies (`--enemies` up to 100,000, and a
  `kin_bench` case `arena_100000_encounter`): the art gathers the enemies near the
  view once per frame for shadows, lights and effects; collisions, respawns and
  the autopilot read slot-indexed positions instead of looking each enemy up; the
  movement pass runs on worker threads for large arenas, with identical results;
  enemies are created and retextured in deferred batches. Simulation checksums are
  unchanged. At 100,000 enemies on the GPU a frame takes about 29 ms, down from
  48; at 10,000, about 3.4 ms, down from 5.3. New profiler zones
  (`--profile-lines`) split the frame.
- Signal Siege's sprites and floor are more detailed: sprites are rasterized at
  twice their drawn size and shaded as bevelled metal with seams, rivets and
  glowing eyes; ships cast soft drop shadows; the deck mixes four tile variants
  (plain, grate, hazard paint, patched) and has hazard strips along the arena's
  edge; reactors are bolted housings with turning fans. The colour roles are
  unchanged.
- Signal Siege reads more clearly: cyan is only the player (drawn over every
  effect, with a ground ring), enemies are solid warm shapes drawn after the
  lighting and without glow, enemy bullets are round white-cored pink orbs, an
  orbiter about to fire shows a closing ring, cores are small and blink before
  expiring, and the floor, lights, particles and bloom are toned down. The HUD's
  countdown gets a panel.
- Signal Siege has a title screen over an autoplaying arena, a results screen
  whose counters roll up, and scene transitions between them; sound effects and an
  ambient drone synthesized at startup and played with `AudioEngine`; a HUD built
  from ui2 widgets with a wave banner animated by property tracks; and the best
  run kept with `SaveStore`. Headless, benchmark and screenshot runs still start
  straight in the arena. `--mute` silences it.
- The Signal Siege example has a new look that shows off more of kin, still
  without asset files: procedurally rasterized sprites (enemies drawn by the ECS
  through `TextureRenderer`), a lit arena with `LightLayer` (a flashlight cone,
  reactor lamps, lights on shots and impacts), `ParticleSystem` effects driven by
  the arena's events, and a bloom, vignette and grade chain on the GPU backend.
  The simulation and the `kin_bench` arena cases are unchanged.

### Fixed

- An `AudioEngine` on the silent (null) backend advances its voices by elapsed
  time. It used to mix only until its queue target was reached, so one-shot sounds
  never finished and, once every voice and instance limit was taken, new ones were
  refused.

## [0.2.1] — 2026-09-30

A multi-line text editor for ui2, faster render queues, tilemaps, ECS inspection
and headless rendering, and a fix for GPU games crashing on exit.

### Upgrading from 0.2.0

- `RenderCommand` fields changed: a Sprite command keeps its texture in
  `texture` and its region in `source` (the `sprite` member is gone), and
  `material` is a `const Material2D*`. Only code that builds or reads commands
  directly is affected; `RenderQueue`'s methods are unchanged.
- After `RenderQueue::flush`, `commands()` keeps submission order; call
  `sort_commands()` first if you need the commands themselves sorted.
- `Input::consume_frame_edges` also clears typed text and the wheel delta.
- TTF text drawn from the glyph atlas is a little narrower (true advances and
  kerning), matching `measure_text`; layouts placed by eye may shift a pixel or
  two.

### Added

- `Renderer2D::id()`: unique for the life of the process, never reused.
- `kin_bench` cases `render.submit_flush_sprites_{2k,8k,32k}`: submit and
  flush, the everyday path.
- `RenderQueue::submit(std::span<const RenderCommand>)` appends a batch of
  commands, such as a cached one, in order.
- `ui2::TextEdit`: a multi-line text editor with wrapping, scrolling, mouse and
  keyboard selection, clipboard, grouped undo, a submit key and auto-grow; with
  `read_only` it is selectable, copyable text. Its state can live in the widget
  or in the `Context` (`Context::text_edit_state`). See docs/ui.md.
- `kin/core/utf8.hpp`: character-safe stepping (`utf8_next`, `utf8_prev`,
  `utf8_floor`), word stepping and `normalize_newlines`.
- `ui2::wrap_text_ranges`: wraps text into byte ranges, keeping every byte.
- `Key::PageUp` and `Key::PageDown`; `Input::frame_repeated` and
  `Context::key_typed` for the OS's key repeat; `Input::set_key_pressed`,
  `set_key_released` and `set_key_repeated` for tests and scripting.
- Scene server `input.key {name, mode?, modifiers?}` presses keys by name with
  modifiers held, e.g. `{"name":"Enter","modifiers":["shift"]}`.
- `games/text_edit_demo`: a chat box, a read-only history and a notes pane.

### Changed

- `RenderQueue::flush` draws in sorted order without moving the commands, so
  `commands()` keeps submission order after a flush. Call `sort_commands()`
  first if you need the commands themselves sorted.
- Text drawn from a TTF glyph atlas (printable ASCII) uses the font's advances
  and kerning. Glyphs narrower than a space no longer take a space's width, so
  such text is a little narrower and now as wide as `measure_text` reports.
- `Input::consume_frame_edges` also clears typed text and the wheel delta.
- `RenderCommand` is 152 bytes instead of 216. Code that builds or reads
  commands directly needs updating:
  - Sprite commands keep their texture in `texture` and their region in
    `source`; the `sprite` member is gone.
  - `material` is a `const Material2D*`. `RenderQueue`'s `MaterialRef`
    parameters are unchanged; only their `material` pointer is kept.
  - `RenderCommandType` is a `u8` enum.

### Performance

- Render queue sorting is 2–2.5× faster: keys are radix sorted, a queue that
  has not changed since its last sort is not sorted again, and `flush` skips
  reordering the commands. Submitting and flushing 32k sprites takes 4.2 ms
  instead of 9.7 ms.
- Smaller render commands make submitting 10–28% faster. Tilemaps also reserve
  queue space for the visible cells and hand their cached chunks over in one
  bulk submit: a partial-view submit takes 0.56 ms instead of 1.7, a full
  uncached one 6.3 ms instead of 10.2, and both allocate half as much.
- The software renderer, used for headless runs and screenshots, draws
  line-heavy scenes about twice as fast: while the logical size equals the
  window it leaves SDL's logical presentation off (with it on, SDL draws every
  line as triangles and blends each through a scratch surface), turning it back
  on when a resize makes them differ. Untextured draws whose colors are all
  opaque also skip blending. The hex demo renders a frame in 5.3 ms instead of
  12.6, `signal_siege` in 5.7 instead of 7.6. Thin translucent lines may
  rasterize a pixel differently.
- ECS inspection is 5× faster: `query_entities` and `EcsWorld::snapshot()`
  read each entity's parent and name once instead of on every comparison while
  sorting. A query matching 4.5k of 10k entities takes 1.7 ms instead of 9 ms.
- `TileMap::collision_rects` finds the rect to merge each run into with a
  lookup instead of scanning every rect found so far: 56× faster on a dense
  layer (101 ms to 1.8 ms), with identical output.

### Fixed

- `TextInput` moves and deletes by character, so multi-byte characters such as
  "é" or "°" no longer break. Ctrl+Left/Right move by word, Ctrl+Backspace and
  Ctrl+Delete delete one, held keys repeat, and pasted line breaks become spaces.
- The scene server's `view.screenshot` no longer replays the last step's input
  into a scene that reads input in `render()`: typed text was entered twice.
- GPU games no longer crash or hang on exit. Textures that outlive their
  renderer, such as the glyph atlases in the static system-font cache, skip
  their release once the GPU device is destroyed instead of calling into it.
- Font caches key on `Renderer2D::id()` rather than the renderer's address, so
  a new renderer created where an old one lived no longer draws text with the
  old renderer's textures.

## [0.2.0] — 2026-09-30

Features the first game built on kin needed: hex grids, 2D lighting, a job
system shared by the whole engine, and shaders with more textures, larger
uniform blocks and data textures. CI now also runs the GPU backend's tests.

### Upgrading from 0.1

- `ShaderParams::uniforms` is a `std::vector<f32>`: code that indexes it is
  unchanged; code that relied on it being a `std::array` needs updating.
- The ECS scheduler, `AssetServer` and `PathServer` share
  `default_job_system()`. `worker_count` in `AssetServerConfig` and
  `PathServerConfig` now limits concurrent work rather than starting threads.
- Destroying an `AssetServer` or `PathServer` drops work that has not started.
- A scripted `press` is released at the start of the next tick, so games see
  its release edge one tick later than before.

### Added

- `draw_shader_surface()` takes a list of source textures, bound at fragment
  sampler slots 0, 1, 2, ... up to `MaxShaderSamplers` (16); previously at most
  two.
- `JobSystem` (`kin/core/jobs.hpp`): background jobs with `Job<T>` handles,
  results applied in submission order by `pump()` / `drain()`, cancellation
  through `std::stop_token`, and `parallel_for`. Background jobs use at most
  part of the workers, so parallel loops always have threads.
- Hex grids: axial coordinates, directions, distance, rotation, rings, ranges
  and lines (`kin/core/hex.hpp`); `HexLayout` for pixel and offset-cell
  conversion in pointy-top and flat-top, odd and even offset layouts.
- Hex pathfinding: `HexGridNav`, `find_path` (A*) and `reachable_cells`
  (movement range).
- `games/hex_demo`, and a hex grids guide (`docs/hex_grids.md`).
- 2D lighting: `LightLayer` multiplies a drawn scene by an ambient colour plus
  `Light2D` point lights, optionally shaped by a texture (cones, spotlights).
  Works on the software and GPU backends without shaders.
- `games/lighting_demo`: lamps, a campfire, a flashlight and a day/night cycle.
- Data textures for shaders: `create_texture(size, format, pixels)` with
  `TextureFormat::R16Uint`, `Rg16Uint` and `R32Float` (on backends with
  `capabilities().data_textures`); `update_texture()` writes in the texture's
  format.
- Shader uniform blocks up to `MaxShaderUniformFloats` (4096 floats, 16 KiB).
- `frame.timing` server command: update and render time over the last 600
  ticks.
- ui2 tooltips warm up: once one has shown, the next shows at once
  (`TooltipOptions::warm_frames`).

### Changed

- `ShaderParams::uniforms` is a `std::vector<f32>` (16 floats by default)
  instead of a `std::array<f32, 16>`. Indexing it is unchanged.
- On the SDL_GPU backend, sampler slots a material shader declares but a draw
  leaves empty are bound to a white texture instead of being left unbound.
- The ECS scheduler, `AssetServer` and `PathServer` run on one shared job
  system, `default_job_system()`, instead of each starting its own threads.
  `set_default_job_system()` replaces it; each also accepts its own pool. The
  `worker_count` settings now limit how much work each runs at once.
- `AssetServer` and `PathServer` drains do queued work on the calling thread
  while they wait. Destroying either drops work that has not started instead
  of finishing it first.
- Every `GameInfo` member has a default, so designated initializers can omit
  any without `-Wmissing-field-initializers` warnings.

### Fixed

- A scripted `press` (`input.mouse`, `input.action`) is released at the start
  of the next tick instead of at the end of its own, so the release is seen and
  ui2 clicks complete. Before, a pressed button never produced a click.

## [0.1.0] — 2026-09-28

First public release.

### Added

- ECS on flecs: native and data components, prefabs, events, inspection, and a
  system scheduler with dependency graphs and parallel batches.
- SDL3 2D rendering: sprites, tilemaps (orthogonal and isometric), particles,
  post-processing, and an animation system with state machines.
- `ui2` immediate-mode UI with widgets, layout, and themes.
- Box2D physics, Lua scripting (sol2), audio, pathfinding (including the
  threaded `PathServer`), dialogue, save data, and hot-reloadable assets.
- Agent interface shared by every `run_scene_app` game: deterministic headless
  runs with JSON reports, a line-JSON/HTTP control server, screenshots, and
  profiling.
- Demos (`games/`), the Signal Siege and Run Observatory examples, `kin_bench`,
  and the engine test suite.

[Unreleased]: https://github.com/AmedeoBiolatti/kin/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/AmedeoBiolatti/kin/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/AmedeoBiolatti/kin/releases/tag/v0.1.0
