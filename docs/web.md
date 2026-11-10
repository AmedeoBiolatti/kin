# Experimental WebGL2 builds

The next stages for general 2D games and 3D exports are recorded in the
[WebGL2 implementation plan](web_plan.md).

The Emscripten exports include the 2D `hex_demo` and a 2D/3D rendering fixture; the
3D Summit game's export is in the [kin gallery](https://github.com/AmedeoBiolatti/kin-gallery/blob/main/games/summit/web/README.md).
They share native gameplay code and render through WebGL2
without WebGPU, pthreads, Asyncify or cross-origin isolation headers.

## Build and package

Install and activate the [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html).
The initial build was checked with Emscripten **6.0.13**, CMake and Ninja. Source
`emsdk_env.sh` so `EMSDK` and the compiler tools are available, then run:

```sh
cmake --preset hex-webgl2
cmake --build build/hex-webgl2 --target hex_demo_package --parallel 4
```

The preset selects `KIN_RENDERER=webgl2`, disables native workers and unneeded
components, and enables the baked HUD font. It has a separate build tree and
does not inherit the desktop release contract, stripping or PGO policy.

The output is `build/hex-webgl2/packages/hex_demo-webgl2.zip`, with `index.html`,
`hex_demo.js`, `hex_demo.wasm` and `licenses/` at its root. The extracted files
also live in `build/hex-webgl2/web/hex_demo`. Serve them over HTTP:

```sh
python3 -m http.server 8000 --directory build/hex-webgl2/web/hex_demo
```

Opening the HTML directly with `file://` will not load Wasm. On itch.io, choose
**HTML Game**, upload the ZIP, and mark it playable in the browser. The shell
fills its iframe and includes a Fullscreen button. In **Embed options**, choose
**Embed in page → Manually set size** and enter **960 × 540**, or choose
**Click to launch in fullscreen**. Explicit embed dimensions avoid itch.io's
“No dimensions provided or detected” error; automatic detection is not required.
The HTML canvas also declares a 960×540 initial size, while CSS and the runtime
continue to resize it to the containing iframe/fullscreen viewport.
Tab changes hex orientation,
O changes offset layout, R resets, left click selects a goal, and right click
moves the unit. Local iframe testing is implemented; publishing and acceptance
on an actual itch.io page remain to be done.

## Runtime

`App::start_loop(update, render)` installs owned callbacks and resets the fixed
timestep accumulator. Call `App::step_loop(elapsed_seconds)` once per browser
animation frame: it processes input, performs bounded fixed updates and renders
without sleeping. Desktop `App::run` uses the same code.

`WindowedAppSession` owns the window, renderer, input and frame callbacks.
`SceneAppSession` additionally owns the scene manager, copied config/game metadata,
reports and shutdown state. Its `step()` drives one frame; native `run()` uses the
same state with the existing blocking loop. Shutdown is idempotent and destroys
scene resources while the renderer is alive, including when an exit callback
throws.

Use the owned scene entry point for a browser game:

```cpp
auto scenes = std::make_unique<kin::SceneManager>();
scenes->push(std::make_unique<MyScene>());
return kin::run_scene_app(config, std::move(scenes), services);
```

The optional `std::shared_ptr<void> services` keeps game-owned assets, translations
or other services alive across callbacks. Raw service/output pointers and callback
reference captures remain borrowed unless retained by it. Hex now uses this same
entry point on desktop and browser. Browser runs transfer ownership to the
animation driver and return after initialization; it handles canvas resize,
background-tab gaps, quit and exceptions. The Wasm runtime stays alive after
`main()` returns. The older borrowed-scene and `run_windowed_app` wrappers remain
synchronous; a blocking browser `App::run` is rejected explicitly.

`KIN_ENABLE_THREADS=OFF` removes worker creation and the `Threads::Threads` link
requirement. `JobSystemConfig::single_threaded` also selects cooperative jobs in
a native build. Submission queues work without executing it under caller locks.
`pump()` executes the queued generation and applies callbacks in submission
order; `wait()`/`get()` help execute queued work; `drain()` processes subsequent
generations too. `parallel_for` runs serially. Long jobs can still stall frames:
this mode provides no background CPU execution. Worker count zero retains its
old meaning of automatic sizing in a threaded build.

