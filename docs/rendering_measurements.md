# Rendering foundations validation

The implementation branch starts at lighting snapshot `77b76c8`. Correctness, CPU tracking, memory accounting, public graphs and stock-phase scheduling are delivered in separate commits through `e1226c2`. Main's planning edits and the lighting worktree's uncommitted GI subvoxel changes were preserved. The [progress log](rendering_progress.md) distinguishes this delivery from the full [pipeline plan](rendering_pipeline_plan.md).

## CPU work

Release/GCC 15.2; four transform barriers plus scene clear/collection, 10,000 and 100,000 flat entities. The table is the median of five exploratory repeats, each with 50 warm-up and 150 measured frames. It excludes GPU rendering, physics, animation and arbitrary placement callbacks. Thermal/background conditions were not controlled sufficiently for a release performance claim.

| 100,000 entities | Static p50 | 1-percent-edit p50 | Transform visits/frame | Extraction visits/frame |
| --- | ---: | ---: | ---: | ---: |
| Original binary | 10.00 ms | 10.05 ms | Original binary had no counters | Original binary had no counters |
| Conservative compatibility | 9.39 ms | 9.48 ms | 400,000 | 100,000 |
| Tracked mutations | 0.0009 ms | 0.232 ms | 0 / 1,000 | 0 |

The sparse case updates 1,000 retained records. Static timing measures cached snapshot submission, not traversal of 100,000 objects. Tracking metadata is about 25.5 MiB at this scale, excluding allocator overhead and the existing retained instance data. Static models use part-index bindings rather than duplicate local matrices. One later probe after model-path integration measured 0.235 ms for sparse edits, consistent with the exploratory range.

A comparison cache in the raw-write path regressed to about 13 ms and was rejected. The compatibility path keeps tight owned/shared table loops; tracked mode carries the incremental metadata. Arbitrary mutable pointers require compatibility scans or explicit notification/invalidation. Animation, lighting/particle extraction and placement callbacks still have conservative collection work.

Raw benchmark CSVs are under `/tmp/kin-foundations-cpu-{before-current,verified,models}.csv` in this session. Reproduce with `kin_scene_work_bench --warmup 300 --frames 1500 --runs 5`; retain a baseline executable and alternate processes for consequential tuning.

## Functional and resource gates

- The complete native renderer dispatch passes in one process, including culling, cluster pressure, materials, shadows, GI, temporal rendering, particles, skinning, animation, physics interactions, retained caches and compressed imports.
- Focused hardware/software Vulkan tests cover RGB emission, 108 imported sampler combinations, independent per-map sampling, transparent mesh/particle ordering, flat normal maps, complete budget fallback and GI texture updates.
- A two-view regression matches the original fixed phase order exactly with HDR, TAA, AO, contact/sun shadows and picking. The stock graph currently schedules thirteen coarse operations; inner passes and their allocations remain backend-owned.
- CPU tests cover static zero visits, sparse edits, descendants, reparenting, raw and derived-transform edits, shared prefab sources, model parts, material resolver changes, disabled-emitter validation and deferred observer destruction.
- Public graph tests validate cycles, missing/disabled dependencies, initialization, feedback, required features, culling, lifetime compatibility, cache invalidation, passthrough imports and payload admission limits. The compiler also builds/tests without experimental 3D.
- `kin_ascii3d --test` uses runtime checks in Release. Optimized/reference pictures match, four logical targets use three physical slots, stable frames reuse plans/handles, and HDR working-format changes are validated. Hardware Vulkan and software Vulkan both pass.
- Memory tests cover block/mip/layer/sample payload, release accounting, 100 pooled-image reuses, 100 target resize cycles, a stable logical plateau and late texture/buffer destruction. Descriptor payload excludes hidden SDL cycling, pending retirement and driver allocations. Longer feature/multiview/reload stress and external residency measurement remain in the plan.
- WebGL2 rebuild and Chromium/SwiftShader smoke pass with Emscripten 6.0.13: materials, skinning, sky, fog, shadows, input, resizing and fullscreen. Native custom shaders/compute remain capability-gated; this does not claim browser ASCII support.

