# Changelog

All notable changes to kin are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and kin uses
[Semantic Versioning](https://semver.org/) — while the version is 0.x, minor
releases may change APIs.

## [Unreleased]

### Upgrading

- `Input::mouse_wheel_y()` is now for `update()`: what the wheel turned since
  a fixed step last read it. Code that reads the wheel at render time should
  read `frame_mouse_wheel_y()` instead (ui2 already does).
- `Input::advance_keyboard_edges()` is now `advance_step_edges()`: it also uses
  up the step wheel.
- `Input::set_mouse_wheel_y(v)` adds `v`, as a wheel event would, instead of
  replacing the value.
- `IAudioBackend` is pulled instead of pushed: `queued_frames()` and
  `queue_interleaved()` are replaced by `start(render)` and `stop()`, and
  `AudioEngineConfig::queue_target_frames` is gone. Custom backends call the
  render function from their audio thread.
- `AudioEngine::stats()` returns a copy, and `mixed_frames` is 64-bit.
- `set_bus_volume("master", v)` now scales every bus under `master`, as the
  hierarchy says; before, it only reached voices playing on `master` itself.

### Added

- `Input::frame_mouse_wheel_y()`: the wheel since the last rendered frame, for
  render-time UI, as `frame_pressed()` is to `pressed()`.
- Audio: `AudioEngine::render()` mixes on the calling thread, for offline
  rendering and tests. `set_bus_volume` takes a fade, `set_bus_muted` and
  `set_bus_paused` mute or pause a bus and everything under it (pause
  gameplay sound while UI sound plays on), and `AudioPlayRequest::fade_in`
  fades a voice in.
- Audio: `preload(catalog)` decodes a catalog's clips up front, and
  `preload_async(catalog, jobs)` does it on worker threads; a `play()` that
  needs a clip still loading waits for it instead of decoding it again.
- Audio: a cue with several clips plays a random one (never the same twice
  running), and `pitch_var` varies its pitch, both drawn from
  `AudioEngineConfig::seed`.

- Audio formats: Ogg Vorbis, MP3, FLAC and AIFF play alongside WAV, decoded
  by stb_vorbis and dr_libs (new dependencies, fetched by CMake like the
  others). `load_audio_clip` recognises a file by its contents.
- Streamed clips: `clip theme music/theme.ogg stream=true` keeps the file
  compressed in memory and decodes it as it plays, for music and long
  ambience. `load_audio_stream` loads one directly, and `AudioDecoder` /
  `open_audio_decoder` decode a file held in memory.
- Music: `play_music(catalog, cue, crossfade)` keeps one music cue playing
  and crossfades to the next; asking for the cue already playing keeps it
  going. `stop_music`, `music()`.
- Voice control: `set_volume(handle, volume, fade)`, `set_pitch(handle,
  pitch)` and `playback_position(handle)`.
- Ducking: `duck music when=dialogue volume=0.3 attack=0.15 release=0.8` in a
  catalog turns a bus down while another plays (`AudioCatalog::add_duck`).
- `write_audio_settings` / `apply_audio_settings` keep the output device, bus
  volumes and mutes in a settings file, `AudioEngine::bus_states()` lists them, and
  `AudioEngine::write_report` writes stats, buses and voices for a scene's
  report.
- Lua: `bind_lua_audio` gives scripts an `audio` table, and
  `ScriptSceneConfig::bind` (with the matching `ScriptEngine` constructor)
  lets a host add its own bindings to a script scene's Lua state.
- Audio voices: `seek(handle, seconds)` (declicked, streams too) and
  `set_paused(handle, paused)` / `paused(handle)` for one voice.
- Audio memory: `unload_unused()`, `unload(catalog)` and `remove_clip(id)`
  free cached clips, and `loaded_clip_count()` / `loaded_clip_bytes()` say
  what the cache holds. Finished voices hand their clips back to the game
  thread to be freed.
- Distance curves: a cue's `rolloff=smooth|linear|inverse`,
  `rolloff_power=` and `pan=` strength (`AudioRolloff`, `SpatialAudio`).
- Output devices: `list_audio_output_devices()`,
  `AudioEngine::set_output_device(name)` and `output_device()`; a chosen
  device that is unplugged falls back to the default and back again.
  `create_sdl_audio_backend` takes a device name.
- Lua's `audio` table adds `seek`, `position`, `set_paused`, `paused`,
  `output_devices`, `set_output_device` and `output_device`.
- Audio effects per bus: low-pass and high-pass filters, a reverb
  (Freeverb) and a compressor, in a bus's chain from `effect` lines in its
  catalog or `set_bus_effects`; `set_bus_effect` changes one while it plays
  (filters glide to a new cutoff, reverb tails carry on). Lua scripts get
  `audio.set_bus_effects` and `audio.set_bus_effect`.
- Loop points: `loop_start=` and `loop_end=` on a catalog clip (in frames)
  make a looping cue play its intro once and then repeat the loop, streamed or
  not.

### Changed

- WAV files are decoded by dr_wav instead of `SDL_LoadWAV`, and loading a
  clip no longer initialises SDL's audio subsystem.
- Audio mixes on the device's audio thread, so a long frame no longer makes
  it crackle, and output latency no longer depends on the frame rate.
- `stop()` and `stop_bus()` honour their fade; even an immediate stop, or a
  voice stolen for a new one, fades over 5 ms instead of clicking. A stopped
  voice no longer counts as playing.
- Clips keep their file's sample rate and channel count (mono stays mono)
  and are resampled with linear interpolation as they play, which also
  interpolates pitch changes. Engines run at other rates than 48 kHz now play
  clips at the right pitch.
- Spatial voices pan with an equal-power law, so a sound crossing the
  listener no longer dips in the middle, and gain and pan changes ramp over a
  block instead of stepping.
- Buses are mixed on their own and added into their parents (submixes), in
  blocks of at most 1024 frames; with no effects the output is the same as
  before. `write_report`'s buses list their effects.
- A master limiter turns the mix down when voices add up past full scale,
  instead of hard-clipping it (`stats().limited_frames` counts how often).

### Fixed

- A wheel notch read in `update()` reaches exactly one fixed step. It was
  cleared every rendered frame, so above the sim rate a notch on a frame that
  ran no step was lost (about half of them at 120 Hz), and a frame that ran
  several steps applied it in each.
- `AppFrameStats::update_steps` counts the step being run inside `update()`
  under `App::run`, as it already did under `run_for`: 1 is a frame's first
  step (it was 0).

## [0.2.5] — 2026-10-06

A smaller release, for large saves and cheaper UI text. Saves can be written
compact, carry a one-line summary and list without reading their payloads,
and a parsed JSON value is a quarter of the size it was. ui2 text is kerned
as the font says, draws exactly as wide as it measures and measures about 50
times faster; rounded rectangles are built without trigonometry.

### Upgrading from 0.2.4

- `JsonValue`'s layout changed (strings, arrays and objects are boxed):
  rebuild everything that includes `kin/core/json_value.hpp`. A value moved
  from is now null.
- Fonts kerned in GPOS now draw kerned, so their lines are narrower than
  before, and `measure()` changes by about a pixel for most fonts (heights
  follow the line's ink, as SDL_ttf's do). Layouts that hard-code text widths
  may want a look.

### Added

- Saves: `SaveStoreConfig::compact` writes settings and slots without
  indentation or line breaks, a fraction of the size and the time for large
  payloads; either form reads back. `SaveSlotInfo::summary` is a short line of
  the game's own, kept beside the slot's info so `list_slots` shows it.
- `parse_json_skipping(text, key)` parses a document passing over the root
  object's member `key` unbuilt, and `JsonValue::take_member` moves a member
  out of a parsed document instead of copying it.

### Changed

- `JsonValue` keeps strings, arrays and objects in a box of their own: a value
  is at most 32 bytes (was over a hundred), so a parsed save of millions of
  numbers costs a fraction of the memory. `items()` and `members()` return an
  empty container for a value of another type, as before; a value moved from
  is left null.
- `SaveStore::list_slots` reads each slot's info without parsing its payload,
  and `read_slot` / `read_settings` move the payload out of the parsed file
  instead of copying it.
- ui2 TTF text: drawing (Bitmap and Sdf) and `measure()` lay printable ASCII
  out by one walk over the face's metrics, read once a face, so text draws
  exactly as wide as it measures. Measuring no longer shapes the string with
  SDL_ttf each call: 210 measures of HUD labels went from 2.7 ms to 0.05 ms a
  frame. Drawing no longer asks SDL_ttf for each glyph pair's kerning. Other
  text (beyond ASCII) is still shaped by SDL_ttf.
- Rounded rectangles: both backends build the outline from one shared
  `rounded_rect_loop`, whose corner arcs come from a table of cos and sin a
  segment count, not from `cos`/`sin` for every point of every fill and
  outline: the same points, about 3.4x faster to build.

### Fixed

- Frame pacing under `max_fps`: when frames fell more than a period behind,
  the deadlines started over a full period from now, so a slow frame was
  followed by a wait. They now start over from now: a late frame is never
  made later by a wait.
- ui2 TTF text is kerned as the font says: kerning now comes from HarfBuzz
  (GPOS as well as a legacy `kern` table), solved per pair from SDL_ttf's
  shaped widths and shared by every size of a font. Drawing used
  `TTF_GetGlyphKerning`, which reads only the legacy table, so fonts kerned in
  GPOS (Lato, EB Garamond, Noto Sans) drew unkerned and wider than they
  measured, by up to 41 px on a line at 37 px.

## [0.2.4] — 2026-10-06

A release for 2D drawing. Everything can be drawn through a transform, and
cameras zoom and turn; vector shapes are composed in code or read from SVG;
text stays sharp at any size; sprites mirror; anything can be clipped to a
path, a texture or anything drawn; and an opt-in linear colour pipeline blends
as light does, with HDR, tonemapping and LUT grading. Under them: layers,
cached targets, compute shaders, mipmaps and a faster SDL_GPU backend, plus
the render probe and the determinism check for finding what draws where and
what differs between runs.

### Upgrading from 0.2.3

- Children follow their parents' rotation and scale, not only their position,
  and renderer offsets, sizes and line ends go through the entity's world
  transform. Code that turned or scaled children by hand to make up for it
  should stop.
- The ECS physics sync converts angles (`Transform2D::rotation` is degrees,
  Box2D's radians) and works in world space for bodies under a parent. Games
  that converted angles themselves should drop the conversion.
- flecs is 4.0.5 (was 4.0.3): worlds in one process keep their own component
  ids. A project that pins flecs itself should move to 4.0.5.
- SDL's renderer now has layers: `begin_layer` draws into a target and applies
  its opacity, where it drew straight through before.
- `pop_clip()` pops whatever clip is on top, rectangle, path or mask; code that
  only pushes rectangles is unaffected.
- `IRenderer2DBackend` gains virtuals for shapes, distance fields, masks,
  stencil clips, layers and colour, each with a default, so custom backends
  build unchanged. `RenderCommandType` gains `Shape` and `Group`: a `switch`
  over it may warn until they are handled.

### Added

- Render probe: `--probe-render[=PATH]` on any `run_scene_app` game renders each
  headless frame, compares it with the frames before it tile by tile, and
  writes a `kin.render_probe/1` report of flicker (jitter, frame popping, single
  wrong frames) and spikes, with when and where each happened. `--probe-fail`
  fails the run when it finds anything; `--probe-tile=N` sets the tile size.
  `kin::RenderProbe` runs the same analysis on frames from anywhere. The
  `KIN_ENABLE_RENDER_PROBE` CMake option (on by default) compiles it out.
- Render probe events name their culprits: the entities and render components
  (or named scopes, or scenes) whose draws changed where the event happened,
  and how (`moved`, `frame`, `color`, `appeared`, ...). `KIN_DRAW_SCOPE(label)`
  and `KIN_DRAW_ENTITY(entity, component)` name draws made outside the ECS
  render components.
- Determinism check: `--check-determinism[=PATH]` on any `run_scene_app` game
  runs it three times in lockstep (twice alike, once with one job worker),
  hashing each frame's state, and reports the first frame where a run differs
  with the entities, components and report fields that differ
  (`kin.determinism/1`). `kin::hash_state` / `kin::describe_state` hash and list
  a scene stack's state. The `KIN_ENABLE_DETERMINISM_CHECK` CMake option (on by
  default) compiles it out.
- `KIN_JOB_WORKERS` sets the default job system's worker count.
- `ProcessOptions::environment` sets variables for a child process.
- `kin::CachedTarget` and `kin::cache_key(...)`: a render target drawn again
  only when its key or size changes (`kin/renderer/cached_target.hpp`). A
  288 x 288 target of 44 layers: 4.3 Mpixels and 0.5 ms of CPU a frame redrawn,
  0.08 Mpixels and 0.002 ms cached.
- `Renderer2D::begin_layer({.opacity, .resolution, .blend})`: draws land in a
  pooled render target in the same coordinates and are laid over once, at one
  opacity, over only the box they covered; `resolution` below 1 for soft
  content. Four sparse shadow layers on a 2560 x 1440 screen: 15.9 Mpixels a
  frame by hand with whole-screen targets, 2.3 with layers, 1.4 at half
  resolution. SDL_GPU.
- Overdraw by part and by eye: a `gpu_scope`'s pixels and overdraw
  (`GpuScopeTiming::pixels` / `overdraw`, `overdraw.<name>` in `--profile`), and
  the overdraw view (`Renderer2D::set_overdraw_view`, the debug overlay's toggle,
  `--overdraw-view`), which shows how many times each pixel is shaded as
  colours. `--screenshot=PATH` saves a run's last frame.
- Overdraw: `RendererBackendStats::last_pixels_drawn` and `last_overdraw`
  (SDL_GPU), reported as `render.overdraw` by `--profile` and the debug
  overlay: how many times each screen pixel was shaded in the frame.
- `ScaleMode::Mipmapped`: textures drawn smaller than they are get mipmaps,
  made on the GPU (and remade on update), sampled trilinearly: no shimmer, and
  faster to read (4000 sprites of a 2048 x 2048 texture at 24 x 24: about 30%
  less frame time). SDL_GPU, RGBA8 textures.
- Shader hot reload for development: `kin::ShaderFile` keeps a GLSL file
  compiled (with `glslc`, about 0.1 s) and reloaded in place when it is saved,
  keeping the last good version when an edit does not compile;
  `kin::compile_glsl()` and `Renderer2D::reload_shader()` underneath.
- Compute shaders: `Renderer2D::create_compute_shader` (layout and workgroup
  size read from the SPIR-V), `create_storage_texture` and `dispatch_compute`
  (`ComputeBindings`: sampled sources, read-only buffers, written textures and
  buffers, params), recorded in order with the frame's draws. SDL_GPU only
  (`capabilities().compute`).
- `Renderer2D::draw_shader_surface_scaled(resolution, ...)`: a costly, smooth
  effect computed at lower resolution into a pooled render target and
  stretched back (half resolution: about a third of the GPU time).
- Storage buffers for shaders: `Renderer2D::create_data_buffer`,
  `update_data_buffer`, `write_data_buffer`, and `draw_shader_surface` /
  `draw_shader_geometry` taking `DataBuffer`s, bound after the textures
  (`kin/renderer/data_buffer.hpp`). SDL_GPU only (`capabilities().data_buffers`).
- `Renderer2D::pipeline_record()` and `prewarm_pipelines(record)`: the GPU
  pipelines a run made, made again while the next run loads instead of at
  their first draw (up to ~20 ms each on a cold driver cache). `run_scene_app`
  keeps the record for windowed runs in the user data folder
  (`SceneAppConfig::pipeline_record_path`).
- Shader reflection: `create_shader` reads the SPIR-V's sampler, storage buffer
  and uniform block counts (a `ShaderDesc` that disagrees is logged, the
  shader's used), and `Renderer2D::shader_params(shader)` gives params that
  `set("name", ...)` by uniform block member. `kin::reflect_spirv` is public.
- `Renderer2D::write_texture(texture, at, size, fill)`: `fill` writes the
  texels straight into the upload memory on SDL_GPU, skipping the copy
  `update_texture` makes, for texels made each frame.
- `Renderer2D::gpu_scope("name")`: the GPU time of a part of a frame, reported
  as `gpu.<name>` by `--profile` and the debug overlay (and by
  `take_gpu_scope_timings()`). Measured with fences on each side, splitting the
  frame's submission there, while GPU timing is on; otherwise it does nothing.
- `Renderer2D::draw_shader_geometry()`: triangles drawn with a material shader,
  so only the pixels a shape covers run it, instead of a rectangle over its
  bounds. Each `kin::ShaderVertex` carries four free floats the fragment shader
  reads at `location = 2`, so one draw can hold many shapes with their own
  parameters. SDL_GPU only (`capabilities().shader_geometry`).
- `BlendMode::Max` and `BlendMode::Min`: per channel (alpha too), the larger or
  smaller of what is drawn and what is there. Overlapping shadows, fog of war,
  coverage and heat maps can be drawn shape by shape. The SDL_GPU backend has
  them; SDL's software renderer draws them as `Alpha` with a warning
  (`RendererBackendCapabilities::min_max_blend`).
- Frame times within `AppConfig::snap_tolerance` (1 ms) of a whole number of
  fixed steps count as exactly that many (`kin::FrameTimeSnapper`), so ordinary
  display jitter no longer runs some frames 0 updates and the next 2. The time
  snapped away is paid back a whole step at a time, so game time keeps up.
- `RendererBackendStats::gpu_frames_sampled` and `last_gpu_frame_span`: when
  new GPU timing arrived, and how many frames it covers.
- Transforms: `Renderer2D::push_transform` / `pop_transform` / `scoped_transform`
  draw through a `kin::Affine2` (`kin/core/affine.hpp`), on both backends.
  Mapped on the CPU, so draws still batch across transform changes, and sprite
  batches stay instanced under rotation and even scale. Through a zoomed,
  turned camera 20,000 quads take about 5% more CPU to record
  (`kin_draw_bench 300 20000 camera`); untransformed drawing costs what it did.
- `Camera2D::zoom` and `rotation`, about the viewport's centre, with
  `view_transform()`, `center()` and `look_at()`; `world_to_screen`,
  `screen_to_world` and `visible_rect` follow them, and a `RenderQueue` flushed
  with such a camera draws through it.
- `Transform2D::scale` and `WorldTransform::scale`; `kin::world_transform(entity)`
  and `kin::compose(parent, child)`.
- Vector shapes (`kin/renderer/path.hpp`, `shape.hpp`, `svg.hpp`):
  - `kin::Path` (lines, curves, SVG arcs; rectangles, circles, polygons, arcs,
    pies, stars; SVG path data read and written), filled under non-zero or
    even-odd with holes, stroked with joins, caps and miter limits;
  - anti-aliased at any scale: one pixel of soft edge from screen-space
    derivatives on SDL_GPU, pulled in on the CPU on SDL_Renderer;
  - circles, ellipses, rounded and sharp rectangles and capsules drawn whole
    on SDL_GPU (one quad, the outline from its distance per pixel), as you go
    or kept in meshes; 2000 cached tokens record in 0.88 ms (3.5 ms as
    triangles);
  - `kin::Shape`: elements that compose (`add(shape, transform)`), tessellated
    once into a `ShapeMesh` drawn by `Renderer2D::draw_shape` through any
    transform and tint;
  - SVG-lite: `read_svg` / `load_svg` read the flat-colour subset vector
    editors export (paths, basic shapes, groups, `<use>`, transforms, styles,
    inheritance), listing what they skip; `write_svg` / `save_svg` write it;
  - immediate drawing: `fill_circle`, `draw_circle`, `fill_ellipse`,
    `draw_ellipse`, `fill_polygon`, `draw_polygon`, `draw_polyline`,
    `draw_line` with a width and cap, `draw_arc`, `fill_pie`, `fill_path`,
    `stroke_path`;
  - `kin::ShapeRenderer` and `RenderQueue::draw_shape`; `games/shapes_demo`.
  - New dependency: mapbox earcut.hpp 3.2.4 (ISC), header-only, for fills.
- Text that scales: TTF fonts made with `ui2::TextRendering::Sdf`
  (`load_ttf_font`, `system_ui_font`, `system_ui_font_bold`) draw from one
  atlas of signed distance fields, sharp at any size, zoom or turn, under any
  transform or camera; their metrics scale linearly. The atlas is made once per
  renderer from glyphs drawn at twice the size, distance-transformed on the job
  system (about 15 ms). `ui2::draw_text_outlined` draws an outline from the
  field in one pass (four stamped copies on other fonts); widgets'
  `TextStyle::outline_width` uses it. `Renderer2D::draw_distance_field` draws
  any distance-field texture. SDL_GPU (`capabilities().distance_fields`);
  drawn as Bitmap elsewhere.
- Clips and masks: `Renderer2D::push_clip(path, rule)` clips to any path,
  anti-aliased, under the transform; `push_mask(draw, options)` masks with
  whatever `draw` draws, and `push_mask(texture, dest, options)` with a texture.
  `kin::MaskOptions` (`kin/renderer/mask.hpp`) reads a mask's alpha or
  luminance, as a soft `Alpha` mask or an all-or-nothing `Stencil` at a
  threshold, optionally inverted, at a fraction of the resolution. Rectangles,
  paths and masks share one stack, nest, and pop with `pop_clip()`;
  `scoped_clip` and `scoped_mask` return guards. SDL_GPU composites in a shader
  (`capabilities().masks`); SDL's renderer by blending, or on the CPU for
  luminance and stencil masks and its software renderer. The shapes demo gives
  its planets night sides and a telescope following the rocket.
- Hard clips: `push_clip(path, rule, kin::ClipEdge::Hard)` keeps whole pixels;
  on SDL_GPU (`capabilities().stencil_clips`) it draws into a pooled stencil
  buffer instead of layers, about five times cheaper than a smooth clip, and
  stencil-mode masks read their mask into it, drawing what they mask straight
  on.
- `kin::ClipRegion` (`kin/renderer/clip.hpp`): a clip as a value (a path, a
  mask, a texture), pushed with `Renderer2D::push_clip(region)`.
- `RenderQueue::draw_group(key, content, clip)`: a queue drawn as one command
  at `key` through a clip, so clipped content keeps together in a sorted queue.
- `kin::ClipGroup`: clips an entity's renderers and its descendants' to a
  region in its own space, drawn as one group at its layer and order.
- `Path::rounded_rect(rect, radii)` with a radius per corner;
  `ui2::Context::push_clip(bounds, corner_radii)`, used by lists, tables and
  trees to keep scrolled rows inside their panel's rounded corners.
- Layers on SDL's renderer: `begin_layer` draws into a target and lays it over
  at its opacity, as on SDL_GPU, instead of drawing straight through.
- Colour: `Renderer2D::set_color_space(ColorSpace::Linear, hdr)` blends in
  linear light (sRGB colour textures decoded as sampled, colours decoded in the
  vertex shaders, sRGB or float targets) and encodes on the way out; `hdr`
  keeps light above white in a 16-bit float scene. Opt-in, also as
  `GameWindowInfo::color_space` / `hdr`; the gamma pipeline stays the default.
  `set_color_output(ColorOutput)`: exposure, tonemapping (`Reinhard`, `Aces`),
  grading through a 3D LUT cross-faded to a second, and dithering, in one pass
  before the swapchain (about 0.13 ms at 1080p). SDL_GPU
  (`capabilities().linear_color`, `color_output`).
- `kin::ColorLut` (`kin/renderer/color_grading.hpp`): LUTs from `.cube` files
  and PNG strips or grids, a neutral one to grade in an image editor, applied
  on the CPU too.
- Colour maths: `srgb_to_linear`, `linear_to_srgb`, `LinearColor`, OKLab
  (`to_oklab`, `from_oklab`), and `mix(a, b, t, ColorMix)` in sRGB, linear
  light or OKLab; `Gradient::mix`.
- The lighting demo draws in linear HDR, tonemapped (ACES) and graded per
  time of day; C switches to the gamma pipeline, G toggles grading.
- Mirrored sprites: `SpriteRenderer::flip_x` / `flip_y` and
  `TextureRenderer::flip_x` / `flip_y`, mirrored about the pivot; a negative
  world scale mirrors too (and cancels a flag). `kin::Flip` on
  `Renderer2D::draw_texture` / `draw_sprite`, the `RenderQueue` texture and
  sprite calls, and `SpriteInstance`; flipped sprites still batch, on both
  backends. Animatable as `SpriteRenderer.flip_x` / `flip_y`.

### Changed

- SDL_GPU shader draws with the same params (and shader, sources, state) in a
  row are one draw call, not one each: 2000 small shader surfaces a frame went
  from 0.47 to 0.24 ms back to back (`kin_draw_bench 60 1 surfaces`).
- SDL_GPU `create_shader` builds the shader's usual pipeline (alpha blend, an
  RGBA8 target) at once instead of at its first draw: on a cold driver cache
  that moves a ~19 ms hitch from the first frame drawing it to load time.
  Pipelines made later are logged at debug level with their cost.
- SDL_GPU quads with the default shader append straight to the last draw when
  its state matches, and the clip rectangle is computed once per change, not
  per draw: `fill_rect` about 12% less CPU, `draw_texture` 4%. `kin_draw_bench`
  measures `draw_texture`, `fill_rect` and `draw_sprites`.
- SDL_GPU sends a third less geometry: quads (textures, rects, lines) go as
  four corners drawn through a static index buffer, not six vertices (hex_demo
  189 to 126 KB a frame, same draw calls). An empty texture
  (`create_texture` with no pixels) is cleared on the GPU instead of being sent
  zeros, where its format can be a render target.
- SDL_GPU texture uploads (`create_texture`, `update_texture`) no longer submit
  a command buffer each: a frame's uploads are recorded into one, sent ahead of
  the frame, from a shared staging buffer that grows to fit big uploads and
  shrinks back. Texels are copied with streaming stores, and on the renderer's
  job system for uploads of 1 MB or more. Replacing a whole texture the frame
  hasn't drawn yet cycles its storage. Released textures are pooled (a few
  seconds, up to 256 MB) for `create_texture` to reuse. 150 uploads of 256 KB
  in a frame went from about 25 ms of calls plus a 70 ms stall at present to
  2.1 ms. `RendererBackendStats::texture_uploads` and `texture_upload_submits`
  count them; `kin_upload_bench` measures them.