## Supported rendering and follow-up work

The backend supports batched RGBA sprites, tint/rotation/flips, tessellated
geometry, transforms, blend modes, viewport/scissor clipping, logical
letterboxing, nearest/linear/mipmapped sampling and nested RGBA8 render targets.
Render targets and 3D views use premultiplied alpha and preserve their top-left
orientation when drawn as textures. Context loss displays a reload message;
automatic resource restoration is not implemented.

Layers/masks, custom 2D materials, distance-field shaders, post-processing,
compute and data buffers remain unsupported. Loose content can be preloaded with
the host Emscripten packer; Hex and the render fixture have no external content.
Every `kin_game` in a browser gets a 1 MiB stack (`KIN_WEB_STACK_SIZE`):
Emscripten's 64 KiB default is less than creating a flecs world takes.
With `KIN_ENABLE_TTF` it also preloads Noto Sans' Latin subset
(`examples/content/lang/fonts/latin.ttf`, OFL) as `/kin/fonts/ui.ttf`, the UI's
system font there: a browser has none, and kin's bitmap font has capitals only.
Native pack creation/cooking, touch controls and Signal Siege remain follow-up
work.

## First 3D view

The `webgl2-3d` preset enables `KIN_EXPERIMENTAL_3D`, disables
`KIN_ENABLE_PHYSICS3D` and `KIN_ENABLE_SCENE3D_RUNTIME`, and builds a small fixture
without SDL_GPU, Lua or Jolt:

```sh
cmake --preset webgl2-3d
cmake --build build/webgl2-3d --target webgl2_3d_fixture_package --parallel 4
python3 -m http.server 8000 --directory build/webgl2-3d/web/webgl2_3d_fixture
```

The ZIP is `build/webgl2-3d/packages/webgl2_3d_fixture-webgl2.zip`. Left/right
arrows turn the lit cube; Escape stops the owned session.

`Renderer3D::available()` now identifies a real WebGL2 provider. It uploads
indexed/non-indexed meshes and ordinary LOD chains, culls on the CPU, compacts
visible copies into instanced vertex attributes and renders RGBA8 views with
24-bit reverse-Z depth. Stock materials share native metallic/roughness BRDF
math, support base/normal/metallic-roughness/occlusion/emissive maps, sRGB color
versus linear data, opaque/cutout/alpha/additive blending and double-sided faces.
Transparent copies sort back to front. Linear lighting is encoded to sRGB before
premultiplied composition into the gamma 2D frame.

Skinning uses integer joint attributes and a nearest-sampled RGBA32F matrix
texture, with independent palette offsets for instanced copies. This consumes
15 vertex attributes and bounds the total palette by `MAX_TEXTURE_SIZE` rows.
Posed meshes currently bypass bind-pose frustum culling to avoid disappearing
actors; conservative animated bounds remain an optimization. Ordinary meshes use
CPU culling and LOD.

Mesh VAOs retain their instanced attribute layout across uploads. Material
sampling uses at most 27 reusable WebGL2 samplers (three wrap modes per axis
and three filters); texture/filter edits still take effect on the next render.
Texture bindings are reused within a 3D render and invalidated at the next one
so interleaved 2D draws and asset uploads cannot leave stale state. Draw uniform
locations and fixed sampler units are resolved when the program is created.
Material scalar/color/UV-orientation uniforms are updated only when their values
change, including shadow and color passes; each linked mesh program owns its cache.

Each view also retains prepared instance transforms and compacted GPU instance
lists for its shadow cascades and opaque/masked LOD draws. Persistent batches use
their identity/revision; other draws compare transforms, tints and skin offsets.
Visibility is still computed each render and changing the visible order rebuilds
the affected list. Transparent instances keep their sorted streaming path.
All views share limits of 4 MiB of prepared records, 16 MiB of retained instance
buffer payloads and 512 retained buffer/VAO pairs; draws that cannot fit stream
normally. Resizing or destroying a view releases its cached lists. Edited lists
replace buffer storage so already-submitted draws keep their prior data.
Benchmark CSVs expose `retained_upload_bytes` alongside `instance_upload_bytes`;
sum them for total 3D instance record uploads. `prepared_draws` and
`prepared_reused` describe CPU transform reuse. These exclude textures, mesh
uploads and driver allocation overhead.