Hardware: RTX 4080 Laptop GPU, Vulkan, NVIDIA driver 580.178.04. Software coverage uses Mesa lavapipe. Windows/D3D12 and Metal were not exercised. Native build-only scripting flags suppress an existing sol2/GCC 15 template diagnostic; repository/dependency sources were not patched for it.

## Quality candidates

The committed capture is identified by `kin-quality-foundations-v1/manifest.json` under the sibling worktree directory. It records commit `e1226c2`, binary/source/content hashes, renderer compile commands, CMake cache identity, candidate configuration, hardware/driver telemetry, commands and logs. The six scenes are Cornell, furnace, spheres, shadows, Flight Helmet and Sponza. Five candidates compare baseline, anisotropy 4, specular AA 0.25, and TAA with sharpness 0/0.12. Authored nearest/no-mip samplers remain authoritative.

This is a diagnostic 320×180 still-image run, one candidate sweep, 32 warm-up and 64 measured frames. It validates the harness and exposes candidate effects; it does not meet the longer performance or moving-camera acceptance protocol. Requested and effective settings are reported separately, including eligible-map filtering limits, quantized mip bias, active TAA and main-pass samples.

Independently seeded Cycles references use Blender 5.2.2/OptiX, 1,024 samples, no denoising/adaptive sampling/clamping, and the same recorded cameras. Seed-to-seed results are the comparison floor. Metrics here are PSNR/SSIM/level differences from the existing tool, not the planned FLIP/JOD and moving-camera suite. GPU fence timing at this small workload is not a reliable throughput measurement.

No engine defaults or named quality presets were selected from these diagnostic stills. Final calibration needs moving camera paths, calibrated perceptual/display metrics, larger target resolutions, memory/performance runs, and a stable integration of the lighting worktree's current GI changes. The stock token graph also needs finer geometry/shadow/resource operations before the remaining custom-shadow and finite-propagation examples can use them.


### Still-image results

The table shows PSNR against seed 0; the floor compares the two Cycles seeds. Higher is closer for this metric. The TAA column includes anisotropy 4, specular AA 0.25 and sharpness 0.12. GI uses the same settings and a matched 600-frame warm-up, 64 rays/probe and 4,096 probes updated per render. The shadow-only TAA case uses the initial 32-frame warm-up.

| View | Truth floor | Baseline | TAA candidate | Settled GI candidate |
| --- | ---: | ---: | ---: | ---: |
| cornell/front | 43.21 | 22.53 | 22.56 | 29.77 |
| helmet/front | 56.77 | 33.83 | 37.22 | 37.24 |
| helmet/side | 58.51 | 34.98 | 37.41 | 37.30 |
| shadows/fence | 54.53 | 31.43 | 32.59 | — |
| sponza/nave | 42.01 | 13.43 | 13.50 | 30.11 |
| sponza/roof | 43.66 | 15.93 | 16.03 | 27.90 |

The main finding is lighting: the Sponza nave's mean signed error falls from about +49 levels to −1.3 with settled GI. Cornell also improves substantially when the emissive-panel GI replaces its point-light stand-in. These are the committed lighting snapshot's results; they do not evaluate the active worktree's finer GI occupancy.

TAA improves static helmet/shadow edges. In the initial sweep, unsharpened TAA scores slightly better than sharpness 0.12 in these stills (helmet front 37.41 versus 37.20 dB). Specular AA alone has mixed results, and filtering alone changes whole-image scores little at this size. None of this decides moving-image detail, ghosting, convergence after edits or a universal preset.

### Matched memory observation

The GI/control captures use identical binary/source hashes, the same three scenes, order, filtering/TAA and 600-frame warm-up. Idle sampled-texture pool payload is zero at the reported settled points. These are owned descriptor payloads, not measured physical VRAM.

