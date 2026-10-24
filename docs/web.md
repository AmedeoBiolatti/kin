# Experimental WebGL2 builds

The next stages for general 2D games and a playable 3D Summit export are recorded
in the [WebGL2 implementation plan](web_plan.md).

The Emscripten exports include the 2D `hex_demo`, a rendering fixture and the
3D Summit game. They share native gameplay code and render through WebGL2
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
Native pack creation/cooking, touch controls and Signal Siege remain follow-up
work. Summit includes bundled images/fonts, gesture-activated audio and persistent
IndexedDB settings.

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
that result. Core-quality Summit supplies the same cached hemisphere lighting
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

## Summit

```sh
cmake --preset summit-webgl2
cmake --build build/summit-webgl2 --target summit_web_package --parallel 4
python3 -m http.server 8000 --directory build/summit-webgl2/web/summit_web
```

The upload is `build/summit-webgl2/packages/summit_web-webgl2.zip`. It contains
root `index.html`, JS, Wasm, preloaded `.data` and third-party/game/font notices.
It uses the existing Summit scene, simulation, Jolt physics, animation, UI and
audio, with real bundled terrain, props and survivor assets. Missing bundled
assets fail startup instead of silently using procedural substitutes. The browser
profile selects SDR, one sample and two 1024px shadow cascades; native quality
settings retain their defaults. Native Summit can select the same rendering
profile with `--quality core --scenery compact`. Both core paths use the same
cached hemisphere lighting, disable stars/particles/spot lights and retain
weather's fog/light changes. Browser builds accept only core quality.
Compact scenery is the browser default, preserving the route and real assets
with a smaller scenery budget. Native retains dense scenery. To opt into the
full layout, use `?arg=--scenery&arg=dense`; its rendering performance is unmeasured.

Press R to begin, click to capture the mouse, use W/A/S/D to walk, Space to jump,
Shift to sprint, mouse buttons to grab and F to pull up. Escape opens the menu;
click to reacquire capture after leaving it. V switches camera and F2 cycles the
look. Fullscreen is provided by the host shell. Desktop keyboard/mouse is the
initial target.

Jolt always selects its serial job system when Kin has no workers, including
when a world requests the default pooled configuration. The native barrier adapter
cannot consume a cooperative queue safely. Native worker-backed worlds retain
the pooled path.

`KIN_WEB_PERSISTENCE=ON` restores `/persistent` from IDBFS before `main()` and
settings construction. Summit uses the existing SaveStore under that root and
explicitly requests a coalesced asynchronous flush after successful setting
writes. `Module.kinStorage` reports readiness, availability, pending writes,
flush count and errors. Storage denial/quota errors leave the game usable with
in-memory settings; persistence is not guaranteed until a flush succeeds.

Summit preloads a licensed UI font and sets `KIN_UI_FONT_REGULAR` and
`KIN_UI_FONT_BOLD` before UI construction. The shared UI font lookup honors these
paths before platform candidates; unset variables retain native discovery.
The loading overlay stays visible until the first game frame. Before that frame,
Summit prepares the HUD/menu font metrics and glyph atlases in an offscreen target
at the current UI size. It repeats preparation after a resize. This moves font
work into loading/resize rather than the first gameplay interaction. Hidden-tab gaps
are discarded on resume, and page navigation destroys the owned session while
its canvas still exists. Context loss requires reload.

The two new 3D build switches default to ON, preserving native presets. The
scripted scene runtime still requires scripting and 3D physics. Disabling physics
also omits the coupled 3D ECS/scene adapters; render-only builds use `Scene3D`
and `Renderer3D` directly. Existing 3D examples require those full adapters.

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

Summit's integration check requires regular Chrome, X11 and `xdotool` for real
native-window focus. Use an isolated display such as Xvfb:

```sh
xvfb-run -a node tests/webgl2_smoke.mjs --headed --mode summit \
  --root build/summit-webgl2/web/summit_web --output /tmp/kin-summit-smoke
xvfb-run -a node tests/webgl2_smoke.mjs --headed --deny-storage --mode summit \
  --root build/summit-webgl2/web/summit_web --output /tmp/kin-summit-denied
cmake --build build/summit-webgl2 --target summit_web_simulation_package --parallel 4
node tests/webgl2_smoke.mjs --mode simulation \
  --root build/summit-webgl2/web/summit_web_simulation --output /tmp/kin-summit-simulation
```