- `--profile` (and `--profile-json`, `--profile-text`, `--profile-lines`) no
  longer makes a run headless: it profiles the window, until the game quits or
  `--frames` N. For the old 600-frame headless pass add `--headless`.
  (`--profile-render` is still a headless pass.)
- `gpu.frame` is recorded only on frames with a new GPU sample, instead of
  repeating the last one; a sample taken after untimed frames (the GPU far
  behind the CPU) is their average rather than their sum.
- Children follow their parents' rotation and scale: `propagate_transforms()`
  (and `world_position()`) turn and scale a child's position by its parents'.
  Before, only positions and angles were added, so a child of a turned parent
  stayed put while the parent turned.
- Renderer offsets, sizes and line ends go through the entity's world
  transform; `TextureRenderer` and `RectRenderer` now turn with their entity
  (they ignored `rotation`), and queued `FillRect` / `DrawRect` commands honour
  `rotation` and `pivot`.
- ui2 lists, tables and trees keep their scrolled rows inside their panel's
  rounded corners (`Context::push_clip(bounds, corner_radii)`).

### Fixed

- ui2 text moves the pen by each glyph's advance, not the width of its bitmap:
  italic text is no longer letter-spaced, glyphs that overhang (an f or a j in
  many faces) no longer push the next one away, and drawn text matches
  `measure_text` more closely.
