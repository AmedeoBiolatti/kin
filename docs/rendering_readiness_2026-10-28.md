# Rendering readiness — 2026-10-28/29

This is a dated measurement record. For current build/viewer commands, GI controls, multi-camera visibility and
the CI/platform coverage contract, use the [rendering validation guide](rendering_validation.md).
The runtime checkpoints and test counts below describe their original builds; they are not current-commit CI results.
Later CI enabled shader-access analysis explicitly and exposed missing compute-stage sampler barriers in SDL.
The current build applies that fix; historical local synchronization passes below did not cover those shader
accesses. See the current guide and rendering progress for the corrected validation configuration.

## Three-hour testing and optimization follow-up — 29 October

Current runtime: `c084128` on `exp/rendering-foundations`, compared with frozen `22e1ed7` binaries. Main remains at `6245258`; its two local planning edits were preserved. Use the new [manual launcher guide](/mnt/windows/Users/amedeo/Desktop/gamedev/kin-eval/optimization-2026-10-29/START-HERE.txt), [result summary](/mnt/windows/Users/amedeo/Desktop/gamedev/kin-eval/optimization-2026-10-29/summary.json), and [executable hashes/configuration](/mnt/windows/Users/amedeo/Desktop/gamedev/kin-eval/optimization-2026-10-29/release-provenance.json). Earlier sections below retain the preceding validation campaign's results.

Five runtime commits:

- `d893341`: shared toroidal GI addressing uses a mask for power-of-two windows and preserves the general path for fitted dimensions.
- `5a07347`: unchanged GI submissions avoid sorted-copy allocation/sort/diff; voxel-job order and bounds are prepared once for both passes. Dense dirty boxes skip mask reads, while sparse unions reject kept blocks before triangle tests. The CPU cache stores eight bytes per submitted copy per view; it does not remove the initial hash/bounds traversal.
- `ed1596f`, `c084128`: device teardown releases retained texture/buffer/transfer allocations and material shader modules before destroying the device. Late CPU-handle destruction remains safe. Core validation exposed ten buffer/image/view lifetime errors and a separate retained-shader-module error; the repaired cases report zero. Shader tracking preserves descriptor-payload accounting.
- `dfe393d`: quads and sprites supply zero custom vertex data, matching fragment materials' geometry interface. Four Vulkan pipeline-interface errors become zero; a surface regression covers the fallback value.

The shared-memory AO reconstruction experiment was rejected: it saved only about 0.009 ms at 1440p and caused a handful of one-level pixel differences. The existing AO reconstruction and quality budgets remain unchanged.

### Verification