| Scene | No GI | GI | Added owned payload |
| --- | ---: | ---: | ---: |
| cornell | 9.09 MiB | 272.39 MiB | 263.30 MiB |
| helmet | 299.80 MiB | 477.86 MiB | 178.06 MiB |
| sponza | 397.09 MiB | 524.88 MiB | 127.79 MiB |

GI's memory cost depends on voxel/probe dimensions and scene bounds. The initial six-scene baseline also reached about 861 MiB peak owned payload, with roughly 252 MiB idle sampled textures and 65 MiB transfer capacity at one Sponza capture. Comparing unmatched run peaks would confuse pool expiry and staging with GI's cost. The next allocator experiments should separate active feature data, idle retention and large-upload staging before changing defaults.

Full PNGs, both truth seeds, manifests, comparison HTML/Markdown and `scores.json` remain in sibling artifact directories `kin-quality-foundations-v1`, `kin-quality-foundations-gi-v1` and `kin-quality-foundations-no-gi-v1`. Source/content identities are preserved there; the report commits no large image assets.

## Standalone geometry migration

The standalone depth/normal operation and stock shared recorder preserve all four RGBA captures from predecessor `5944084`: Cornell/front and the motion scene's still, pan and close cameras. They are byte-identical at 320 x 240 with AO, contact shadows and TAA enabled. Reproduce against a saved predecessor executable with:

```sh
kin_reference tests/reference/scenes/cornell.json tests/reference/scenes/motion.json \
  --out OUTPUT --width 320 --height 240 --frames 4 --warmup 4 --repeat 1 \
  --ao --contact-shadows --taa
```

Session captures are `/tmp/kin-geometry-before` and `/tmp/kin-geometry-after`; comparison checks decoded RGBA bytes. This is a correctness comparison, not a performance benchmark. The outline example measures stable GPU handle allocation during ordinary recording; readback is outside that interval because it allocates transfer resources. Driver cycling/backing storage remains outside Kin's payload ledger.

### Separate device-lifecycle finding

Two live SDL_GPU renderers on this NVIDIA 580.178.04/X11 system crash during window release. The debugger stack enters `VULKAN_ReleaseWindow` and NVIDIA's `libnvidia-glcore`; the failing call is outside geometry recording. A separate executable reproduces it with only `App`, two windows, two ordinary `Renderer2D` objects, clear/present and scope exit—no `Renderer3D`, graph or geometry operation. Initializing/presenting both swapchains does not resolve it. The root cause is not established, and this change does not repair the broader device/window lifecycle.

The foreign-device geometry test is separate from ordinary geometry acceptance. Software Vulkan exercises live foreign targets, meshes and cutout textures. Hardware runs skip this lifecycle probe by default; `KIN_TEST_LIVE_GPU_DEVICES=1 kin_render3d_geometry_tests --foreign-device` opts in for diagnosis/platform validation. Core geometry, graph reuse, resize/HDR lifetime and interleaved stock-history checks run on hardware normally.

Minimal reproduction inside an application entry point:

```cpp
App app;
auto& first = app.create_window({.width = 160, .height = 120, .hidden = true});
auto& second = app.create_window({.width = 160, .height = 120, .hidden = true});
Renderer2D a{make_render_backend(first, false, true)};
Renderer2D b{make_render_backend(second, false, true)};
a.clear(colors::black); a.present();
b.clear(colors::black); b.present();
// Crash on scope exit on the tested NVIDIA/X11 configuration.
```

The completed native renderer dispatch is logged at `/tmp/kin-geometry-full-renderer.log`. Final focused results are `/tmp/kin-geometry-final-hardware-ctest.log` (ten passes, one documented lifecycle skip) and `/tmp/kin-geometry-final-software-ctest.log` (six passes, including live foreign-device rejection). Web smoke output is `/tmp/kin-geometry-web-verified-smoke.log`. These include projection/depth, signed/flat normals, cutouts, skinning, retained batches, CPU/GPU LOD cuts, target validation/feedback, allocation/plan reuse, resize/HDR lifetime and exact stock-history interleaving. The native full suite was run once after the implementation changes; subsequent edits separated the ownership test's device-lifecycle probe.