- SDL_GPU: a texture released right after it was drawn, before the frame was
  flushed, left the queued draw a dangling handle: a crash, or on Vulkan a lost
  device once SDL destroyed the image. The backend now keeps the textures the
  frame's draws use alive until the frame is submitted.
- SDL_GPU: GPU frame timing (`--profile`, `KIN_LOG_FRAME_STATS`, the debug
  overlay) waited on frame fences with `SDL_WaitForGPUFences` on its own thread,
  which also ran SDL's resource cleanup there; under load on Vulkan that lost the
  device or stalled frames for seconds. It now polls `SDL_QueryGPUFence`, a plain
  status read (resolution about 0.1 ms).
- SDL_GPU: with GPU frame timing on and the GPU more than 8 frames behind (a
  heavy scene, no vsync), each further frame's fence was released while the
  frame still ran. SDL put it back in its pool and reset it for the next
  submission, whose resources it then freed when the old frame finished: on
  Vulkan, a lost device a few seconds into `--profile`. Those frames are now
  submitted without a fence (and go untimed).
- Worlds in one process can register components in any order: flecs 4.0.5
  (from 4.0.3) keeps C++ component ids per world. Before, a component first
  registered in a second world could take the id another type had in the
  first, and setting that type wrote past the component's storage (the
  prefab tests crashed once `Transform2D` grew). The animation preview and the
  tests run several worlds.
