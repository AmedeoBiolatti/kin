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