## Shared draw preparation

The frame-scoped preparation slice builds packed records, conservative world/posed bounds and affine LOD scales once, then shares them between geometry and stock consumers. Exact source-row maps and GPU write generations admit upload reuse; camera/shadow visibility, LOD, composition and previous-frame data remain per consumer. Current palettes and local-shadow unions can reuse their buffers. Identical camera/local unions share an input; one recycled spare avoids per-frame handle allocation when a writer detaches.

A 10,000-copy functional workload verifies a 1,280,000-byte initial instance upload followed by zero repeated instance-upload bytes and zero bound transformations in a compatible consumer. Reuse allocates no GPU handles. Local-shadow recording also allocates no new GPU handles over four measured frames after three warm-up frames. The checks exclude readback from the allocation interval. These are operation/byte gates, not a claim about frame-time speedup.

Preparation costs 148 bytes per submitted instance for packed records/bounds/scales, plus metadata and compact row maps. Default CPU payload admission is 32 MiB per handle. Current handle payload and renderer mapping capacity are reported separately. The limit excludes existing scene/assets and allocator overhead. Preparation builds even subsequently culled copies, so single-pass and retained-only workloads may be cheaper through the existing streaming/incremental paths. Consequential tuning should compare total CPU time including preparation, separately from consumer recording, over repeated runs.

Prepared-vs-streamed pixel checks cover separate cameras, resize, off-camera sun casters, local shadows, posed meshes, TAA, CPU/GPU cluster cuts, whole-mesh LOD, transparent composition and overlays. Lifetime checks cover edits, batch/pose revisions, mutable-span notifications, moves/destruction, frame/readback boundaries, cross-device inputs and renderer destruction. Interleaved ordinary uploads invalidate reuse proofs. The separate live-foreign-device probe keeps its documented hardware opt-in policy.

Four saved stock RGBA captures from predecessor `35518ab` remain byte-identical: Cornell/front and motion/still, pan and close, at 320 x 240 with AO, contact shadows and TAA. Session artifacts are `/tmp/kin-shared-prep-before` and `/tmp/kin-shared-prep-after`, using the reference command above. The complete native renderer dispatch is `/tmp/kin-shared-prep-full-renderer.log`; focused hardware/software results are `/tmp/kin-shared-prep-final-{hardware,software}-ctest.log`.

The completed gates are eleven focused hardware passes plus the inherited device-lifecycle skip, seven software passes (including foreign prepared handles/views and renderer destruction), the full native dispatch, WebGL2 smoke (`/tmp/kin-shared-prep-web-smoke.log`), a 2D-only graph compile/run, and the 190-file formatter. A final palette-capacity admission guard was checked by the focused suites after the full native run; it does not alter the ordinary rendering path.


## Public shadow and surface operations

This slice adds explicit directional-shadow depth arrays and mesh shading into graph-owned colour/depth. It reuses the preceding prepared data; camera/caster selection and LOD remain consumer-specific. Shadow recording redraws its supplied target, while stock shadow caches/maps and per-view temporal histories retain their ownership. Surface shading runs without implicit local/fill shadows, sky, fog, particles, AO/GI, temporal resolve or view output. These are separate public operations still to migrate.

`kin_surface3d --test` checks exact optimized/reference pixels, cached plans, zero new GPU handle allocations over eight measured frames after warm-up, no CPU device wait, and retained data arrays across HDR changes. Readback is excluded from the allocation interval. No claim of frame-time speedup, native heap aliasing, driver backing-storage reuse or complete graph command compilation follows from these gates.

Four decoded RGBA captures remain byte-identical to saved predecessor `6b24761`: Cornell/front and motion/still, pan and close, at 320 x 240 with AO, contact shadows and TAA. Session captures are `/tmp/kin-public-pass-before` and `/tmp/kin-public-pass-after`, using the reference command above. The complete native stock dispatch is `/tmp/kin-public-pass-full-renderer.log`. Focused results are `/tmp/kin-public-pass-hardware-ctest.log` (twelve passes and the inherited NVIDIA/X11 live-device lifecycle skip) and `/tmp/kin-public-pass-software-ctest.log` (eight passes, including live foreign ownership).