- Physics bodies under a parent: the ECS sync works in the world and converts
  to and from the parent's space. A simulated body (`sync_from_physics`) stays
  put in the world when its parent moves; one the entity leads
  (`sync_to_physics`) follows its parent. Bodies under bodies are written
  parents first. Before, world positions were written into the
  parent-relative `Transform2D`. `kin::to_local(parent, world)` (the reverse of
  `compose`) and `kin::current_world_transform(entity)` support it.
- The ECS physics sync converts angles: `Transform2D::rotation` is degrees, as
  everywhere in kin, and `PhysicsWorld` radians, as Box2D. Before, radians were
  written into `rotation`, so physics bodies' sprites barely turned.
- `fill_rounded_rect` and `draw_rounded_rect` on SDL_Renderer: a border drew
  as a faint one-pixel line instead of its width. Both now draw as shape
  primitives on every backend.
- The render probe traces draws where the renderer's transform puts them,
  not where they would be without it.
- Bitmap TTF fonts drawn at a scale that changes every frame opened a font face
  and built a glyph atlas for every value, kept for good: now the most recent 12
  faces and 8 atlases are kept.
- SDL_Renderer: a texture kept past its renderer (as the system fonts' static
  cache keeps glyph atlases) no longer frees a texture of the next renderer
  when it goes; the examples crashed on Windows.

