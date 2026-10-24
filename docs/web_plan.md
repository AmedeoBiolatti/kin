# WebGL2 games: 2D and 3D plan

Status: core implementation complete, 2026-10-24; hosted/browser coverage remains in M6.
The architecture below records the original staged plan; implemented behavior and
limits are maintained in [web builds](web.md).
First playable 3D target: **Summit**, selected by the user. A small rendering
fixture precedes it. Existing native rendering remains supported throughout.

## Goal and current state

Ship Kin games as self-contained HTML/Wasm ZIPs playable in an itch.io iframe,
with one browser runtime supporting 2D frames, 3D views and 2D UI over those views.
Keep gameplay, scene data and simulation APIs shared with native builds.

The initial branch already builds and packages `hex_demo` with WebGL2 sprites,
geometry, an embedded font, caller-driven app frames and cooperative Kin jobs.
Chrome iframe checks cover input, picking a hex, movement, fullscreen and resize.
Hex now uses the shared owned scene runtime. A second export renders real
WebGL2 3D meshes into two views alongside 2D targets/HUD; pixel checks cover depth,
texture orientation and transparency, and the browser check covers keyboard,
view resize and shutdown. Native regression runs pass 67/67 minimal and 76/76 full 2D software tests. These
local results do not establish hardware GPU performance or an itch.io deployment.

The browser renderer now supports Summit's stock PBR maps, animated joint
palettes, procedural sky/fog and directional cascaded shadows using core WebGL2.
The Summit target shares the native scene and simulation, loads its actual
bundled content/font, uses serial Jolt, and integrates browser audio and IDBFS
settings. The rendering fixture has pixel checks for these paths. General 2D
shader effects, custom shader assets/reflection and advanced 3D fidelity remain
follow-up work. See [build instructions](web.md) and the
[Summit compatibility inventory](web_summit.md).

## Browser contract

- Require WebGL2 and GLSL ES 3.00. Keep the default export single-threaded and
  usable without SharedArrayBuffer or COOP/COEP. Desktop browsers with keyboard
  and mouse are the first acceptance target; touch/mobile gets a separate gate.
- Require only core WebGL2 for the first playable Summit path. Use RGBA8 color,
  depth targets, conventional vertex/fragment passes and CPU visibility work.
- Detect optional extensions and actual limits at runtime. HDR is conditional
  on working float color targets; MSAA sample counts are queried per format.
  Default quality settings must run without either optional feature.
- Keep capabilities accurate. A background-only 3D fallback, an invisible mesh
  or a swallowed shader error never counts as a passing browser game.
- Offer an explicit reduced rendering profile. Shader storage buffers, compute
  and GPU-generated indirect draws are outside the core WebGL2 contract.