The interleaving regression renders a different public shadow/surface scene between independent stock TAA/AO views, and separately verifies that warmed private sun-shadow cache hits survive public recording. GPU probes read surface depth and every unused/inactive array layer. Public shadow/surface images agree with stock mesh rendering for one/four cascades, fractional strength, flat/masked/skinned/retained geometry, CPU/GPU cluster cuts, transparent/overlay composition and unshadowed local lights. Validation covers expired/foreign handles, all material-map feedback slots, array import/type distinctions, nonfinite projections and payload overflow. Temporary maps/attachments released before submission remain valid through queued recording.

Code review found a stale stock particle-caster stream could be consumed by the new mesh-only public shadow operation. The regression fails at the unexpected particle-draw counter before the public-pass exclusion guard (`/tmp/kin-public-pass-particle-before.log`). Final focused suites include the fix, explicit ignored particle requests, and both hardware/software paths. The full stock dispatch preceded this public-only guard; its stock branch is unchanged by the guard. WebGL2 smoke is `/tmp/kin-public-pass-web-smoke.log`; unsupported array/shadow/surface operations return diagnostics while stock browser rendering remains functional. A standalone 2D-only graph compile/run and the 191-file format check also pass.


The extended sequence also crossed the existing adaptive upload-staging shrink window during the local-shadow allocation gate. Allocation tracing (`/tmp/kin-public-pass-allocation-trace.log`) identified one 2 MiB transfer buffer, rather than new geometry/shadow resources. That gate now subtracts the independently reported upload-staging allocation delta and also requires staging capacity not to grow. Geometry/local-shadow resources must still allocate zero handles. The standalone surface example continues to check zero total new handles over its measured interval.

## Local-shadow graphs and material light responses

This slice extends explicit native composition with up to eight spot maps and two complete six-face point cubes in a twenty-layer D32F array. Map selection is explicit; private stock importance/hysteresis, maps, cache versions and temporal histories are unchanged. All point faces are initialized and reusable for another camera. This may rasterize more geometry than the stock camera-pruned cube path. Local LOD/cluster selection uses the existing light-specific CPU path; this change does not introduce a new GPU local-cut algorithm or native heap aliasing.

`kin_render3d_local_graph_tests` compares stock and explicit surface pixels for mixed spots/points, changed layer locations, mixed viewport resolutions, masks, skinning, retained batches, CPU/GPU mesh-cut modes, transparency and overlays. It probes every point face, verifies off-camera caster contribution, compares light-grid output, checks private cache hits after public work, and interleaves public operations with eight frames of independent stock TAA/AO histories. Projection edits are demonstrably consumed. Historical maps and temporary output handles survive application-handle release before submission. The full 8-spot/2-point profile succeeds; excessive/overlapping/invalid layouts and bad projection/filter metadata are rejected.

An eight-frame warmed interval requires zero new geometry/local-shadow resource handles and no upload-staging growth; independent staging-maintenance allocations are accounted for as in the preceding slice. Repeated consumers use prepared world/posed bounds (`bounds_instance_tests == 0`). No explicit device-idle wait occurs in the measured consumers. These are correctness, operation and allocation gates, not a frame-time speedup claim. The standalone `kin_surface3d --test` also requires zero total new GPU handles in its measured interval, cached structural plans, exact optimized/reference pixels and compatible HDR transitions.

The optional material callback is tested against the stock response (at most one RGBA8 quantization step in the identity case), visibility bypass, light-kind channels, position/distance consistency and wrapped directional light. Custom shaders still have the existing fixed 16-point/16-spot blocks; stock light-grid rendering consumes explicit maps but this hook adds no custom storage/sampler bindings. A propagation solver still needs bounded scene/light history and explicit resource inputs.