The game check verifies real assets/skinning/shadows, keyboard-driven physics
movement, pointer lock/reacquire, audio activation, settings reload, fullscreen
and canvas resize. `--deny-storage` uses Chrome's cookie/storage policy to test
actual IndexedDB refusal and in-memory fallback. The separate simulation export
runs the existing native Summit and expedition suites in Wasm, including climb,
fall/ragdoll/recovery, camp respawn/reset, save/replay and complete direct/sheltered
expeditions on compact/dense levels. It makes no cross-platform bit-identity
claim. All exports run in the experimental CI workflow.

For a bounded local performance sample, add `--profile` to the Summit check.
It records up to 1,200 game callbacks, including the first gameplay callback and
scene/UI spans, in `frame-samples.json` and summarizes them in `result.json`.
The default still uses SwiftShader; these times include main-thread stalls and
are not a hardware GPU benchmark or a measurement of displayed FPS. On a real
X11 desktop, `--headed --hardware --profile` lets Chrome select its normal GPU;
inspect the reported renderer string because it may still select software.
Keep resolution, look, route and device fixed when comparing builds.
For hotspot discovery, add `--cpu-profile` to save the walking segment as
`gameplay.cpuprofile`, readable in Chrome DevTools' JavaScript Profiler.
Each build emits `bin/<target>.html.symbols` alongside its Wasm; CI retains this
sidecar separately from the itch.io ZIP. Preserve the exact matching Wasm and
symbol map: rebuilding can reorder functions, so another build's map is unsafe.
Add `--gl-calls` with `--profile` to record selected WebGL API counts per callback
and their gameplay medians. Run these diagnostics separately from timing runs:
CPU sampling and API wrappers add overhead. Neither measures GPU execution.
`Module.kinSummit` exposes `fillMs`, `renderMs`, `uiMs`, `uiWarmupMs` and
`hudPrepared` for hosted diagnostics. These spans are CPU wall times, not GPU
queries. Font preparation adds startup work; it does not remove that work.

For a matched native/browser comparison on an NVIDIA Linux machine, build both
Release targets, stop competing GPU applications and use an isolated X11
display. The harness keeps the same 640x360 resolution, compact level, font,
seed, frozen morning look, null audio and real-time sheltered bot route:

```sh
xvfb-run -a python3 examples/summit/compare_web_native.py \
  --native build-3d/bin/summit \
  --web-root build/summit-webgl2/web/summit_web \
  --output /disk/backed/scratch/summit-native-web --pairs 2
```

The harness requires `nvidia-smi`, Chrome and Node 22+. It selects the NVIDIA
Vulkan ICD for native and ANGLE/Vulkan for Chrome, rejects software WebGL, and
records the actual renderer. It alternates full traversals, checks completion,
falls/deaths, skinning and cascades, and writes CSVs, browser callback samples,
GPU process logs, hashes and JSON summaries. Native is capped at 60 Hz by
`--fps 60`; Chrome retains normal rAF pacing. Check measured intervals before
assuming those ceilings match. These ceiling-limited runs compare pacing and
CPU headroom, not maximum FPS.

The default browser `--window app` uses a bounded Chrome app window and a
viewport matching the game dimensions, while retaining the iframe. The parent
has no scrollbars. Reports record both viewport and native browser-window
bounds. Matching only the game's WebGL buffer while leaving a larger browser
viewport can distort presentation costs on a virtual display. Use
`--browser-window browser` in the comparison harness, or `--window browser` in
the browser runner, to reproduce that larger-window diagnostic.

Use `--browser-mode offscreen` to separate browser rendering capacity from
window presentation. This selects headless Chrome while retaining the hardware
WebGL renderer, iframe, render resolution and full game route. Its synthetic
rAF clock does not measure physical display pacing or itch.io FPS. Reports label
the mode explicitly. Chrome uses its default compositor; forcing the Vulkan
compositor is an optional `--vulkan-compositor` diagnostic in the browser runner,
and failed context initialization in headless mode on the tested NVIDIA driver.

