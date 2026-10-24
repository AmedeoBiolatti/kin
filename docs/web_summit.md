# Summit browser compatibility inventory

Recorded against main `b620b00` and the work on `experimental/webgl2`, 2026-10-24.
Summit is the first playable 3D target; `webgl2_3d_fixture` is the preceding
rendering gate. See [the staged plan](web_plan.md) and [build instructions](web.md).

## Existing game and required browser behavior

| Existing path | Native default/use | Browser boundary |
|---|---|---|
| Scene lifecycle | `SummitScene` through `run_scene_app` | Implemented: owned scene/session with completion/unavailable results retained outside the scene; browser navigation tears it down before the canvas disappears. |
| View | `SummitView::view_options` forces HDR, look exposure and bloom; main defaults to 4 samples | Required: explicit SDR profile and 1 sample. HDR/bloom and higher MSAA are optional and must not be forced by the look. |
| Camera and geometry | Orbit/first-person cameras, static/instanced terrain, props and the survivor | Required: shared camera conventions, reverse-Z depth, correct winding and normal transforms, CPU visibility and ordinary LOD. The first fixture covers the basic path. |
| Terrain materials | Rock030 and Ground037: two sets of color JPG, OpenGL normal JPG and ORM PNG, all 1024×1024; repeat/mipmapped sampling | Required: image decode, color/data distinction, normal maps, metallic/roughness and AO. Implemented with native BRDF math and pixel checks for color/data channels and normal orientation. |
| Prop materials | Imported glTF meshes/atlas, material factors, cutout/blended and double-sided surfaces | Required: preserve assets and material alpha modes. Exercise transparent ordering/depth independently of the opaque fixture. |
| Survivor | `kenney-character/survivor.glb`, authored animation/animator assets; uploaded when skinning is available | Required for the planned browser product: basic joint palettes and existing CPU animation. Implemented with per-instance RGBA32F joint palettes and actual pose-update tests; missing bundled characters fail startup. |
| Sun/ambient | Time-of-day lighting; procedural atmosphere supplies sky lighting | Required: directional/ambient material lighting with a documented core-quality sky. Prefiltered IBL is optional. |
| Shadows | Bounds-fitted sun shadow, 2048-pixel resolution, four cascades, 64-unit distance | Implemented: browser Summit uses two 1024px cascades; provider supports up to four. Larger maps and extra cascades are optional quality. Keep the player, terrain and props in the same coordinate/depth conventions. |
| Atmosphere | Procedural sky, height/distance fog, changing time and storm settings | Required: a recognizable, readable core-quality environment. Advanced scattering quality is optional. |
| Camp and weather FX | Fire/smoke/sparks, breath/dust and weather particles; camp local lights | Optional visual budget after the baseline. Preserve gameplay meaning through existing HUD/state even where richer effects are unavailable. |
| Advanced GPU work | Native compute effects, cluster/indirect infrastructure | Deferred: compute GTAO/contact shadows/TAA, Hi-Z, GPU cluster cut and indirect draws. Use ordinary meshes and CPU preparation. |
| Physics | `GameConfig::single_threaded` already defaults to true and is copied into `World3DConfig` | Required: retain this explicit path, cross-compile pinned Jolt, verify gameplay/script outcomes without worker waits. Cooperative Kin jobs alone do not validate Jolt. |
| UI and fonts | Existing HUD/menu, settings, shared example font code | Required: same UI over the 3D view and a bundled font path. The render fixture's bars are not Summit UI acceptance. |
| Input | Keyboard/mouse, relative mouse capture, optional gamepads | Required: gesture-driven pointer lock, Escape/reacquire, focus handling, movement and climb controls. Desktop keyboard/mouse first. |
| Audio | `SummitAudio`, volume settings and game sound cues | Required for hosted acceptance: initialize/resume after a user gesture and test mute/volume. Do not infer browser support from native playback. |
| Settings | `SaveStore`, native platform directory | Required: restore persistent browser storage before loading settings and deliberately flush writes. Continue with defaults if storage is denied. |
| Content | Bundled GLBs, atlases, images, animations, scene/prefab/collider data and licenses | Required: preload actual bundled assets and include notices. Do not substitute the procedural-asset mode to claim bundled-asset acceptance. |
| Host page | Current desktop executable/pack | Required: ZIP-root `index.html` and browser assets, actual itch.io iframe, pointer lock/fullscreen, resize, reload, focus and persistence. Local iframe testing closes only the local gate. |

The inventory follows `examples/summit/{main,view,game,external_assets}.cpp`,
`game.hpp` and the bundled `content/` directory. It describes the implementation
in this checkout, not browser portability inferred from API names.

## Reference gates

1. Preserve Hex's keyboard/path/movement regression in the shared browser runtime.
2. Require a real 3D provider and successful mesh uploads. In the small fixture,
   draw the far textured quad after a nearer opaque quad; check the near color,
   the texture's top/bottom colors, untouched transparent background and nested
   2D targets. Check submitted/culled instance counts, two interleaved views,
   keyboard camera rotation, resize and clean session shutdown.
3. Add focused material-map, alpha-sort, nonuniform transform, perspective clip,
   LOD, skinning and shadow reference cases as those paths land. Record browser
   limits and compare against the native references with explicit tolerances.
4. Load Summit's real terrain, props and survivor. Verify walking, climbing,
   falling, reset and expedition completion with the existing simulation.
5. Measure a named hardware device before setting download/startup/frame/memory
   budgets. SwiftShader CI is a correctness gate, not that performance baseline.

## Core profile implementation

The Summit preset now compiles the same scene/simulation/view/HUD/audio sources
as native. It preloads the full bundled content plus a licensed UI font, mounts
IDBFS before settings load, flushes saves deliberately and falls back to memory
when storage is refused. The player, terrain and props use shared camera/depth
conventions. Jolt selects its serial path whenever Kin has no worker threads,
including the default world configuration.

The material/animation/environment gate includes actual-pixel references for
PBR maps, cutout/blend behavior, independent skinned copies and pose changes,
CPU-reference sky/fog and moving/cutout shadow casters. The browser integration
checks actual loaded assets, physics movement, pointer lock/reacquire, audio,
settings reload, storage denial, fullscreen and resize. A separate browser test
export runs the existing native simulation and expedition suites. These checks
use Chrome/SwiftShader and are not hardware performance measurements.

Known quality differences: SDR clipping rather than HDR exposure/ACES/bloom,
one sample, compact scenery by default, reduced shadow budget, CPU hemisphere sky ambient rather than
prefiltered IBL, first 16 point lights, no stars/spot/local shadows/particles.
Bind-pose culling is bypassed for animated actors until conservative posed bounds
are available. Compute effects, picking, custom material shaders, touch/mobile
and actual itch.io-hosted acceptance remain follow-up work.