## [0.2.3] — 2026-10-01

Tools for building a game around its data: a game's own files hot-reload as
they are edited, Lua runs rules and formulas outside the ECS in a deterministic
sandbox, and ui2 themes load from `.kintheme` files. Games also get files
dropped on the window, the system's file dialogs and child processes without
calling SDL, an optional frame rate cap, and GPU frame timing in the profiler.

### Upgrading from 0.2.2

- A windowed run given `--frames N` now quits after N rendered frames. Scripts
  that passed `--frames` to a windowed run and expected it to keep going should
  drop the flag.
- `KIN_LOG_FRAME_STATS` reports fps from the time between frames, so it reads
  lower than before when the GPU or a frame rate cap holds frames back.
- Voice-limit culling no longer logs a warning per request; read
  `AudioEngine::stats().culled_requests`, or log at debug level, to see it.
- `IRenderer2DBackend` gains `set_gpu_timing_enabled()`, with a default that
  does nothing, so custom backends keep working unchanged.

### Added

- `kin::FileWatcher` hot-reloads a game's own data files: `load_and_watch(path,
  load)` loads a file and reloads it when it changes, keeping the last good data
  when an edit is rejected; `watch(path, callback)` reports changes. Changes are
  found by polling (at most every 250 ms) and reported once a file has settled,
  so partial writes and save-by-rename are seen once. `SceneAppConfig::file_watcher`
  has `run_scene_app` poll it between frames, except in headless and server runs.
- `kin::LuaScript` (`kin/scripting/lua_script.hpp`) runs Lua for code outside
  the ECS (rules, AI, formulas): the host binds an API in `setup`, then
  `call(name, args...)` / `call_for<R>(...)` run the script's functions. A
  deterministic sandbox (no files, time, random numbers or loading code;
  `require` only from `module_root`; `print` to the log), an instruction budget
  per load and call, loads that keep the last good script when a new one fails,
  errors with script:line, and hot reload of the script and its modules through
  `FileWatcher`. New `docs/scripting.md`.
- Desktop integration without calling SDL (`docs/platform.md`): files dropped
  on a window (`Input::take_dropped_files()`, `drop_position()` while a drag
  hovers, `add_dropped_file()` for tests); the system's file dialogs
  (`kin::FileDialogs`: open, save, folder; answers taken on the game's thread,
  never shown in headless runs, `answer_next()` for tests and agents); and child
  processes with non-blocking pipes (`kin::Process`: `write`, `read_line`,
  `close_input`, `running`/`exit_code`, `wait`, `kill`; a child still running
  when its `Process` goes away is ended).
- An optional frame rate cap: `AppConfig::max_fps` / `WindowedAppConfig::max_fps`
  (0, the default, is uncapped), `App::set_max_fps()` to change it while running
  (e.g. from a settings menu), and `--max-fps N` on any kin game. Frames are
  paced to deadlines with `SDL_DelayPrecise`, so the rate holds without drift,
  and a frame that runs late resets the pace instead of rushing the next ones.
  Headless runs are never paced. `AppFrameStats::pacing_wait` and the profile's
  `app.pacing_wait` show the time slept.
- ui2 themes from data files (`kin/ui2/theme_file.hpp`, `.kintheme`): start
  from a built-in theme, name the game's own colours (usable anywhere a colour
  goes, with `@aa` alpha), set kin's colour tokens and sizes (the rest of the
  theme follows the tokens), style any surface (fill, gradient, border, radius,
  shadow, `like` another), and ask for procedural nine-slice skin frames.
  Unknown sections, keys and names are errors with their line, and a file with
  errors changes nothing; `parse_theme_file` fits `FileWatcher::load_and_watch`.
  `make_theme_from_tokens` is now public.
- GPU frame timing on the SDL_GPU backend: `gpu.wait` (time `present()` blocked
  on a swapchain image, i.e. waiting for the GPU) and `gpu.frame` (each frame's
  GPU time, measured with fences since SDL_GPU has no timestamp queries) in
  profiles, the debug overlay and `KIN_LOG_FRAME_STATS`, and as
  `RendererBackendStats::last_gpu_wait_ms` / `last_gpu_frame_ms`. Timing is on
  while something reads it, or via `Renderer2D::set_gpu_timing_enabled()`. The
  backend now also reports `present.flush` and `present.backend`.

### Changed

- `KIN_LOG_FRAME_STATS` reports fps from the time between frames, which counts
  waiting on the GPU and the frame rate cap, instead of from the frame's work
  alone (which overstated it); `frame_ms` is still the work.
- A windowed run given `--frames N` now quits after N rendered frames, running
  in real time with the frame rate cap; it used to ignore `--frames` unless the
  run was also made headless (`--report`, `--profile`).
- Audio requests culled by the voice limits are logged at debug level (still
  counted in `stats().culled_requests`) instead of a warning per request, which
  flooded the log of busy games.

## [0.2.2] — 2026-09-30

Large sprite counts get far cheaper: sprites that share a texture are drawn as
instanced batches on the GPU, the render queue stores plain sprites compactly,
and ECS sprite collection and batch building can use worker threads, with the
same results as one thread. Signal Siege, reworked with a title screen, sound,
new art and a 100,000-enemy stress mode, goes from 48 ms to about 12 ms a frame
at 100,000 enemies on the GPU.

### Upgrading from 0.2.1

- `RenderQueue` flushes hand runs of consecutive same-texture sprites to the new
  `IRenderer2DBackend::draw_sprites()`. Its default draws them one by one through
  `draw_texture()`, so custom backends keep working unchanged.
- `RenderQueue` stores plain Texture and Sprite commands compactly;
  `commands()`, `sort_commands()` and the presorted flushes convert them back on
  first use, so readers see the same commands, in submission order. As before,
  a span from `commands()` is invalidated by further submissions.
- `WorldRenderState::propagate_transforms()` updates parents before children in
  one pass, so a child no longer sees its parent's previous-frame transform when
  the child was created first.
- `WorldRenderState::collect_*` cull rotated sprites as they are submitted, by
  the circle they can turn within; a queue collected with a view may keep a few
  more rotated sprites near its edges than before (flushes still cull exactly).

### Added

- `Renderer2D::draw_sprites(texture, sprites)` draws many quads from one texture
  in one call; the SDL_GPU backend draws them as one instanced batch
  (`SpriteInstance`, `sprite_instanced.vert`), other backends quad by quad.
  `RenderQueue` flushes hand it every run of consecutive same-texture sprites, so
  ECS sprites batch automatically. `Texture` handles compare with `==`.
- `AudioEngine::add_clip` registers an in-memory clip (synthesized, or from
  `make_memory_audio_clip`) under a clip id that cues name like a file-backed one.
- Render queue and threading APIs: `RenderQueue::draw_texture_region()`,
  `append_sprites()` (with `PreparedSprite`), `reserve_sprites()` /
  `texture_index()` / `write_sprite()` for producers on several threads,
  `submitted()` and a ranged `cull(view, first, last)`;
  `SpriteRenderOptions::jobs`; `Renderer2D::set_job_system()` and
  `IRenderer2DBackend::set_job_system()`.

### Changed

- The parallel ECS sprite collect writes sprites straight into the queue
  (`RenderQueue::reserve_sprites()`, `texture_index()`, `write_sprite()`)
  instead of preparing them on workers and appending them on one thread: about
  3–4× faster than a single thread for 100,000 sprites, where the previous
  version was slower than one. Chunks are smaller (1,024 rows) so fast cores take
  over from slow ones. `RenderQueue` sorts in 11-bit radix passes and shifts
  queued sprites into the view with the camera offset looked up once. Signal
  Siege at 100,000 enemies: about 8.4 ms a frame on the GPU against 9.5 ms for
  main (same session); its sprite collect 1.1 ms against 1.9.
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

[Unreleased]: https://github.com/AmedeoBiolatti/kin/compare/v0.2.5...HEAD
[0.2.5]: https://github.com/AmedeoBiolatti/kin/compare/v0.2.4...v0.2.5
[0.2.4]: https://github.com/AmedeoBiolatti/kin/compare/v0.2.3...v0.2.4
[0.2.3]: https://github.com/AmedeoBiolatti/kin/compare/v0.2.2...v0.2.3
[0.2.2]: https://github.com/AmedeoBiolatti/kin/compare/v0.2.1...v0.2.2
[0.2.1]: https://github.com/AmedeoBiolatti/kin/compare/v0.2.0...v0.2.1
[0.2.0]: https://github.com/AmedeoBiolatti/kin/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/AmedeoBiolatti/kin/releases/tag/v0.1.0