For a presentation trace, use a short browser-only run:

```sh
xvfb-run -a node tests/webgl2_benchmark.mjs \
  --root build/summit-webgl2/web/summit_web \
  --output /tmp/summit-pacing --steps 900 --trace
python3 tools/analyze_webgl_pacing.py /tmp/summit-pacing/browser-trace.json.gz \
  --frames /tmp/summit-pacing/frames.json --output /tmp/summit-pacing/trace-summary.json
```

The compressed JSON trace records Chromium renderer/compositor/GPU stages and frame-skipping
reasons; callback samples also record dispatch delay relative to the rAF clock.
Tracing adds overhead, so use separate untraced runs for comparisons. Advanced
controls `--minimal-context` and `--disable-gpu-vsync` are diagnostic overrides,
not game optimizations or required browser launch flags for players.
The browser runner also accepts `--cpu-profile` to save `gameplay.cpuprofile`,
and `--gl-calls` to record selected API counts and buffer upload bytes in
`frames.json`. Keep these diagnostics separate from timing comparisons and
retain the exact build's symbol map when inspecting Wasm samples.

Separate 900-step diagnostic runs enable WebGL timer queries and native GPU
fence timing. Those APIs measure different scopes; do not interpret their
ratio as a GPU speedup. CPU render-path spans sum scene preparation, 3D
submission and UI; they exclude simulation updates and final presentation/flush.
The browser startup span starts in Wasm main and excludes download/compilation.
The script refuses competing GPU applications by default. `--allow-contention`
keeps explicitly labeled diagnostic runs with the background activity recorded;
they do not establish an uncontended performance baseline.

`tests/webgl2_benchmark.mjs` can run the browser half independently;
`--software --steps 360` exercises the export bridge in CI without a hardware-speed claim.
This is a local iframe comparison; hosted itch.io and other browsers/devices
still need separate measurements.

## Local performance reference

Recorded on 2026-10-24 with an RTX 4080 Laptop GPU (driver 580.178.04),
Chrome/ANGLE Vulkan and native SDL_GPU Vulkan. Six complete traversals alternated
before/after instance reuse and fresh native runs, with all builds and diagnostic
instrumentation idle. Both the browser app window and viewport matched the
640x360 game buffer; a larger browser viewport distorted earlier presentation
measurements on Xvfb.

| Instance reuse comparison | Median render CPU spans | p95 | Mean 3D submission |
|---|---:|---:|---:|
| WebGL before instance reuse | 0.90 ms | 2.00 ms | 0.483 ms |
| WebGL with instance reuse | 0.80 ms | 1.60 ms | 0.366 ms |
| Fresh native reference | 0.421 ms | 0.613 ms | 0.191 ms |

Each browser result pools 7,272 measured frames; native pools 7,270. Browser
median 3D instance record uploads fell from 31,740 to 360 bytes per frame, with
42,000 bytes median retained instance buffer payload. Both browser runs per
build completed the same route with the same state hash and no competing GPU
workload observed. Mean 3D submission fell 24%; total median CPU spans fell 11%.
Browser timers resolve 0.1 ms, and scene/UI spans also vary between runs; the
comparison does not establish a maximum-FPS or hosted itch.io speedup. These
results precede the source cleanup for merge; regenerate them with the harness
above for a new binary. Local raw results and exact symbol maps are kept under
`out/webgl2/instances-20261024/`; generated reports and binaries are not tracked.
Earlier uniform caching, indexed 2D geometry and analytic HUD work reduced the
browser median from 1.3 to 0.9 ms in separate runs.

Native `kin-platform` tests caller-driven frame accumulation, pause, overload
and restart. `kin-serial-jobs` tests deferred execution under caller locks,
callback order, nested waits, cancellation, exceptions and queue destruction.

`kin-owned-scene-session` checks owned config/services across frames, scene
replacement, repeated finish, exit/shutdown exceptions and cooperative asset
completion. Local native regression runs pass 67/67 tests in the minimal profile and 76/76
in the full 2D software profile, including profiling, scripting and server mode.
