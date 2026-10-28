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