Four decoded RGBA captures remain byte-identical to predecessor `1084397`: Cornell/front and motion/still, pan and close, at 320 x 240 with AO, contact shadows and TAA, using the reference command above. Captures are `/tmp/kin-local-pass-before` and `/tmp/kin-local-pass-after`. Full native dispatch is `/tmp/kin-local-pass-full-renderer.log`; focused results are `/tmp/kin-local-pass-hardware-ctest.log` (thirteen passes plus the inherited live-device lifecycle skip) and `/tmp/kin-local-pass-software-ctest.log` (nine passes including foreign map/preparation rejection). WebGL2 smoke is `/tmp/kin-local-pass-web-smoke.log`; unsupported local operations return diagnostics and the stock browser path passes. The standalone 2D-only graph compiler and 192-file format gate pass.

Final review added a first-use stock no-op regression: a very broad point light reaches the camera, but its short shadow range lies entirely outside it. All cube faces are pruned, so caster buffers have never been needed. The extracted recorder initially treated absent buffers as failure (`/tmp/kin-local-pass-noop-before.log`); checking whether any view needs recording first preserves successful no-op behavior. The full native dispatch preceded this final narrow guard; final focused suites exercise the guard, cached maps, active maps and public map recording.

## Material graph inputs

Native custom materials now carry one optional shared `MaterialInputs3D` handle, with shader-specific texture/buffer vectors owned by the binding set. The tested 64-bit layout adds a sixteen-byte shared handle to each material rather than fixed arrays of resource handles to every draw. Factory creation performs bounded reflection/type/device checks and creates no GPU resources. The profile is four extra sampled textures and eight read-only storage buffers; writable fragment resources, descriptor arrays, cube/3D/MSAA/signed-integer images and extra-input sampler overrides remain outside it. Validation visits draw descriptors and scans each distinct set once per consumer; it does not repeat transformed bounds or scan instance copies.

The compute → material → ASCII surface example checks exact optimized/reference pixels, cached plans, HDR compatibility and zero total new GPU handles in eight warmed frames. The material-input suite checks shared prepared consumers with zero repeated bound transformations, stock TAA reset on changed input contents, and a warmed interval with no new resource handles beyond independently counted staging maintenance and no staging-capacity growth. These are correctness/work/allocation gates. They do not establish a frame-time speedup, zero host allocations, native heap aliasing or driver backing-storage reuse.

Bindings retain physical allocations, not logical resource versions. Graph reads must include every material access; immutable binding sets must be recreated after graph physical reallocation or shader replacement. GPU compute producers stay ordered in-frame. CPU uploads after queued material reads submit previous reads before upload, without an explicit device-idle wait, and expire prepared handles. Prefer uploading before preparation or use a GPU producer for multi-consumer graphs. Changed buffer/texture content revisions and extra-texture scale state conservatively reset stock TAA; even an identical write may reset history. A propagation solver still needs bounded scene/light history and its own simulation policy.

Coverage includes float/unsigned inputs, raw/comparison depth arrays, the complete four-texture/eight-buffer budget, std430 matrix sizing, runtime-array minimum sizes, malformed descriptor layouts, missing/mismatched sets, short/foreign buffers, foreign shaders/textures, surface/view feedback, queued handle retention, CPU buffer/texture upload ordering, compute updates and observation/destruction after renderer lifetime. A pending 2D target clear is also consumed before the temporal revision check, preventing a one-frame-late history reset. The regression exposed a stale clear flag after an empty batch reset; write accounting now requires an actual batch target.

The unmodified full 2D renderer suite stops at `test_gpu_native_pixel_size_and_pointer_mapping`, where its logical-center assertion also fails in the pre-existing October 6 binary (`/mnt/windows/Users/amedeo/Desktop/gamedev/kin/build/bin/kin_renderer_tests`). Logs are `/tmp/kin-material-inputs-full2d-isolated.log` and `/tmp/kin-material-inputs-baseline2d.log`. A temporary source copy omitting only that test runs the remaining suite with assertions enabled; repository tests are unchanged. Its first run caught a new reflection limit being applied to private/workgroup arrays; restricting that limit to descriptor-decorated resources fixes it. The subsequent diagnostic run passes, including reflection of every compiled engine shader (`/tmp/kin-material-inputs-2d-diagnostic2.log`). This is partial-suite validation with an explicitly recorded pre-existing failure, not a full 2D pass.