WebGL2 follows OpenGL ES 3.0; its resources and limits are defined by the
[Khronos specification](https://registry.khronos.org/webgl/specs/latest/2.0/),
which is a living editor's draft. Float renderability is provided separately by
[EXT_color_buffer_float](https://registry.khronos.org/webgl/extensions/EXT_color_buffer_float/).
Neither the experimental WebGL compute proposal nor optional extensions become
baseline requirements.

## Architecture and known changes

### Shared runtime and build selection

Extract an owned scene application/session from `run_scene_app` and
`run_windowed_app`. Native `run` drives it synchronously; a browser animation
callback drives its frame operation. Own config, game metadata, scenes, assets,
callbacks and shutdown state across frames, with scene resources destroyed before
the renderer. Reuse it for Hex, Signal Siege and Summit. Keep `App::step_loop`
as the shared fixed-step/input implementation already added on this branch.

The render-only build now separates SDL_GPU and WebGL2 providers and permits
`KIN_ENABLE_PHYSICS3D=OFF` / `KIN_ENABLE_SCENE3D_RUNTIME=OFF` without Lua or Jolt.
Native defaults retain the complete feature set. The coupled 3D ECS/scene
adapters are omitted with physics in this first separation.

### One GL device for both renderers

Extract context/resource ownership from the current WebGL2 2D backend into an
internal WebGL device shared with a new `IRendererBackend3D` implementation.
Continue using `Renderer3D`, `Scene3D`, `IViewBackend3D`, `Mesh3D` and `Texture`.
A 3D view renders offscreen and exposes its color as a regular Kin texture.

Add 2D render targets before 3D view composition. Flush pending 2D draws when
entering a 3D pass and restore GL state before subsequent UI: framebuffer,
viewport, program, vertex array, textures, blend, depth, culling and scissor.
Test interleaved 2D/3D views, not just a full-screen mesh with no HUD.

Share portable CPU camera/frustum, LOD and draw preparation code where needed.
Some CPU helpers currently live inside `gpu_renderer3d.cpp`; extract focused
pieces as the WebGL implementation consumes them.

### Shader assets and data layouts

Today 2D shader creation and built-ins are gated by `KIN_HAS_GPU_RENDERER`, and
3D custom shader creation takes SPIR-V bytes. Introduce target-aware shader
assets containing native SPIR-V or GLSL ES source plus binding/uniform metadata.
Keep existing native entry points compatible and make shader capability
independent of whether SDL_GPU was compiled.

Keep shared lighting/material calculations, with explicit backend variants for
resource declarations and vertex inputs. Native instance and joint shaders use
`std430` storage buffers, so changing the shader version alone cannot port them.

| Data or operation | WebGL2 baseline |
|---|---|
| Camera/material constants | Bounded uniform blocks, sized from queried limits |
| Visible instances | CPU-compacted instanced attributes; bounded uniform batches where necessary |
| Joint matrices | Vertex-readable data textures with nearest `texelFetch`; explicit palette/offset limits |
| Frustum and ordinary mesh LOD | CPU preparation; upload only visible draws |
| GPU cluster cut / indirect arguments | Disabled initially; ordinary mesh/CPU paths |
| Native shader buffer inputs | Portable uniform/texture variant or an actionable unsupported-feature diagnostic |
| Environment prefiltering | Host-prepared data later; hemisphere/procedural sky in the baseline |

Joint data texture sampling does not require rendering into float targets.
Account for the minimum vertex attribute budget when combining base mesh,
skinning, transforms, normal matrices, colors and IDs.

Use a pinned host shader pipeline. Evaluate GLSL variants through glslc and
[SPIRV-Cross's ES output](https://github.com/KhronosGroup/SPIRV-Cross), including
reflection and explicit bindings. Translation does not supply missing storage
buffer or compute semantics. Fail unsupported variants during asset preparation;
perform actual browser compilation/link validation as well. Browser exports do
not launch a shader compiler or depend on hot-reloading source files.

Keep Kin's camera conventions. Adapt native depth/clip coordinates at the WebGL
boundary, and verify near/far clipping, face winding, texture/render-target Y
orientation, picking unprojection and shadow projection together. Define sRGB
decoding, linear lighting, presentation encoding and premultiplied-alpha view
composition explicitly.

### Physics, assets and platform services

Kin's cooperative job mode alone does not port Jolt. `World3D` already has a
`JobSystemSingleThreaded` path: select it explicitly for browser worlds. The
pooled `KinJobs` adapter has waits/spins that need an audit before any browser
use. Cross-compile the pinned Jolt release and test simulation separately from
rendering; preserve the current floating-point policy without claiming untested
cross-platform bit identity.

Start with host-preloaded loose content and RGBA textures. Reuse manifests,
reports, font subsets, model loading and cooking. Move expensive preprocessing
off startup where possible. Browser pack loading must own a bounded memory buffer
and cannot assume native mmap demand paging. Compressed texture formats remain
optional; retain a usable baseline asset variant.

Add gesture-activated audio, pointer lock/fullscreen, focus and background-tab
handling. Mount and restore persistent storage before loading settings, then
flush writes deliberately. Keep the game usable when browser storage is refused.

## Stages and acceptance gates

Stages are reviewable increments; each may need more than one implementation PR.
Work advances on passing gates rather than an assumed calendar estimate.

| Stage | Deliverable | Acceptance gate |
|---|---|---|
| M0: compatibility contract | A Summit feature inventory, small reference scenes, queried-limit report and preserved Hex baseline | Every Summit renderer option is classified as required, optional or deferred; native/browser baselines are recorded |
| M1: reusable browser runtime | Owned scene session, loading/lifecycle flow, and separated 3D build/provider selection | Hex uses the shared session; a render-only 3D target configures without SDL_GPU; desktop runtime tests stay green |
| M2: shared render targets and shaders | WebGL device ownership, 2D offscreen targets, view-compatible textures and target-aware shader assets | Textured offscreen content, clipping and alpha composite correctly under/over UI; supported shaders link in the browser and unsupported ones fail clearly |
| M3: first real 3D view | WebGL mesh/view backends, depth/culling, camera transforms, instancing, unlit and basic lit materials | A small textured scene with overlapping near/far meshes and instanced copies renders correctly inside a 2D HUD, resizes and releases resources cleanly |
| M4: Summit's minimum renderer | glTF/static meshes, material maps and alpha modes, normal/metallic/roughness lighting, basic skinning, CPU culling/LOD, directional shadow and procedural sky | Summit's bundled props and animated survivor render with correct depth, transparency, lighting and UI on the core-quality profile |
| M5: playable Summit | Shared scene runtime, serial Jolt, gameplay/input/camera, bundled images/fonts, audio activation and settings storage | Walk, climb, fall, reset and complete an expedition in the browser; same gameplay implementation and scripted checks as native, with supported quality differences documented |
| M6: itch.io release acceptance | Complete ZIP, browser/device coverage and measured budgets | Summit runs on an actual itch.io iframe, including audio, pointer lock, fullscreen, resize, focus, reload and settings persistence; both 2D and 3D exports pass regression checks |
| M7: optional fidelity | HDR/bloom, improved shadows, baked IBL, soft particles and selected additional materials/effects | Each feature has a detected capability, a tested fallback, measured cost and a game-driven reason to ship |

M2 benefits 2D games as well: use Signal Siege to exercise the shared runtime,
fonts, content, audio and shader/target path as those pieces land. Summit does
not wait for complete 2D effect parity. A minimal render fixture is a correctness
gate; it is not the first playable 3D product milestone.

### First playable Summit feature boundary

Required: ordinary mesh rendering, bundled textured assets, camera/depth,
stock material maps and alpha modes, directional/ambient light, a basic sun
shadow, skinning and existing animation evaluation, 2D HUD, single-threaded
physics, input, game flow and platform behavior needed to play.

Optional: extension-backed HDR and bloom, higher MSAA levels, larger shadow
budgets, advanced material variants, richer particles and prefiltered IBL.
Expose only the choices the current browser can honor.

Deferred: the existing compute GTAO/contact-shadow/TAA implementations,
compute-driven clusters/Hi-Z/indirect drawing, arbitrary storage-buffer shaders,
desktop-scale stress-scene throughput and mobile/touch acceptance. Fragment-pass
versions of selected effects can be considered later, independently of native
compute implementations. Desktop retains its existing rendering features.

Picking is implemented against core-compatible targets when consumed, with
object IDs and a tested world-position reconstruction. Keep synchronous
readback limited to explicit requests; avoid introducing a per-frame stall.

## Verification and measurements

- Keep the existing Hex browser smoke and native tests passing at every stage.
  Add focused render cases for depth, winding, transforms, instancing, view
  texture orientation, alpha/color handling, UI order, skinning and shadows.
- Use scripted inputs and game outcomes to validate physics/gameplay. Compare
  browser and native images with stated tolerances; do not demand identical
  pixels from different rasterizers or assume simulation bit identity.
- Test with optional extensions deliberately unavailable and low resource
  limits. Exercise missing/invalid assets, shader diagnostics, scene replacement,
  repeated resize, shutdown and the context-loss reload path.
- CI Chrome/SwiftShader checks functionality. Add Firefox coverage and manual
  Safari/real-GPU acceptance before claiming broad browser support. Mobile
  remains a separately measured target.
- Use a named reference device and fixed scene/resolution for performance.
  Record compressed download, startup/first-playable time, peak Wasm memory,
  CPU frame-time distributions, draw counts and GPU timing when available.
  Set numeric budgets from M0/M3 measurements; software CI is not a GPU benchmark.
- Verify ZIP root layout and bundled notices. Test the actual hosted iframe;
  localhost results alone do not close M6.

## Current implementation checkpoint

- M0: Summit defaults and asset inventory recorded; browser limits are included
  in smoke reports. A matched core-quality RTX 4080 native/WebGL traversal is
  recorded in `web.md`; hosted/device-wide performance budgets remain open.
- M1: owned sessions and Hex migration implemented; render-only 3D builds
  configure without SDL_GPU/Lua/Jolt. Native runtime regressions are checked.
- M2: shared device, nested 2D targets and 3D view textures implemented. The
  fixture's embedded GLSL ES shaders link in the browser. General shader assets,
  material reflection and 2D effects are still pending.
- M3: first real 3D view implemented and checked in Chrome/SwiftShader. CPU
  culling, instanced copies, depth, unlit/basic lit textures, alpha composition,
  resize and clean quit are covered. Broader lighting/transform/LOD reference
  cases and real-GPU/browser coverage remain open.
- M4: implemented and checked with material/skinning/sky/fog/shadow pixel
  references and Summit's real assets. Core quality uses SDR, hemisphere sky
  lighting and two 1024px shadow cascades. Native HDR/IBL/particles remain outside
  this profile.
- M5: Summit export and browser platform services implemented. Local game
  integration (including storage denial) and both existing gameplay suites pass
  in Chrome/SwiftShader. Compact scenery is the browser default;
  the simulation checks cover compact and dense expeditions. JSON results and
  logs accompany the bundle. Browser/device breadth remains open.
- M6: ZIP packaging and CI implemented. Hosted Summit boot/gameplay were
  confirmed by the user and a DevTools trace. The optimized build needs a fresh
  hosted trace; full hosted acceptance, GPU budgets and Firefox/Safari coverage
  remain pending.
- M7: pending; each fidelity feature still needs an explicit capability/fallback
  and a game-driven acceptance case.