The 2D backend retains indexed geometry for sprites, text and dense shape meshes.
Rounded rectangles, circles, ellipses and their strokes use instanced analytic
quads with the same distance/coverage math as the native GPU backend, avoiding
per-frame CPU tessellation. Triangle meshes remain available for general paths.
Both paths preserve clipping, logical coordinates, transforms and draw order.
Native-coordinate draws also skip unused SDL window-size queries per vertex.

The core-quality environment includes procedural sky, sun/moon discs, shared
height/distance fog, sun/fill/hemisphere lighting, up to 16 point lights and up
to four directional-shadow cascades with depth24 maps and manual PCF. Float data
textures need no float color-target extension. Sky ambient lighting uses a cached
CPU hemisphere approximation. Each view owns its cache and respects
`Sky3D::light_threshold` and `light_interval`; shadow, sky and mesh passes reuse
that result. A game can supply the same cached hemisphere lighting
to either renderer and skips the separate high-resolution time-of-day ambient
integrals, while sun/moon/fog still update each frame.
Prefiltered IBL, star fields, spot/local shadows
and particles remain unavailable. Point lights currently use the first 16 in
scene order rather than the native importance ranking.

Views use RGBA8 and one sample. HDR, bloom and picking views, clusters, wireframe
and custom material shaders are refused; unsupported spot/particle draws warn.
SDR output clips bright values and does not apply native HDR exposure/tonemapping.
Stock GLSL ES is expanded and embedded at build time using shared pure-math
includes; custom shader assets/reflection and general 2D effects remain pending.

## Validation

With Node 22+ and Chrome installed, run the dependency-free integration check:

```sh
node tests/webgl2_smoke.mjs \
  --root build/hex-webgl2/web/hex_demo \
  --output /tmp/kin-webgl2-smoke
```

Use `--chrome /path/to/chrome` for another executable location. The check serves
the bundle in an iframe without COOP/COEP, checks a real WebGL2 context, changes
orientation with the keyboard, selects a path, moves the unit, enters/exits
fullscreen, resizes the canvas, and fails on
browser exceptions or GL errors. Screenshots and a JSON result are saved.
Chrome uses SwiftShader here: this verifies the browser API path and interaction,
not hardware GPU performance. Other browsers, mobile and production hosting
need further validation.

The 3D fixture has a second browser check:

```sh
node tests/webgl2_smoke.mjs --mode 3d \
  --root build/webgl2-3d/web/webgl2_3d_fixture \
  --output /tmp/kin-webgl2-3d-smoke
```

It checks actual pixels for near/far occlusion, multiple instance transforms,
texture/view orientation, translucent sorting/composition and nested targets.
Near/far and offscreen copies are culled; oversized and feedback targets are
refused. It also checks instance/cull counts,
keyboard rotation, canvas and view-target resizing, fullscreen and graceful
scene shutdown. The result includes queried WebGL limits. Material checks
additionally cover channel/color conventions, normal-map
orientation/scale, alpha cutoff, independent animated palettes and pose updates.
They also check independent wrapping on a shared texture, nearest/linear/mipmap
filter changes and sampler state across interleaved 2D/3D renders.
Sky/fog pixels are compared with native CPU math. Shadow cases cover casters,
receivers, cutout changes, animated casters and cascade statistics.

Native `kin-platform` tests caller-driven frame accumulation, pause, overload
and restart. `kin-serial-jobs` tests deferred execution under caller locks,
callback order, nested waits, cancellation, exceptions and queue destruction.

`kin-owned-scene-session` checks owned config/services across frames, scene
replacement, repeated finish, exit/shutdown exceptions and cooperative asset
completion. Local native regression runs pass 67/67 tests in the minimal profile and 76/76
in the full 2D software profile, including profiling, scripting and server mode.