Final artifacts: `/tmp/kin-material-inputs-full3d-final.log` records a successful full native 3D dispatch. Focused results are `/tmp/kin-material-inputs-hardware-final.log` (fifteen passes, two inherited live-device lifecycle skips) and `/tmp/kin-material-inputs-software-final.log` (all twelve pass, including live foreign ownership). The final remaining-2D diagnostic run passes in `/tmp/kin-material-inputs-2d-final-diagnostic.log`. WebGL2 stock smoke and explicit unsupported-material-input checks pass (`/tmp/kin-material-inputs-web-final-smoke.log`). The standalone 2D-only graph compile/run and 193-file format check pass. Four decoded RGBA captures remain byte-identical to predecessor `c96504f`: Cornell/front and motion/still, pan and close at 320 x 240 with AO, contact shadows and TAA, using the reference command above. Captures are `/tmp/kin-material-inputs-before` and `/tmp/kin-material-inputs-after`.

## Standalone slow-light prototype

The example owns all solver/history code and shaders under `tools/slowlight3d`. No engine implementation, public API or default configuration changes. CPU checks compare against analytic ten-metre/100 km/h delays (0.36 seconds), both transport legs (0.72 seconds), delayed emitter birth/death, a constant-velocity source root, and exact moving-sphere crossing controls where delayed and instantaneous occlusion disagree. Additional checks cover occluder despawn, sequential history eviction, bounded catch-up after a large jump, long-running relative-time precision, pause, reset and invalid time/speed admission.

GPU probes compare energy, total delay, occlusion and history validity against that CPU model for startup, ordinary motion, history rollover/exhaustion, two moving-occluder controls and an already-despawned occluder. The RGBA8 probe packs validity/occlusion into one opaque colour channel so alpha blending cannot hide an unknown result. Expected values allow two quantization steps, including RGBA16F storage. Optimized/reference graph pixels match exactly; the delayed and instantaneous views differ visibly. Eight warmed frames with changed history and speed require unchanged graph compilation/field bindings, zero new resource handles beyond separately reported staging maintenance, no staging-capacity growth and no explicit 3D device-idle wait. Readback and UI startup are outside that warmed interval. These are correctness/resource gates, not a frame-time speedup measurement.

History is a fixed 121 x 32 = 3,872-byte CPU array mirrored in one imported GPU buffer, with at most four seconds at 30 Hz. The compute field is 589,824 bytes (384 x 192 x 8). Two 520 x 480 working-colour targets and one D32F attachment bring the default graph-managed descriptor payload to 3,585,024 bytes, excluding imported history, UI, window targets, swapchain and driver storage. The live path uploads the fixed history buffer before preparing consumers and performs no synchronous readback. The model does not retain full scene/mesh snapshots or implement arbitrary delayed geometry, eye-leg occlusion, multiple bounces or relativity.


Final native hardware and software Vulkan results are `/tmp/kin-slowlight-hardware-final.log` and `/tmp/kin-slowlight-software-final.log`: all four selected tests pass on each backend (CPU model, GPU prototype, existing surface graph and material inputs). The CPU model also passes AddressSanitizer/UndefinedBehaviorSanitizer (`/tmp/kin-slowlight-sanitized.log`). The 90-frame live loop exits cleanly (`/tmp/kin-slowlight-live.log`); malformed command-line values are rejected before window creation. The 196-file formatting gate and whitespace checks pass. Illumination, delay and exhausted-history captures were inspected at `/tmp/kin-slowlight-1.5.png`, `/tmp/kin-slowlight-delay.png` and `/tmp/kin-slowlight-exhausted.png`. The engine library is unchanged, so the preceding full stock/browser validation remains the baseline; this slice reruns the directly used public-operation regressions.