- Native all-target Release build passes. All 146 configured cases are accounted for: 141 pass after an isolated rerun of the 31 evaluation Python tests; five existing hardware foreign-device cases remain skipped and pass on software Vulkan. One contended run hit the evaluator's 120-second limit; its isolated CTest rerun passes. The final full 3D executable, 2D tests and GPU memory tests pass.
- Full 3D core and synchronization sweeps report zero Vulkan errors. Six graph/geometry families pass synchronization validation. Final retained-shader and 2D interface/lifetime checks pass core validation with the validation library observed loaded. Configuration follows [LunarG's synchronization validation guide](https://vulkan.lunarg.com/doc/view/latest/windows/synchronization_usage.html).
- Scoped AddressSanitizer/UndefinedBehaviorSanitizer checks pass for GPU memory, GI, retained handles, model imports, attachment graphs and postprocessing; the final material-shader lifetime case passes after rebuilding. This is **not a completed full sanitizer suite**. The initial full run was interrupted for budget, remained alive after SIGTERM and was force-stopped; the last logged point-light case exits normally in an isolated debugger run. LeakSanitizer was disabled.
- Minimal GCC 13 with 3D/optional systems disabled passes six selected checks, followed by fresh renderer/memory reruns after the repairs. Frozen Sponza, Bistro and Yard manual launchers pass bounded loading/drawing smokes.
- The GI/interface checkpoint (`dfe393d`): [720p regression](/home/bamedeo/kin-build/rendering-optimization-2026-10-29/accepted-regression-720/report/index.html) and [1440p regression](/home/bamedeo/kin-build/rendering-optimization-2026-10-29/accepted-regression-1440/report/index.html): seven stills plus a 12-frame moving Bistro clip at each size are pixel-identical to the original baseline. A [final ownership-only regression](/home/bamedeo/kin-build/rendering-optimization-2026-10-29/release-regression-720/report/index.html) adds five identical frames across Sponza/Bistro/Yard. No new Cycles renders were made; the earlier two-seed references remain the accuracy baseline. These runs establish regression equivalence, not a new accuracy measurement.
- [GI resize/feature stress](/home/bamedeo/kin-build/rendering-optimization-2026-10-29/release-stress-fixed/report/index.html): three cycles and 1,720 frames each at 1440p return to exactly **777,092,092 bytes** for Sponza and **505,251,688 bytes** for Cornell. Both initial/final plateaus are stable. Asset/shader reload, load/unload and multi-view stress remain unexercised by this pass. Payload counters are owned descriptors, not physical residency.

### Timing

Diagnostic moving-Bistro traces show about 32% less idle GI CPU preparation (0.239 to 0.163 ms at 1440p), 16% less GI-ray time (0.417 to 0.349 ms), and 13% less voxel-attribute time (1.595 to 1.387 ms on six update frames). The latter reduces total measured GI scene-update cost about 6%. These are individual scopes, with profiling overhead and different sampling windows; they are not whole-frame gains. [Per-pass data](/mnt/windows/Users/amedeo/Desktop/gamedev/kin-eval/optimization-2026-10-29/profile-summary.json) retain clocks, control-pass timings and nested-range accounting.

Final normal-binary measurements use three alternating pairs, 180 warm-up plus 540 measured frames per run: **48 runs / 25,920 measured frames**, with 20 Hz NVML telemetry. No pass markers or synchronizing captures run in throughput jobs. Yard uses the **heavy** recipe; its extra shutdown frame is retained in raw data and excluded from the requested sample window.

| Scene | Render size | Baseline median FPS | Candidate median FPS | Median paired change |
|---|---|---:|---:|---:|
| bistro | 1280x720 | 207.9 | 210.5 | +1.11% |
| bistro | 2560x1440 | 88.9 | 88.9 | +0.07% |
| sponza | 1280x720 | 390.3 | 396.6 | +1.68% |
| sponza | 2560x1440 | 129.5 | 131.0 | +1.47% |
| cornell | 1280x720 | 964.4 | 941.5 | -2.75% |
| cornell | 2560x1440 | 281.8 | 278.1 | -2.02% |
| yard | 1280x720 | 33.2 | 33.1 | -0.02% |
| yard | 2560x1440 | 30.5 | 31.0 | +2.73% |

The scene-dependent changes remain within about ±3%. Software power capping and some thermal slowdown occur; pairwise clock differences are retained in [throughput-summary.json](/mnt/windows/Users/amedeo/Desktop/gamedev/kin-eval/optimization-2026-10-29/throughput-summary.json). **There is no demonstrated broad 1440p FPS win or performance signoff.** Cornell's small negative observations need a controlled rerun, including the fitted-volume fallback. Heavy Yard averages roughly 29.9 ms of CPU render work and 0.09 ms presenting at 720p, pointing to CPU render/submission work as its next profiling target. All paired final owned-payload counters match.

The SDL/NVIDIA/X11 simultaneous-device teardown crash still reproduces in an SDL-only program, including with core validation; releasing both surfaces first avoids it in the probe. The reproduction and logs are retained, with no speculative engine workaround shipped. Native Windows/macOS, other hardware GPUs, and a complete sanitizer run remain pre-merge coverage gaps. This is a stronger native manual-testing candidate; main has not been deployed.

Original campaign measured source: `66f834f78411f022703f24edde36926bec01f35e` on `exp/rendering-foundations`. The last runtime change is `41d25d1`; later commits adjust tests and an API comment. Frozen executable SHA-256 values and the exact CMake configuration are in [provenance.json](/mnt/windows/Users/amedeo/Desktop/gamedev/kin-eval/readiness-2026-10-28/provenance.json) and [CMakeCache-candidate.txt](/mnt/windows/Users/amedeo/Desktop/gamedev/kin-eval/readiness-2026-10-28/CMakeCache-candidate.txt).

This is a candidate for native manual testing. Main has not been merged, and its local plan edits were left untouched. Performance acceptance remains open because the laptop throttled heavily. Successful evaluation jobs mean measurements completed and passed protocol/resource checks; image-quality acceptance also requires reviewing their differences.

## Build and regression checks

| Check | Result | Evidence |
|---|---|---|
| Native Release/GCC 15.2 all-target build | Pass; local CMake workaround removed | [build log](/mnt/windows/Users/amedeo/Desktop/gamedev/kin-eval/readiness-2026-10-28/native-build-clean.log) |
| Configured native CTest inventory | 141 executable cases pass after targeted test repairs; five hardware foreign-device cases skipped | [initial full run](/mnt/windows/Users/amedeo/Desktop/gamedev/kin-eval/readiness-2026-10-28/native-ctest.log), [final focused rerun](/mnt/windows/Users/amedeo/Desktop/gamedev/kin-eval/readiness-2026-10-28/native-focused-final.log) |
| Five foreign-device probes on software Vulkan | 5/5 pass | [software run](/mnt/windows/Users/amedeo/Desktop/gamedev/kin-eval/readiness-2026-10-28/software-foreign-ctest.log) |
| Minimal GCC 13 build, 3D and optional subsystems disabled | 5/5 selected checks pass, including GPU 2D | [minimal run](/mnt/windows/Users/amedeo/Desktop/gamedev/kin-eval/readiness-2026-10-28/no3d-ctest-final.log) |
| Chrome/WebGL2 packaged fixture, SwiftShader | Pass: materials, skinning, sky, fog, shadows, input, fullscreen, three resizes | [browser smoke](/mnt/windows/Users/amedeo/Desktop/gamedev/kin-eval/readiness-2026-10-28/web-smoke.log) |
| Evaluation Python tests | 31 pass | [unit results](/mnt/windows/Users/amedeo/Desktop/gamedev/kin-eval/readiness-2026-10-28/eval-python-tests.log) |
| 3D formatting and whitespace | 199 files pass; git diff check clean | [format gate](/mnt/windows/Users/amedeo/Desktop/gamedev/kin-eval/readiness-2026-10-28/format-final.log) |
| Frozen Sponza/Bistro/Yard launchers | All bounded loading/drawing smokes pass | [manual guide](/mnt/windows/Users/amedeo/Desktop/gamedev/kin-eval/readiness-2026-10-28/START-HERE.txt) |

The native configuration enables 3D, Jolt, scripting, audio, TTF, image I/O and runtime assets; 2D physics is disabled. The minimal configuration is a smoke subset, not an all-components CI replacement. Native Windows/macOS, other GPUs and Khronos validation-layer coverage were not run. The five hardware cross-device probes retain their existing SDL/NVIDIA/X11 lifecycle guard; their software passes do not establish hardware coverage.

## Repairs made

- Fixed fractional-DPI pointer conversion. A 721×451 rounded scene previously mapped the centre to `(80, 50.0416)` instead of `(80, 50)`. Conversion now follows each projected axis; tests cover odd window sizes, multiple logical sizes and integer/fractional presentation.
- Applied the sol2/GCC 15 diagnostic compatibility option through the dependency target so public-header consumers and tests build consistently. This retains the upstream warning rather than patching cached third-party source. See [upstream issue 1695](https://github.com/ThePhD/sol2/issues/1695).
- Updated two stale asset-sharing assertions: imported images share storage while material maps retain independent sampling. Colour/data separation and legacy particle filtering remain checked.
- Guarded PNG-dependent output tests when image I/O is disabled; raw GPU colour/HDR/blending checks still run.

## Scene coverage and completed jobs

720p covers Sponza, Industrial Yard, Bistro, Cornell, Flight Helmet, IBL spheres, shadows, a 20,000-instance crowd, moving thin geometry, and an opt-in half-AO Sponza variant. The large image pass covers Sponza, Yard, Bistro and half AO. Timing covers every configuration at both 1280×720 and 2560×1440.

| Run | Status | Completed jobs | Report |
|---|---|---:|---|
| bench-720 | ok | 60 | [HTML](/mnt/windows/Users/amedeo/Desktop/gamedev/kin-eval/readiness-2026-10-28/bench-720/report/index.html) |
| bench-1440 | ok | 60 | [HTML](/mnt/windows/Users/amedeo/Desktop/gamedev/kin-eval/readiness-2026-10-28/bench-1440/report/index.html) |
| regression-720 | ok | 40 | [HTML](/mnt/windows/Users/amedeo/Desktop/gamedev/kin-eval/readiness-2026-10-28/regression-720/report/index.html) |
| regression-1440 | ok | 16 | [HTML](/mnt/windows/Users/amedeo/Desktop/gamedev/kin-eval/readiness-2026-10-28/regression-1440/report/index.html) |
| stress | ok | 8 | [HTML](/mnt/windows/Users/amedeo/Desktop/gamedev/kin-eval/readiness-2026-10-28/stress/report/index.html) |
| quality-720 | ok | 18 | [HTML](/mnt/windows/Users/amedeo/Desktop/gamedev/kin-eval/readiness-2026-10-28/quality-720/report/index.html) |
| soak-1440 | ok | 2 | [HTML](/mnt/windows/Users/amedeo/Desktop/gamedev/kin-eval/readiness-2026-10-28/soak-1440/report/index.html) |
| yard-repeatability | ok | 4 | [HTML](/mnt/windows/Users/amedeo/Desktop/gamedev/kin-eval/readiness-2026-10-28/yard-repeatability/report/index.html) |
| quality-settled | ok | 6 | [HTML](/mnt/windows/Users/amedeo/Desktop/gamedev/kin-eval/readiness-2026-10-28/quality-settled/report/index.html) |

## Images against the preceding build

The baseline is the frozen pre-AO evaluation build described by its binary hashes. It is **not** a main-branch baseline. Native outputs are compared at identical dimensions and deterministic simulation times. Quality metrics are computed on full-size images; the contact sheet is only an overview.

At 720p, all 19 unaffected views/clips are pixel-identical. The other three cases explicitly use half-resolution AO: Yard's route and two Sponza views. Their differences are expected from the reconstruction fix. Yard hardcodes half AO in its scene configuration (`examples/industrial_yard/main.cpp`); its feature payload grows by exactly one full-resolution R8 target: 921,600 bytes at 720p, 3,686,400 at 1440p. All other non-timing Yard workload counters match. The matched-counter audit covers 3,615 frame pairs across both resolutions and captures (`yard-workload-audit.json`). Its twelve-frame clip has mean absolute channel error 0.026737/255, SSIM 0.999897, FLIP 0.001900 and video JOD 10.0. Independent executions of the same candidate produce identical Yard pixels. The amplified differences remain available for review.

| Resolution | Scene/view | Identical | MAE /255 | SSIM | FLIP | JOD |
|---|---|---|---:|---:|---:|---:|
| 720 | sponza/nave | True | 0.000000 | 1.000000 | 0.000000 | 10.0000 |
| 720 | sponza/roof | True | 0.000000 | 1.000000 | 0.000000 | 10.0000 |
| 720 | yard/route | False | 0.026737 | 0.999897 | 0.001900 | 10.0000 |
| 720 | bistro/start | True | 0.000000 | 1.000000 | 0.000000 | 10.0000 |
| 720 | bistro/street | True | 0.000000 | 1.000000 | 0.000000 | 10.0000 |
| 720 | bistro/terrace | True | 0.000000 | 1.000000 | 0.000000 | 10.0000 |
| 720 | bistro/start_walk | True | 0.000000 | 1.000000 | 0.000000 | 10.0000 |
| 720 | cornell/front | True | 0.000000 | 1.000000 | 0.000000 | 10.0000 |
| 720 | helmet/front | True | 0.000000 | 1.000000 | 0.000000 | 10.0000 |
| 720 | helmet/side | True | 0.000000 | 1.000000 | 0.000000 | 10.0000 |
| 720 | ibl_spheres/front | True | 0.000000 | 1.000000 | 0.000000 | 10.0000 |
| 720 | ibl_spheres/grazing | True | 0.000000 | 1.000000 | 0.000000 | 10.0000 |
| 720 | shadows/overview | True | 0.000000 | 1.000000 | 0.000000 | 10.0000 |
| 720 | shadows/contact | True | 0.000000 | 1.000000 | 0.000000 | 10.0000 |
| 720 | shadows/fence | True | 0.000000 | 1.000000 | 0.000000 | 10.0000 |
| 720 | shadows/grazing | True | 0.000000 | 1.000000 | 0.000000 | 10.0000 |
| 720 | crowd/first_person | True | 0.000000 | 1.000000 | 0.000000 | 10.0000 |
| 720 | crowd/mid_deck | True | 0.000000 | 1.000000 | 0.000000 | 10.0000 |
| 720 | crowd/above | True | 0.000000 | 1.000000 | 0.000000 | 10.0000 |
| 720 | motion/pan | True | 0.000000 | 1.000000 | 0.000000 | 10.0000 |
| 720 | sponza_half_ao/nave | False | 0.069683 | 0.999589 | 0.004799 | 9.9881 |
| 720 | sponza_half_ao/roof | False | 0.090001 | 0.999312 | 0.005499 | 9.9657 |
| 1440 | sponza/nave | True | 0.000000 | 1.000000 | — | — |
| 1440 | yard/route | False | 0.019228 | 0.999920 | — | — |
| 1440 | bistro/start | True | 0.000000 | 1.000000 | — | — |
| 1440 | sponza_half_ao/nave | False | 0.051054 | 0.999696 | — | — |

[Scene overview](/mnt/windows/Users/amedeo/Desktop/gamedev/kin-eval/readiness-2026-10-28/contact-sheet-720.png) · [Yard difference amplified 4×](/mnt/windows/Users/amedeo/Desktop/gamedev/kin-eval/readiness-2026-10-28/yard-difference4.png) · [Same-binary repeatability control](/mnt/windows/Users/amedeo/Desktop/gamedev/kin-eval/readiness-2026-10-28/yard-repeatability/report/index.html)

## Cycles accuracy and reference noise

Each 720p view uses two independent 512-sample Cycles renders, seeds **17 and 53**. The image score uses the first seed; the two seeds' disagreement is reported separately as the reference noise floor. No noise-subtracted quality score is claimed. Display mapping uses the existing `bistro_laptop` model and reviewed exporters, not a new display calibration. Bistro's ISO/ACES/saturation mapping and reference transforms remain explicit in the manifests.

| Scene/view | FLIP ↓ | Cycles/Cycles FLIP ↓ | JOD ↑ | Cycles/Cycles JOD ↑ | SSIM ↑ | Bias, levels |
|---|---:|---:|---:|---:|---:|---:|
| sponza/nave | 0.520263 | 0.021626 | 6.0069 | 9.8664 | 0.49240 | 48.052 |
| sponza/roof | 0.398154 | 0.017882 | 5.6039 | 9.9399 | 0.64817 | 31.557 |
| bistro/start | 0.176644 | 0.029015 | 7.4682 | 9.8793 | 0.80523 | -2.800 |
| bistro/street | 0.190477 | 0.026198 | 7.3285 | 9.8830 | 0.79627 | -4.080 |
| bistro/terrace | 0.129184 | 0.022705 | 7.6471 | 9.8596 | 0.80133 | -1.334 |
| helmet/front | 0.013546 | 0.001139 | 9.4681 | 9.9828 | 0.99102 | -0.236 |
| helmet/side | 0.014036 | 0.001177 | 9.3829 | 9.9721 | 0.98884 | -0.313 |
| ibl_spheres/front | 0.172341 | 0.011816 | 7.3899 | 9.9547 | 0.93111 | -1.427 |
| ibl_spheres/grazing | 0.164651 | 0.011138 | 6.9928 | 9.9632 | 0.92911 | -0.802 |
| shadows/contact | 0.122618 | 0.011550 | 8.1356 | 9.9963 | 0.96750 | -3.952 |
| shadows/fence | 0.094114 | 0.009113 | 8.7693 | 10.0000 | 0.97322 | -3.710 |
| cornell/front | 0.122823 | 0.017571 | 7.2539 | 9.7605 | 0.92447 | -3.708 |

### Documented settled-GI preset

The earlier quality-agent notes use 600 warm-up frames, GI at 64 rays/probe and 4,096 probe updates/render, anisotropy 4, specular AA 0.25 and TAA sharpness 0.12. The broad Sponza fixture above has GI disabled; it must not be interpreted as a GI-enabled quality result. Cornell above uses default-budget GI after 180 warm-up frames. This supplemental preset reuses the exact same two-seed Cycles references. It is a combined quality preset, not a GI-only ablation.

| Scene/view | FLIP ↓ | Reference noise FLIP | JOD ↑ | SSIM ↑ | Bias, levels |
|---|---:|---:|---:|---:|---:|
| sponza/nave | 0.106455 | 0.021626 | 8.0733 | 0.81491 | -1.903 |
| sponza/roof | 0.112812 | 0.017882 | 7.3590 | 0.84053 | -1.772 |
| cornell/front | 0.099739 | 0.017571 | 7.7001 | 0.94132 | -2.178 |

These values measure the configured engine against the existing scene/export mapping. They include lighting/material approximations and any mapping mismatch; two seeds do not establish perfect reference convergence. Cornell's small enclosure visibly exposes the default-budget GI's convergence/approximation limits; compare it with the settled preset before drawing conclusions about available quality. Daylight Bistro references exclude the unsupported night/fog cases. The procedural Yard and crowd have no reviewed Cycles provider and use regression references only.

## Timing, tails and memory observations

Three alternating repeats per build/configuration, 180 warm-up and 600 measured frames each: **72,000 measured frames** across both resolutions. FPS below is 1000 divided by mean measured frame interval, not the 60 Hz simulation rate or CPU submission time. No synchronizing pass markers/readbacks were enabled in throughput jobs. These runs do not provide source-frame GPU timing percentiles.

The laptop's GPU clocks fell dramatically during sustained work. A later read-only driver query confirmed software thermal slowdown and software power capping active while AC power was connected; this observation is recorded in hardware.json. Bistro at 1440p changed from 76.28 to 40.04 to 25.34 FPS in the previous build, and 75.28 to 37.01 to 30.45 in the candidate. Therefore the aggregate speed comparisons and nominal bootstrap intervals are **diagnostic, not a performance acceptance gate**. Shared-machine activity is another uncontrolled variable. The automatic comparison flags require a controlled rerun before quantifying code effects. Yard and the Sponza half-AO variant both select the new reconstruction pass, which adds a dispatch and an R8 target; it has a real work/memory cost even though its exact timing cost cannot be isolated from this run. The public half-resolution setting remains opt-in (default false).

| Configuration | 720p FPS (repeat range) | 720p p99 ms | 1440p FPS (repeat range) | 1440p p99 ms | Settled owned MiB 720/1440 |
|---|---:|---:|---:|---:|---:|
| sponza | 549.2 (529.8–572.4) | 2.18 | 63.3 (34.9–150.9) | 31.84 | 479.2 / 761.6 |
| yard | 541.4 (465.8–609.3) | 6.53 | 75.4 (45.0–167.2) | 35.08 | 112.9 / 433.1 |
| bistro | 184.6 (176.6–194.5) | 11.58 | 41.0 (30.4–75.3) | 42.18 | 1447.8 / 1682.8 |
| cornell | 943.1 (926.1–966.9) | 1.51 | 84.1 (47.2–270.9) | 23.28 | 359.4 / 657.6 |
| helmet | 1581.4 (1420.4–1718.2) | 4.04 | 227.3 (148.8–427.8) | 7.19 | 382.2 / 664.6 |
| ibl_spheres | 1240.5 (1227.7–1266.5) | 1.05 | 99.0 (55.5–298.3) | 18.41 | 101.9 / 379.0 |
| shadows | 1065.8 (984.7–1114.9) | 1.14 | 80.3 (45.7–202.8) | 25.42 | 107.5 / 389.9 |
| crowd | 306.3 (284.9–319.2) | 6.17 | 35.9 (21.3–98.2) | 48.23 | 113.8 / 396.2 |
| motion | 710.0 (698.0–717.9) | 2.00 | 43.1 (31.7–150.2) | 32.69 | 107.5 / 389.9 |
| sponza_half_ao | 710.8 (652.3–767.2) | 2.84 | 63.2 (35.3–147.9) | 29.71 | 478.8 / 759.9 |

[Full timing/RSS table](/mnt/windows/Users/amedeo/Desktop/gamedev/kin-eval/readiness-2026-10-28/performance.csv) and [combined machine-readable results](/mnt/windows/Users/amedeo/Desktop/gamedev/kin-eval/readiness-2026-10-28/summary.json) include p50/p95/p99, frame-budget exceedances, sampled peak RSS, resource peaks, late allocations and paired changes. GPU resource counters represent owned texture/buffer descriptor payloads and tracked handles, not physical residency or every driver allocation. Driver-wide observations include other applications. Initial pool/staging retirement is not a leak.

## Persistent resource checks

Each reference case performs eight AO/TAA/resize cycles at a 1440p base size with disabled-feature settling periods. Every tested case returns to the exact initial owned-payload plateau.

| Case | Measured frames | Initial bytes | Final bytes | Delta |
|---|---:|---:|---:|---:|
| sponza | 2370 | 614321952 | 614321952 | 0 |
| motion | 2370 | 224515424 | 224515424 | 0 |
| ibl_spheres | 2370 | 164006320 | 164006320 | 0 |
| sponza_half_ao | 2370 | 557816352 | 557816352 | 0 |

The application replays use 5,760 measured frames at 1440p: four 1,440-frame Yard route loops and four closed 24-second Bistro tours, after 240 warm-up frames. Per-loop resource counters:

| Application | Loop | End owned MiB | Peak owned MiB | Allocations within loop |
|---|---:|---:|---:|---:|
| yard | 1 | 433.139 | 433.139 | 2 |
| yard | 2 | 433.139 | 433.139 | 0 |
| yard | 3 | 433.139 | 433.139 | 0 |
| yard | 4 | 433.139 | 433.139 | 0 |
| bistro_tour | 1 | 1682.761 | 1794.762 | 0 |
| bistro_tour | 2 | 1682.761 | 1682.761 | 0 |
| bistro_tour | 3 | 1682.761 | 1682.761 | 0 |
| bistro_tour | 4 | 1682.761 | 1682.761 | 0 |

The reference stress adapter does not exercise asset/shader reload, complete scene load/unload, or multiple views. Unit tests cover parts of those paths; the large-scene replay is a movement/resource-growth check, not a complete application reload qualification. Physical driver residency/leaks are not ruled out solely by the owned-payload counters.

## Manual testing and next decisions

Start with the frozen candidate:

```sh
python3 /mnt/windows/Users/amedeo/Desktop/gamedev/kin-eval/readiness-2026-10-28/try_rendering.py sponza
python3 /mnt/windows/Users/amedeo/Desktop/gamedev/kin-eval/readiness-2026-10-28/try_rendering.py bistro
python3 /mnt/windows/Users/amedeo/Desktop/gamedev/kin-eval/readiness-2026-10-28/try_rendering.py yard
```

Add `--width 2560 --height 1440`; compare Sponza's opt-in half AO with `--ao-half`. Sponza: WASD/mouse, Tab/Escape for the panel, 1/2 for cameras, F2 for reproducible settings, F3 for stats, F12/P for screenshots. Yard preview follows its route; Escape exits. [Full launcher guide](/mnt/windows/Users/amedeo/Desktop/gamedev/kin-eval/readiness-2026-10-28/START-HERE.txt).

Prioritize resize, camera changes, AO/TAA toggles, materials/shadows and your own imports. Sponza Walk currently exposes AO/TAA and other existing viewer controls, but no GI control; the settled-GI measurement uses kin_reference. Use Bistro for interactive GI testing. Bistro still reports ignored animation channels for nodes without attached parts in `mannequin.glb` (Fall/Idle/Jog/Run), in both builds. Preserve those diagnostics when evaluating a new rig.

The next gate is your manual testing plus the project's normal CI/platform matrix. Keep performance signoff separate until clocks/load can be controlled. Quality follow-up should use the worst measured scenes, including small-enclosure GI, rather than tuning only Bistro. Micro-optimization is deferred.

All raw native frames, effective settings, captures, two-seed references, error maps, process logs and provenance live under `/mnt/windows/Users/amedeo/Desktop/gamedev/kin-eval/readiness-2026-10-28`. Large previous evaluations were SHA-256 verified and archived to the Windows volume with their original paths preserved by a symlink. Re-run suites into a fresh output directory; strict resume requires the original measured code/configuration identity.
