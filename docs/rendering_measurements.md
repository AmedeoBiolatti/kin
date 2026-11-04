# Rendering foundations validation

The implementation branch starts at lighting snapshot `77b76c8`. Correctness, CPU tracking, memory accounting, public graphs and stock-phase scheduling are delivered in separate commits through `e1226c2`. Main's planning edits and the lighting worktree's uncommitted GI subvoxel changes were preserved. The [progress log](rendering_progress.md) distinguishes this delivery from the full [pipeline plan](rendering_pipeline_plan.md).

## CPU work

### Industrial Yard CPU investigation, 2026-11-03

The newer cross-engine experiment lives in the sibling `kin-wt/yard-engines`
worktree, based on `998c1e9`. Its `out/yard-baseline/medium-engines-summary.md`
reports 720p frame p50 **6.13 ms Kin / 2.67 ms Unreal matched** over three repeats.
Kin's render caller consumes **5.88 ms thread CPU**. These are frame intervals;
Unreal's separate game/render/RHI timers do not describe the same scope as Kin's
caller timer. The original comparison also uses Kin 4x MSAA against Unreal's
no-AA matched preset, and has no equivalent native Kin GPU timestamp.

A new CPU profile uses the existing Yard binary, NVIDIA Vulkan on the RTX 4080
Laptop GPU, CPUs 0–15, four helpers, 300 warm-up and 1,440 measured route frames.
`perf record -F 499 -e cycles:u --call-graph dwarf,8192` records all threads.
CSV monotonic timestamps select only measured frames; main-thread percentages
below are inclusive, period-weighted user cycles, not elapsed or GPU times.

| Main-thread work | Share |
| --- | ---: |
| Local-shadow culling, caster lists, packing and batching | 42.5% |
| Camera culling, bounds and instance uploads | 25.7% |
| Main-pass recording, including its driver calls | 12.5% |
| Local-light grid | 5.9% |
| Draw/composition ordering | 5.1% |
| Local-shadow pass recording | 2.9% |
| Material-input validation | 1.1% |

This is primarily preparation work. The existing prepared-layout cache hits on
every measured frame, but explicitly excludes visibility and instance payloads.
The 7,200 stationary props use individual `Scene3D::draw` records: they do not
receive `InstanceBatch3D` retained GPU records or persistent world-bound caches.
Their bounds and camera/shadow instance packing repeat. Local-shadow gathering
also makes tasks for thousands of tiny draws and sorts caster entries each frame.
The ECS collection path already groups compatible meshes/materials; this finding
is specific to the direct submission pattern and is not evidence that ECS
transform propagation is responsible for this benchmark.

**Tested reduction using existing APIs.** A temporary harness groups stationary
props by `(mesh, material)` into 64 persistent batches, one spatial group per
batch. It preserves all prop transforms, materials, 800 moving props, 128 animated
robots, smoke, lights, 4x MSAA and HDR. Five paired full-route repeats alternate
baseline/batched/spatial order. Neither `perf` nor image readback runs during
throughput measurement; thread/process CPU clocks run in every case.

| Median of five run medians | Individual static draws | Retained static batches |
| --- | ---: | ---: |
| Frame p50 | 6.268 ms | 3.186 ms |
| Frame p95 (median of run p95s) | 7.559 ms | 5.388 ms |
| Render caller thread CPU p50 | 6.017 ms | 1.379 ms |
| Render process CPU p50, including helpers | 8.047 ms | 1.874 ms |
| Prepared draw records | 7,429 | 293 |
| Main-pass draws, p50 | 4,211.5 | 240 |
| Local-shadow draws | 1,140 | 524 |
| Streamed instance bytes/frame, p50 | 1,338,560 | 103,360 |
| Changed retained instance bytes/frame | 102,400 | 102,400 |
| Recomputed streamed bounds/frame | 8,855 | 1,655 |
| Local gather task payload capacity | 950,656 bytes | 37,248 bytes |
| Resident instance GPU payload | 102,400 bytes | 1,024,000 bytes |

Paired thread-CPU reductions range from **73.0% to 79.9%**; process-CPU reductions
range from **73.0% to 79.5%**. Frame reductions range from 36.0% to 55.0%.
Clocks/background load are uncontrolled and drift substantially, so these are
local paired observations, not a new Unreal ranking or a universal speedup.
Resident GPU records grow by about **0.88 MiB**, avoiding repeated computation
and streaming. The Medium scene's 64 static plus 64 moving batches exactly fill
the existing 128-table admission limit; no budget was raised.

Per-frame camera instances/triangles, shadow casters/triangles, joint counts and
motion sums agree across the entire route. Outside, warehouse and elevated
captures (route offsets 0, 540 and 960) are decoded-RGBA identical for both
batching variants. A 16-unit spatial-cell variant cuts median local instance
tests from 33,348 to 17,610 but increases task payload to 275,840 bytes; its caller
CPU is 1.857 ms versus 1.379 ms for simple batches. Fewer sphere tests alone do
not establish a win. Three no-local-shadow and three MSAA-1 controls support the
preparation diagnosis; their later timing windows are not used as paired feature
cost estimates.

The example now exposes the successful path as `--batch-static-props` and reports
its effective batch count. Default comparison submissions remain unchanged.
After generating the usual Medium recipe, compare otherwise identical commands
with/without that flag:

```sh
KIN_RENDER_BACKEND=gpu build/bin/industrial_yard \
  --recipe /path/to/medium.yrd --metrics /path/to/batched.csv \
  --report=/path/to/batched.json --profile-cpu --measured 1440 --batch-static-props
```

**Next renderer work, in order.** Make compatible direct opaque/masked draws
benefit from retained batching or revision-tracked static records automatically;
the current ECS buckets provide an existing model. Cache fixed local-light caster
membership and batch plans before rebuilding tasks, with invalidation for changed
meshes, transforms, poses, material masks, light projections and camera-dependent
LOD/reach. Keep static and dynamic dependencies separate: the fixture deliberately
disables whole-map shadow caching, and one moving caster would invalidate a
whole cached map anyway. Reuse bounded scratch storage and precomputed sort keys
after reducing the number of records. The batched path also unlocks existing
four-sphere SIMD ranges that one-copy draws cannot use. Reprofile those changes
before adding workers or changing compiler flags. The per-frame public prepared
scene handle shares work between consumers; it is not a persistent cross-frame
cache for static scene content.

Artifacts are `/tmp/kin-cpu-investigation/`: measured-window profile summary,
raw stacks/data, source patch, build commands, per-run CSV/JSON/logs, provenance,
work checks and exact image comparisons. The original sibling worktree and its
existing rendering edits were used read-only. At this investigation checkpoint,
only the example option and these notes changed; no renderer algorithm was replaced.

Final main-source validation separately builds the edited example against the
existing native renderer libraries. Its default/batched full 1,440-frame routes
have identical geometry, shadow and motion counters. Three paired captures use
main's own background settings and agree exactly; an additional TAA/AO/contact
capture agrees exactly. Rebuilt-command and empty-population runs pass. Small
and Heavy controls (300 warm-up, 16 measured frames) preserve geometry/shadow/
motion counts, including Heavy's streamed fallback beyond 128 retained tables;
these short checks are not scaling measurements. All five recipe tests pass,
and `git diff --check` is clean. At that checkpoint the renderer library was
unchanged, so the full renderer suite was not repeated for the example-only option.

### Direct scene retention implementation

Branch `perf/render3d-cpu-preparation` starts at main `998c1e9`. Its first engine
slice shares one retained table across a source scene's direct opaque/masked
records while preserving every colour draw's order, material and picking ID.
It caches packed GPU records, transformed bounds and admission counts by source
identity/revision; pose-only updates and camera/light changes do not rebuild the
static payload. Visibility, local-shadow gathering and sorting still run. The
earlier material-grouped example remains a separate, opt-in experiment: this
implementation does not claim its much larger CPU reduction.

Admission requires 32 eligible draw records and 256 copies after an unchanged
render, and shares the existing 32 MiB/128-table GPU and 4 MiB bounds limits.
Raw mutable spans, TAA, enabled LOD/occlusion, skinned/blended/custom/wireframe
draws and explicit prepared consumers keep their prior paths. A scene command
edit refreshes the whole source table; sparse direct-record updates and cached
light caster plans are subsequent work. The fallback for an additional edit
after an upload in one GPU frame preserves all earlier views.

The new GPU regression compares decoded pixels and geometry/shadow counters with
`debug.retain_scene_instances = false`, including mixed masked/opaque/alpha,
explicit batches, skinning, equal-depth surfaces, one/four sun cascades, local
shadows, AO/contact, camera and pose edits, raw spans, source moves with a held
weak-identity token, independent overlay tables, three views around same-frame
edits, and over-budget streaming. Its warmed static path requires no record
uploads or repeated static bounds, with no explicit device-idle wait. It passes
on NVIDIA Vulkan and llvmpipe. The regression exposed a fresh moved-scene table
mistaken for populated revision zero; checking initialization independently of
revision fixes it. The CPU suite and five recipe tests also pass.

Fresh same-binary on/off timing runs follow the user's power-setting change;
the preceding investigation's timings are not their baseline. Both controls use
four helpers, CPUs 0–15, 720p, default 4x MSAA/HDR, the complete Medium route,
300 warm-up and 1,440 measured frames. Five alternating pairs run serially,
with no build, test, profiler or capture load alongside them. Captures follow
throughput measurement at offsets 0, 540 and 960. Reproduce with
`tools/benchmark_render3d_direct_cache.py --binary <industrial_yard> --output <directory>`.

| Median of five run medians | Cache off | Cache on |
| --- | ---: | ---: |
| Render caller thread CPU p50 | 6.204 ms | 5.728 ms |
| Render process CPU p50 | 8.220 ms | 7.333 ms |
| Frame p50 / p95 | 6.477 / 9.257 ms | 5.984 / 8.557 ms |
| Recomputed bounds/frame | 8,855 | 1,446 |
| Streamed instance bytes/frame, p50 | 1,338,560 | 66,048 |
| Direct records retained | 0 | 7,409 |
| Static table uploads / bounds rebuilt after warm-up | — | 0 / 0 |
| Resident instance GPU payload | 102,400 bytes | 1,133,440 bytes |

The deterministic reductions are **83.7% fewer bound transformations** and
**95.1% fewer streamed instance bytes** at p50, for about **0.98 MiB** additional
live GPU payload. Main draw counts and per-frame geometry, shadows, joints and
motion sums agree, and all three decoded captures are exact. Local-shadow draw
counts remain 1,140: the gather/list/sort workload has not been eliminated.

Clocks/background load drift substantially across these runs. The median of the
five paired thread-CPU reductions is **2.9%** (range **0.5–20.2%**); process-CPU
reduction is **6.8%** (range **3.8–22.0%**). Paired frame reductions range from
**−0.2% to 21.5%**, with a 3.8% median. The run-median table alone overstates the
typical paired gain under that drift. Treat frame throughput as noisy and the
CPU benefit as modest; the larger example-batching gain above is not achieved
by this order-preserving slice. An initial version that rescanned admission
eligibility every frame is archived under `yard-initial`; the final code caches
that count with the source revision.

The final complete native 3D suite also passes after admission-count reuse.
Implementation artifacts are `/tmp/kin-cpu-implementation/`: clean build logs,
full native suite logs, NVIDIA/llvmpipe direct-cache regressions, CPU/recipe logs,
source identities and `yard/` commands/CSV/captures/summary. The next measured
target is fixed-light caster membership and cached gather/batch plans, whose
dependency rules must keep moving casters, masks, poses and camera-dependent
LOD/reach correct.

#### Fixed-light local caster plans

The next engine slice caches static direct-source membership for stock spot and
point maps, and exact sort permutations within the existing 128-entry depth
batching windows. The key includes weak source identity/revision, source ref
count, actual light projection/frustum and bounded reach. Two identical
observations admit a projection; continuously changing projections keep empty
probes. Cache payload is limited to 4 MiB across 64 source/physical-light-slot
probes. Relative copy indices are replayed into the normal source-order backing
store, so GPU table replacement does not leave stale instance offsets.

Moving batches, skeletons and ineligible draws still gather live, interleaved
with the static rows. Sort reuse requires the complete current comparator-key
sequence and window length to match. Changed keys get the original stable tie
order; current counts, offsets, textures and bindings are never saved as draw
records. Camera-dependent wanted cube faces remain live. Sun/moon/GI maps,
LOD/TAA, exposed spans and explicit prepared consumers keep their prior paths.
This cache does not skip GPU shadow-map work.

The targeted GPU regression now also covers dynamic refs between static draws,
batch disappearance/re-entry, position/radius/near-plane/direction changes,
point faces under camera changes, pose and mask-texture edits during cache hits,
whole-map caching, disabled/tiny budgets, static-only zero-task hits and empty
selections. Resident accounting and budget changes apply even with no selected
local lights. Cache-control metadata uses fixed stack arrays; the existing
caster scan collects admission metadata without a separate source-ref scan.

Final Release/GCC 15.2 controls use NVIDIA Vulkan, four helpers, CPUs 0–15,
720p 4x MSAA/HDR, 300 warm-up and 1,440 measured frames, with no concurrent
build, tests, profiler or capture load. Five pairs alternate order. Direct-scene
retention is enabled in both; only local gather/sort caching changes. Reproduce
with `tools/benchmark_render3d_direct_cache.py --binary <industrial_yard>
--output <directory> --mode local-gather --repeats 5`.

| Median of five run statistics | Gather cache off | Gather cache on |
| --- | ---: | ---: |
| Render caller CPU p50 / p95 | 5.940 / 6.724 ms | 4.411 / 4.957 ms |
| Render process CPU p50 / p95 | 7.532 / 8.478 ms | 5.107 / 5.705 ms |
| Frame interval p50 / p95 | 6.226 / 11.826 ms | 5.513 / 9.715 ms |
| Actual local instance tests/frame | 33,348 | 3,712 |
| Gather-task vector payload | 950,656 bytes | 24,576 bytes |
| CPU plan payload | 0 | 491,920 bytes |
| Static accepted copies replayed/frame, p50 | 0 | 14,301 |
| Sort windows reused/frame, p50 | 0 | 115 |
| Sort entries reused/frame, p50 | 0 | 14,509 |
| Local-shadow draws/frame | 1,140 | 1,140 |

The deterministic reductions are **88.9% fewer local instance tests** and
**97.4% less gather-task storage**, for **0.47 MiB** of resident CPU plan payload
and no additional GPU retention. Four fixed spot selections hit each frame.
The warmed sort cache has zero missed windows at p50 and one at p95, allowing
dynamic caster membership to change. Per-frame geometry, shadow draws/maps/
triangles/casters, skinning, motion, uploads and retained GPU bytes agree in all
five paired routes. Three separate captures at offsets 0, 540 and 960 are
pixel-identical. Explicit device-idle waits remain zero.

The median of the five paired caller-CPU reductions is **27.4%** (range
**24.7–30.2%**), process CPU **33.6%** (**31.1–36.3%**), and frame interval
**16.4%** (**10.3–27.5%**). Absolute timings still drift between runs; clocks
and temperatures were not logged throughout. Use the paired reductions rather
than infer a controlled-clock result or combine these absolute timings with
the earlier engine-cache experiment. This is not a refreshed Unreal comparison
or a GPU timestamp measurement. Caster-list packing, depth-state interning,
uploads and command recording remain per-render work.

The final complete native 3D suite, targeted NVIDIA and llvmpipe regressions,
CPU suite and five recipe tests pass. Artifacts are `/tmp/kin-local-gather/`:
`yard/` contains final commands, CSVs, reports, captures and summaries;
`paired-summary.json` records each paired reduction; `source-identity.json`
records final source/binary hashes and the GPU renderer compile command.
`yard-initial/` preserves the preceding matrix before the no-light accounting
fix and removal of cache-metadata allocations/redundant scans. Its timings are
not mixed into the final figures above.

#### Main draw decisions and consecutive instancing

A fresh measured-window CPU profile of `74feed5` (the instance and local gather
caches enabled) finds 32.5% of main-thread user cycles in camera preparation,
22.7% in remaining local-shadow preparation, 18.0% in main recording, 8.6% in
light-grid construction, 7.0% in ordering and 4.7% in local recording. These
inclusive, period-weighted samples describe Kin's remaining CPU work, not the
fraction of the cross-engine gap explained by each stage. Profile/provenance
artifacts are `/tmp/kin-remaining-cpu/`.

The next slice retains the main pass's sorted source order, material constants,
texture/sampler compatibility classes and pipeline decisions. It filters current
visibility instead of sorting the same opaque refs, and sorts only transparent
copies/particles before assembling the same stable world/overlay order. Cached
pipeline pointers key current target/shading variants and the shared pipeline
cache's invalidation revision. Immutable submission-kind metadata also avoids
repeated batch-handle validity calls in culling and scene-table binding. Custom
shader decisions remain live.

A retained GPU material table makes different scalar PBR values compatible
within a draw. Static direct copies use their original source row to fetch the
same 64-byte constants the ordinary draw pushes. Only consecutive commands with
matching mesh, scene, pipeline, instance storage and texture/sampler bindings
merge. The compact main index list concatenates their existing camera lists in
order. Colours, IDs, source refs, picking and shadow lists are unchanged; there
is no global material regrouping. Both opaque and masked copies use the stock
shading logic, with their individual alpha cutoffs and emission values.

CPU plans cap at 8,192 refs / 4 MiB; GPU materials cap at eight sources / 4 MiB,
plus a main index buffer bounded by the admitted material rows (<=256 KiB).
Two observations admit a stable command revision. Continually rebuilt scenes
remain immediate; unavailable shader variants, exposed spans, TAA/LOD, custom,
unlit, wireframe, skinned and over-budget main draws retain their old paths.
The instance table may fit even when the independent material cap does not;
that case must still draw completely with per-draw uniforms.

The final same-binary matrix keeps the two preceding caches enabled and changes
only `--cache-draw-commands 0|1`. Five alternating Medium routes use four helpers,
CPUs 0–15, 720p 4x MSAA/HDR, 300 warm-up and 1,440 measured frames. Native Vulkan
runs through Wayland with the NVIDIA ICD; the user's separate preview was closed
before measurement. No builds, tests, profiler or readbacks run alongside the
throughput pairs. Reproduce with `tools/benchmark_render3d_direct_cache.py
--binary <industrial_yard> --output <directory> --mode draw-commands --repeats 5`.

| Median of five run statistics | Command cache off | Command cache on |
| --- | ---: | ---: |
| Caller CPU p50 / p95 | 3.522 / 4.071 ms | 3.292 / 3.915 ms |
| Process CPU p50 / p95 | 4.056 / 4.634 ms | 3.973 / 4.664 ms |
| Frame interval p50 / p95 | 3.670 / 5.137 ms | 3.523 / 5.439 ms |
| Main draws, p50 / p95 | 4,211.5 / 6,380 | 162 / 211 |
| Main material-table draws/frame, p50 | 0 | 8 |
| Local-shadow draws/frame | 1,140 | 1,140 |
| CPU plan vector payload | 0 | 894,676 bytes |
| GPU material payload | 0 | 515,520 bytes |
| GPU main-index capacity | 0 | 26,532 bytes |
| Main-index upload/frame, p50 | 0 | 16,682 bytes |
| Warm plan rebuilds / pipeline misses / material uploads | — | 0 / 0 / 0 |

Main draw p50 falls **96.2%**, preserving source/copy order. Every measured
frame's reduced draw count is explained exactly by `main_merged_draws`; geometry,
shadow maps/draws/triangles/casters, skinning, motion, instance uploads and existing
retained GPU bytes match across all five paired routes. Three decoded captures
at offsets 0, 540 and 960 are pixel-identical. Logical additional cache payload
is about **0.85 MiB CPU / 0.52 MiB GPU**, excluding fixed metadata, shader/pipeline
objects, staging and driver cycle backings. Existing instance uploads remain
66,048 bytes/frame at p50; the new compact main-index upload is counted separately.

The median paired caller-CPU reduction is **17.9%** (range **6.5–23.4%**), process
CPU **15.3%** (**2.1–21.1%**), and frame p50 **14.3%** (**4.0–20.3%**). Each paired
reduction uses the two adjacent runs' p50 values; it differs from dividing the
table's independently aggregated medians. Absolute timings drift substantially:
caller CPU in the off control rises from 3.102 to 4.436 ms across the matrix.
These controls are a new measurement window, not an additive comparison with
earlier absolute times or a refreshed Unreal ranking. Frame p95 is not
consistently improved: the median run p95 is 5.137 ms off and 5.439 ms on.
The remaining camera and local-shadow preparation still visits the
original refs, and shadow geometry is still redrawn. Fewer main commands alone
do not remove those costs.

The targeted regression passes on NVIDIA and llvmpipe, requiring exact pixels,
picks and geometry through heterogeneous scalar materials, mask cutoffs,
equal-depth surfaces, HDR/MSAA, all four stock fog/light variants, sky, AO/contact,
poses, texture writes, legacy scale/mip changes, overlays, source moves, raw
spans, tiny budgets, a material-over-budget instance table, CPU metadata that fits
while its index scratch exceeds the cap, and same-frame views.
It also checks unchanged-source zero uploads/decision misses and that a scene
resubmitted every render never rebuilds a retained plan. CPU and five recipe
tests pass.

All **271 existing renderer cases pass on NVIDIA in isolated processes**. The
combined process cannot complete in the current desktop session: GPU device
creation fails after repeated initialization, at the same point with the
unchanged pre-implementation binary. This is recorded separately from assertion
results. The warm-up case itself creates 24 devices, so its twelve unchanged
cold/warm comparisons run separately via the optional `KIN_TEST_WARM_UP_CONFIG`
index; normal unfiltered test behavior is unchanged. No assertion was relaxed.

Artifacts are `/tmp/kin-draw-commands/`: `yard/` holds the final paired routes and
captures; `paired-summary.json` records each reduction; `renderer-cases.json`,
the runner and per-case logs record the complete isolated suite. Combined-process
and unchanged-binary failure logs preserve the device issue. Source/binary/shader
hashes and validation checks are in `source-identity.json` and
`final-validation.json`. `yard-preflight/` and `yard-before-budget-fix/` are
preceding measurements, not included in the final figures above.

### Cornell GI across Kin, Godot and Unreal, 2026-11-03

The three CPU optimization commits are merged into local main at `427b594`,
after updating from `origin/main`. A fresh main build passes the CPU renderer,
direct-instance/local-shadow cache and draw-command GPU regressions.

The comparison uses one static 1280x720 Cornell view on the RTX 4080 Laptop GPU,
Vulkan release builds, TAA, no MSAA, black ambient/background and no explicit
lights: only the original ceiling panel emits. All consumers receive the same
materials, emission, camera and 82 triangles. The original interior faces are
preserved; closed 2 cm exterior wall slabs and a uniform 10x geometry/camera scale
make the shared fixture suitable for distance-field GI. Fresh Cycles references
use this same adapted asset.

| Renderer / GI profile | Frame p50 | Frame p95 | Crop SSIM, higher better | Room RGB MAE, lower better |
| --- | ---: | ---: | ---: | ---: |
| Kin stock GI | 0.950 ms | 1.214 ms | 0.934 | 7.51 |
| Godot 4.7.2 Forward+ / SDFGI | 0.526 ms | 1.051 ms | 0.730 | 38.77 |
| Unreal 5.8.4 Shipping / software Lumen | 2.483 ms | 2.571 ms | 0.971 | 8.09 |

Timing figures are medians of three rotated, serial repeats, each with 1,200
warm-up and 2,400 measured frames, CPUs 0–15. No builds or captures overlap cost
runs. Run p50 ranges are 0.851–1.047 ms Kin, 0.523–0.619 ms Godot and
2.455–2.655 ms Unreal. These are full-frame intervals, including each adapter and
presentation; unlike individual engine CPU/GPU columns, that scope is comparable.
Clocks are unlocked on a shared desktop. Cold import, shader compilation, mesh
distance-field generation and initial GI convergence are excluded.

Quality is captured separately after 2,400 warm-up frames. Scores average two
independent 1,024-sample denoised Cycles references, without exposure or color
fitting. SSIM uses the repository's 11x11 Gaussian luminance implementation on
the same room crop. MAE is in 8-bit sRGB levels on reference room pixels, excluding
black background and the clipped emitter. The denoised references agree to
SSIM 0.9991 / room MAE 0.147; raw reference seeds and their scores are retained.

Kin is about 2.6x Lumen's throughput in this configuration, retaining broad
illumination/color bleeding with bright edge leaks and wall banding. Lumen has
cleaner shadows and fewer localized artifacts; Kin's slightly lower average
color error alone does not establish better overall quality. Godot SDFGI is
fastest, with pronounced lighting/shadow discontinuities and excessive back-wall
brightness. These profiles do not have equal image quality and do not establish
a general engine ranking. This small static scene exercises GI rather than the
many-object CPU submission workload optimized above; temporal stability is untested.

Kin uses 128 rays and up to 4,096 probe updates/render, with TAA sharpness 0.
Godot uses a 0.1 m minimum SDFGI cell, four cascades, occlusion, bounce feedback
0.5 and its half-resolution GI project setting. Unreal uses Epic software Lumen,
100% screen percentage, TAA and mesh distance-field resolution scale 4, with
AO/bloom/motion blur/occlusion queries off. Initial Godot VoxelGI 128/256 bakes
underlit the panel-only fixture through dynamic-range and black-ambient controls;
those diagnostics are excluded from the main ranking, not evidence that another
VoxelGI setup cannot render Cornell correctly.

Session artifacts are `out/cornell-engines-20261103/`: `index.html` and
`comparison.png` show the images; `campaign.json`, `speed-summary.json`,
`quality-summary.json` and `final-summary.json` record commands, checks, effective
settings, binary/content hashes and scores. Scene preparation, the release Godot
pack, Unreal adapter sources/build logs, per-frame logs and both raw/denoised
reference seeds are retained there.

### Experimental Cornell GI quality/cost, 2026-11-03

`exp/gi-yard` and `quality/gi-response` both identify `6d75304`. Its better
Cornell result comes from a generated static-source/residual-history prototype,
not the branch's default renderer. The reviewed generator chain was rebuilt as
isolated archive overlays on optimized main `63e1fef` (renderer `427b594`).
Both source quadrature tiers include the immutable emission-lookup SSBO fix.
No shipping GI code or defaults were changed for this study.

The fixture, authored emission, camera, TAA, raster sample count and two Cycles
references are identical to the engine comparison above. Fresh serial cost runs
use three rotated repeats, each 1,200 warm-up + 2,400 measured frames at 720p,
CPUs 0–15, release Vulkan, RTX 4080 Laptop. Captures/readbacks and memory sampling
are separate from throughput. Quality averages phases 2,400/2,573 against both
1,024-sample denoised reference seeds, with the same room crop/mask and no fitting.

| GI configuration | Frame p50 | Run p50 range | Crop SSIM ↑ | Room RGB MAE ↓ | Process VRAM |
| --- | ---: | ---: | ---: | ---: | ---: |
| Optimized main | 1.059 ms | 0.856–1.060 ms | 0.9338 | 7.524 | 483 MiB |
| Experimental, 32 source points | 5.181 ms | 3.930–5.567 ms | 0.9781 | 2.993 | 534 MiB |
| Experimental, 128 source points | 17.845 ms | 14.809–17.946 ms | 0.9784 | 2.987 | 534 MiB |

The 32-point tier removes most of main's wall bands and bright edge leaks,
reducing room color error by 60.2%, at 4.89x main's median frame cost. Its interval
is 71.0% below 128 points with only a small aggregate quality difference. The
two tiers differ by a mean 0.0288 RGB levels out of 255 across the full frames;
the largest channel difference is eight levels, localized near shadow edges.
For this static scene, 32 points is the useful experimental tier.

The prototype separates authored emission from feedback, improves probe
reconstruction, keeps signed indirect residuals in history and recomposes current
direct source lighting after history. It integrates the actual emissive triangles
with visibility against the frozen opaque triangle cache. Half-resolution source
integration/visibility, extra transport treatment and the composite dispatch add
GPU work. Probe budgets remain 51,984 probes, 4,096 updates x 128 rays/render,
with 3,326,976 voxels. After subtracting main's interval, extra cost grows 4.07x
for 4x the source points, supporting source integration as the next target.
This does not assign individual GPU-pass durations.

Median CPU render-call scopes are 0.124/0.182/0.229 ms for main/32/128;
presentation/backpressure scopes are 0.883/4.946/17.456 ms. Most added interval
appears as GPU backpressure rather than CPU submission. The next experiment
should cache static source irradiance/visibility in surface or world space with
explicit invalidation, or adapt source samples around penumbrae. These are
proposals, not measured improvements. The branch's earlier BVH/full-table-buffer
pilots did not establish a win.

VRAM is per-PID NVML from a separate 1,200 + 2,400-frame run per tier, excluding
the last buffered progress block. Main/32/128 have 44/230/739 bounded samples;
GNOME Shell is the only other observed GPU PID. The lookup buffer is 30,624 bytes
for 696 emission entries. The old pre-SSBO prototype is not measured here.

Unlocked clocks and thermals vary substantially: startup observations span
77–86 C, and the memory lane holds around 1,290 MHz graphics. Run ranges are
retained; exact timings and ratios are exploratory, not fixed-clock claims.
These results exclude source-cache construction, shader compilation and cold
convergence. The source/opaque geometry caches are immutable; arbitrary scenes,
dynamic sources/geometry, motion and cuts are untested by this campaign. Two
static capture phases do not establish temporal stability. Earlier Godot/Unreal
times remain a separate campaign, not part of this fresh speed ranking.

Artifacts: `out/cornell-gi-tradeoff-20261103/index.html` and `comparison.png`;
`summary.json`, `quality-summary.json`, `campaign.json`, `build-record.json` and
`memory-summary.json` retain metrics, phases/seeds, commands, source/binary/shader
hashes, clocks and bounded windows. The isolated builder, runners, staged source
caches and raw per-frame/process logs are retained there. Five lookup-table and
nine residual/integration CPU contracts pass. This is a static-scene study, not
production qualification of the generated overlays.

#### Lower source-point budgets

A follow-up compares 4, 8 and 16 total source points against freshly measured
main/32-point controls, using the identical scene, source radiance, visibility,
history and probe-ray budgets. Gaussian rules per emitting triangle are 2x1,
2x2, 4x2 and 4x4 for 4/8/16/32 total points. The generic rectangular-rule
generator reproduces the previous 32-point helper byte for byte. Every rule
preserves source area, power and first moments with positive finite weights;
only the source-quadrature helper changes between experimental tiers.

| Configuration | Frame p50 | Run p50 range | Room SSIM ↑ | Room RGB MAE ↓ | Shadow-crop SSIM ↑ |
| --- | ---: | ---: | ---: | ---: | ---: |
| Main | 1.060 ms | 1.059–1.179 ms | 0.9338 | 7.524 | 0.9696 |
| 4 points | 2.024 ms | 2.024–2.211 ms | 0.9754 | 3.157 | 0.9841 |
| 8 points | 2.506 ms | 2.505–2.507 ms | 0.9767 | 3.019 | 0.9884 |
| 16 points | 3.511 ms | 3.503–3.512 ms | 0.9776 | 3.008 | 0.9900 |
| 32 points | 5.555 ms | 5.515–5.557 ms | 0.9781 | 2.993 | 0.9914 |

The three lower tiers reduce median frame cost by 63.6%/54.9%/36.8% versus the
fresh 32-point control. Four points retains broad illumination improvements over
main but creates conspicuous separated shadow bands. Eight has finer bands;
16 has cleaner transitions and is visually closer to 32, with residual stepping.
Eight is a useful performance candidate and 16 a cleaner quality candidate.
Aggregate SSIM understates these localized shadow differences. Maximum channel
differences from 32 are 31/21/17 levels for 4/8/16; room pixels differing by over
eight levels on any channel are 1.00%/0.37%/0.14%.

Protocol is three serial cost repeats in varied orders, each 1,200 warm-up plus
2,400 measured frames, separate captures at phases 2,400/2,573 against both
denoised Cycles seeds. The room mask/crop remain unchanged. A fixed shadow crop
at [340,340,595,690] covers the left wall and floor behind the tall block; its
SSIM is an additional diagnostic, not a general quality score. Lower-budget
bands are deterministic spatial quadrature error, not stochastic noise.
Clock/thermal drift affects exact timing; main and four points slow in the last
repeat. Static-source/geometry scope and untested motion/cuts remain unchanged.

Artifacts are `out/cornell-gi-low-budget-20261103/`: `comparison.png`, enlarged
`shadows.png`, `index.html`, source builders and runner, `summary.json`,
`quality-summary.json`, `campaign.json`, `build-record.json`,
`quadrature-validation.json` and `reconstruction-validation.json`. All 25 jobs
pass output/settings/count checks. Area/first-moment/control-reproduction
checks, exact regenerated helpers, unchanged backend, declared sample counts
and binary/SPIR-V hashes pass for all three new tiers. No production GI code
or defaults are changed.

#### Zero, one and two source points

A further fresh campaign tests zero explicit source quadrature, one area-weighted
panel centroid and one centroid per emitting triangle (two total). Main and the
same 8-point binary are freshly measured controls. One/two preserve source area,
power and first moments; the one-point rule requires the verified uniform,
coplanar panel with common normal and sidedness.

Zero has two explicitly different meanings. Simply removing quadrature while
still subtracting primary emission from probe rays leaves the room black apart
from the panel. That literal-zero ablation is captured at both phases and is
excluded from the timing ranking. The usable zero-point fallback keeps primary
emission in the probe rays, so probes carry full source and bounce lighting;
the explicit source helper returns constant zero without surface/visibility work.
This fallback changes the trace subtraction policy as well as the helper, so it
is an algorithm variation rather than a sample-count-only comparison.

| Configuration | Frame p50 | Run p50 range | Room SSIM ↑ | Room RGB MAE ↓ | Shadow-crop SSIM ↑ |
| --- | ---: | ---: | ---: | ---: | ---: |
| Main | 1.054 ms | 0.888–1.054 ms | 0.9338 | 7.524 | 0.9696 |
| 0, full-probe fallback | 1.439 ms | 1.248–1.440 ms | 0.9728 | 3.263 | 0.9824 |
| 1, panel centroid | 1.705 ms | 1.588–1.730 ms | 0.9701 | 3.639 | 0.9693 |
| 2, triangle centroids | 1.806 ms | 1.626–1.814 ms | 0.9730 | 3.250 | 0.9780 |
| 8 points | 2.448 ms | 2.399–2.510 ms | 0.9767 | 3.019 | 0.9884 |

The zero-point fallback costs 41.2% less per frame than 8 points and 36.6% more
than main, with 56.6% less room RGB error than main. It keeps most broad visual
improvements and smoother probe shadows, with blurred contacts and some residual
edge leakage. One point creates a hard shadow and is worse than zero on both
cost and aggregate error. Two points create split shadows, adding cost for little
aggregate improvement over zero and worse shadow-crop SSIM. The zero fallback is
the useful new candidate for further work. It still retains the prototype's
paired history resources and source-composite pass; removing that overhead is
unmeasured. The literal-zero ablation has room SSIM 0.0087 and MAE 59.344.

All 27 jobs pass count/settings/output checks: 15 serial cost runs (three varied
orders, 1,200 warm-up + 2,400 measured frames) and 12 separate quality captures
(phases 2,400/2,573, including the ablation). The two 1,024-sample denoised Cycles
seeds, crop/mask, fixed shadow ROI and no-fitting policy remain unchanged.
All cases retain 51,984 probes, 4,096 updates x 128 rays, TAA and 82 triangles at
720p on RTX 4080 Laptop/Vulkan. Startup/thermal drift affects exact timings.
These are static-source/geometry results; motion/cuts and general runtime
invalidation remain untested. No production GI code or defaults change.

Artifacts: `out/cornell-gi-min-budget-20261103/` contains the full-room and enlarged
shadow contact sheets, HTML report, literal-zero image, per-phase/seed metrics,
commands/frame telemetry, source builder and validation runner. Exact source
reconstruction, declared helper/trace changes, zero-path behavior, source power/
centroid checks and binary/SPIR-V hashes pass for all four generated variants.

### Independent GI changes on main, 2026-11-04

Four separate archive overlays isolate the proposed portable changes on the
unchanged CPU-optimized renderer: resolve-only visibility confidence, GI/TAA
history-reset propagation plus skipped/failed-path invalidation, per-world-probe
quaternion rotations, and angular reconstruction correction. No candidate includes
another candidate, the frozen Cornell geometry/emission caches, source quadrature,
guided sampling, or the residual-pair/composite pipeline. Main's ratio irradiance
estimator stays unchanged in every case. Angular metadata uses spare atlas alpha
channels, preserving the original probe-age texel; no new allocation/pass is added.

Same 720p Cornell fixture and two reference seeds as above; two static phases,
no fitting. Fresh mains-powered timing: three varied-order serial repeats, each
1,200 warm-up + 2,400 measured frames, CPUs 0–15, RTX 4080 Laptop/Vulkan/TAA.
All 15 accepted cost jobs record mains online before and after. An earlier
matrix crossed into battery/P8 at 210/405 MHz and is rejected in full. A separate
battery attempt was interrupted when the user restored mains; it is excluded too.
Clocks remain unlocked and run-order/thermal variation is substantial.

| Independent candidate | Frame p50 | Run p50 range | Room SSIM ↑ | Room RGB MAE ↓ |
| --- | ---: | ---: | ---: | ---: |
| Main | 1.054 ms | 0.858–1.172 ms | 0.9338 | 7.524 |
| Visibility guard | 1.053 ms | 0.861–1.184 ms | 0.9338 | 7.514 |
| History reset | 0.957 ms | 0.861–1.057 ms | 0.9338 | 7.524 |
| Probe rotations | 1.216 ms | 0.979–1.219 ms | 0.9328 | 7.457 |
| Angular correction | 1.185 ms | 1.068–1.228 ms | 0.9311 | 7.861 |

The guard and reset remain small correctness candidates, not major settled-image
upgrades. In a direct GPU dispatch with constant-bright probes and fully occluded
moments, main returns raw irradiance/pi 1; the guard returns the physical target 0
while preserving coverage 1. Visible constant light stays 1, darkness stays 0 and
missing support retains coverage 0. Applying the guard to bounce feedback is not
tested here. Fifteen GPU resolve cases include these positive/negative controls
and an analytic angular field. Angular correction changes the latter from
1.36133 to 1.39941 for target 1.4, but worsens real Cornell MAE by 4.5%; it should
not be promoted alone on this result.

The reset-only images match main RGBA exactly at both static phases; its shaders
and steady GPU work are unchanged. Its lower measured median is not evidence of
a speedup: run ranges overlap and clocks/order drift. A diagnostic-only link
observes the actual resolve history-enable uniform over twelve rendered frames.
Main incorrectly retains history after same-pose reset, camera cuts and TAA
re-enabling (frames 1/3/6/10); the candidate matches all expected flags, including
ordinary continuation, toggles and odd-size resize. Failure invalidation paths
are source-checked, not exercised by injected allocation/pipeline failures.

Rotations trade mixed still accuracy for better short stationary stability.
Two 64-frame holds at phases 2,400/2,573 improve in both phases. Mean RMS output
variation in RGB8 levels is 0.4759 to 0.3185 over the room (33.1% lower), 0.6787 to
0.3862 in the fixed upper-wall crop [500,170,780,290] (43.1% lower), and 0.6048 to
0.2815 for its 8x8 block means (53.5% lower). These measurements include TAA jitter
and probe variation; they do not establish pure GI-noise or moving-camera/light
response quality. Extra rotation arithmetic costs GPU work; the timings above
remain exploratory. Keep rotations as an optional quality candidate for broader
testing rather than selecting a universal default from this one scene.

The large zero-point prototype gain is not reproduced by any isolated candidate;
remaining emitter/transport changes and interactions still need isolation.
Artifacts are `out/gi-main-ablation-20261104/`: the HTML/image report, per-phase/
seed metrics, 25 accepted quality/cost jobs, four stationary clips (256 captures),
15 GPU resolve controls, two twelve-frame history sequences, rejected timing
attempts and all source/build/command/hash receipts. Every non-target archive
member matches main; regenerated sources, binary/SPIR-V hashes and both reset
image identities pass. No shipping source or defaults change in this study.

#### Visibility/history port

The selected fixes are implemented on `opt/gi-visibility-history`, from main
`ce3d4e6`. `KIN_GI_RESOLVE_VISIBILITY` enables confidence attenuation only in the
final resolve. Coverage/readiness stay unchanged, with no added ray, texture,
buffer or pass. All 127 other embedded shader payloads, including feedback,
match the preceding main-based visibility experiment exactly.

GI history now observes TAA reset, and a completed-render guard invalidates it
whenever resolve was skipped or the frame failed. Central invalidation also
catches a main-pass failure after GI resolve had already recorded successfully.
This replaces the prototype's per-return invalidation approach without adding
renderer state or a public API.

New `kin-render3d-gi-resolve` GPU checks cover visible/blocked/dark/missing-support
and background fields, including unchanged coverage for occluded receivers.
On Linux with GNU/Clang, `kin-render3d-gi-history` observes the actual production
resolve uniform through test-only linker wrappers. It checks resets, cuts, TAA/
GI/probe toggles, odd resize, two views, an injected resolve-pass failure and an
injected later frame failure. The new checks and CPU renderer CTest pass; all
17 selected existing GI/TAA/prepass GPU callbacks pass in isolated processes.
A fresh 720p Cornell capture matches the evaluated resolve-only guard RGBA
exactly. Validation artifacts are `out/gi-port-validation-20261104/` and
`/tmp/kin-gi-port-{ctest,resolve,history}.log`. No additional performance gain is
claimed for this implementation port.

### ECS transform and extraction baseline

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


## Public bloom and view output

The native stock renderer and public `render_bloom()` / `render_output()` use the same fullscreen recorders. Bloom remains the existing threshold extraction and two across/down nine-tap blur rounds: five draws, at caller-selected matching intermediate extents. Output remains the existing optional bloom composition, premultiplied/additive alpha handling, SDR exposure/ACES and encoding, HDR linear/unexposed passthrough and stable gamma dithering. This extracts the stock algorithm without choosing a new quality preset or claiming a faster filter.

`SurfaceTargets3D::linear_colour` adds an explicit sampled RGBA16F attachment alternative to the existing working-format target. Exactly one colour destination is required. The example can therefore produce HDR toon colour, generate bloom and finish into the current SDR target before ASCII, with no global colour-space switch. During a later working-space change, its exact-format HDR/shadow attachments retain their handles; only working-colour targets need replacement. Caller-owned intermediates are declared in the graph and use its existing lifetime planner; no private view/history is created for these operations.

Tests compare public surface/bloom/output with complete stock HDR rendering byte for byte for zero/positive bloom, two exposures and gamma, sRGB and HDR working formats. Independent numeric checks cover ACES/exposure out of partial coverage, encode-only output, zero-coverage additive colour, bloom extension over transparency, threshold rejection, stable dither within one stored step, and preservation of linear values above one in HDR working output. Validation covers numeric options, formats, actual sampler/attachment usages, arrays, size mismatch, feedback, missing inputs and foreign-device resources. Eight interleaved frames preserve independent stock TAA pictures/reset state; changed bloom-input revisions reset material-driven TAA as intended.

An eight-frame warmed post-processing interval requires zero new Kin-owned GPU resource handles beyond separately counted upload-staging maintenance and no staging-capacity growth. The extended surface graph independently requires zero total new GPU handles, stable compilation count and exact optimized/reference pixels. These gates exclude readback from the measured interval. They do not measure a frame-time speedup, host allocation freedom, native heap aliasing or SDL/driver cycle backings. Cold pipeline creation remains possible on first use.

A regression caught a CPU upload overtaking a queued public output write (`/tmp/kin-postprocess-tests2.log`, red-replacement assertion). Public output and surface destinations now publish the same pending-access guard as sampled inputs. Later CPU uploads submit the earlier reads/writes before updating, expiring prepared handles without an explicit idle wait. GPU producers remain ordered in-frame. Tests verify both sampled-input updates and replacement of queued SDR output/HDR surface writes, as well as dropping application handles before submission. Stock finishing failures now propagate and restore 2D batch recording state instead of silently reporting success.

Final focused results are `/tmp/kin-postprocess-hardware-final.log` (eighteen passes and three inherited NVIDIA/X11 live-device lifecycle skips) and `/tmp/kin-postprocess-software-final.log` (all sixteen pass, including foreign-device post-processing inputs/targets). Full native 3D dispatch passes in `/tmp/kin-postprocess-full3d.log`. Browser smoke passes with explicit unsupported-operation checks in `/tmp/kin-postprocess-web-smoke.log`. The standalone 2D-only graph compiler and the 197-file formatting gate pass. Four decoded RGBA captures remain byte-identical to predecessor `e9b833c`: Cornell/front and motion/still, pan and close at 320 x 240 with AO, contact shadows and TAA, using the reference command above. Both capture sets are preserved under `/home/bamedeo/kin-build/rendering-foundations-native/postprocess-captures/{before,after}`.

A temporary-filesystem quota interrupted formatting, followed by removal of the temporary source worktree. The committed baseline and saved edit scripts allowed complete recovery; tracked changes were checked after restoration. Source and native build now live at `/home/bamedeo/kin-wt/rendering-foundations` and `/home/bamedeo/kin-build/rendering-foundations-native`, with old `/tmp` aliases retained for build configuration. The existing local-only GCC/sol2 compatibility hook was restored outside the repository. Main's planning edits and the independent lighting worktree were not changed.

## Opaque/transparent surfaces and particles

Public `SurfaceStage3D` selection shares the stock mesh/sprite recorder and uses one prepared mesh payload across opaque and transparent consumers. Visibility/LOD, particle selection and composition sorting still run per consumer; this stage does not establish elimination of every CPU scan. Combined, in-place split and versioned split output is byte-identical to stock for the exercised hard-particle scenes across gamma, linear SDR and HDR working spaces. Both working-colour and independent HDR attachments are exercised. Soft-particle pictures agree within one 8-bit output step. Cases include cross-material/per-instance glass, alpha/additive/premultiplied sprites, procedural weather, overlays, masked skinning, retained transparency, wireframes, orthographic projection and CPU/GPU cluster cuts. Explicit sun/spot/point map tests use custom local array layers and require visible sprite-shadow differences; null bindings never borrow preceding stock maps.

Transparent consumers load existing colour/depth and leave depth unchanged. A distinct `colour_input` performs one exact-format GPU colour copy while preserving its source. This gives the existing single-writer graph compiler explicit logical versions; it is not native heap aliasing or copy-free attachment versioning. The example's additional full-size RGBA16F colour is 2.1875 MiB at 640 x 448, before driver overhead/backings. Manual in-place calls avoid that copy and second colour allocation. Soft intersections need a separate sampled D32F world-depth producer; no hidden prepass or attachment allocation is added.

The coloured-background regression failed with a largest output difference of 61 before the HDR clear correction (`/tmp/kin-surface-stages-tests1.log`). The first split test caught pre-existing per-copy composition items leaking into the opaque stage during extraction (`/tmp/kin-surface-stages-tests2.log`); composition generation now honours the stage while retaining compatible shared upload ordering. A stronger TAA test then exposed a stock issue: colour can split a sprite source around glass, but coverage replays each source once. Comparing colour-run and coverage counts caused false history resets (`/tmp/kin-surface-stages-tests6.log`). Comparing against source batches restores accumulation. The final test requires accumulating stock histories, exact interleaved-view pixels and independently recorded public particles.

Queued sprite texture release and later CPU updates are tested, including particle-only preparation and adding live particle sources after opaque rendering. Public calls preserve stock particle history/caster ownership and use only explicit soft/shadow inputs. Invalid stage/option combinations, feedback, incompatible soft depth and foreign-device textures/colour/depth fail explicitly. An eight-frame warmed interval checks zero repeated bound transformations, compatible shared uploads, no device idle waits, no new GPU resource handles beyond separately counted staging maintenance and no staging growth. The graph example independently checks exact optimized/reference pixels, cached compilation and zero new GPU handles. These are correctness, work and allocation checks, not measured frame-time speedups or SDL/driver backing-allocation measurements.

Focused hardware results are `/tmp/kin-surface-stages-hardware-final.log`: nineteen passes and four inherited NVIDIA/X11 live-device lifecycle skips. Software Vulkan passes all eighteen selected tests, including live foreign-device checks (`/tmp/kin-surface-stages-software-final.log`). Browser smoke passes explicit unsupported-stage/particle checks and the existing stock profile (`/tmp/kin-surface-stages-web-smoke.log`). The standalone 2D-only graph compiler and the 198-file formatting gate pass. Four decoded stock RGBA captures remain byte-identical to predecessor `ec58ead`: Cornell/front and motion/still, pan and close at 320 x 240 with AO, contact shadows and TAA, using the reference command above. Captures are `/home/bamedeo/kin-build/rendering-foundations-native/surface-stages-captures/{before,after}`.

An exploratory all-target build still encounters the installed sol2 dependency's GCC 15 `optional<T&>::construct` template-body errors in unrelated content/Lua tests (`/tmp/kin-surface-stages-build9.log`). The focused rendering targets build successfully (`/tmp/kin-surface-stages-build10.log`), using the existing local-only compatibility flags for the engine's scripting targets. No dependency workaround is added to the repository. The preceding unmodified full 2D suite's DPI failure is unchanged; no full-project build or full-2D pass is claimed.

The complete native stock 3D dispatch passes in `/tmp/kin-surface-stages-full3d-final.log` (155 seconds). Final logs, including the reproducing failures and build limitation, are also preserved under `/home/bamedeo/kin-build/rendering-foundations-native/surface-stages-logs`.

## Compiled attachment versions

Attachment clear/discard/load and store/discard declarations now participate in dependency, initialization, lifetime and resource-slot compilation. Read-only depth testing is separate from sampled reads. At a loaded version's last attachment use, compatible transient storage is handed to its successor; later readers, same-pass sampling, imports and pinned contents require separate storage and a pre-callback preservation copy. This extends the existing compiler rather than compiling opaque callback internals or aliasing native heaps. Existing generic reads/writes remain compatible.

The 640 x 448 surface example now makes zero opaque-to-transparent colour copies and shares the two logical RGBA16F versions in one allocation. Relative to the separate-copy reference, this removes one 2,293,760-byte (2.1875 MiB) texture payload and one full colour copy per execution. `CompiledResourceGraph` reports each decision/reason, handoff count, copy records/bytes and graph-owned slot payload. Backend copy counters agree with compiled records across working spaces. Physical descriptor-ledger growth matches a freshly prepared graph's slot payload. `peak_live_payload_bytes` remains a logical-version metric, including both versions at a handoff; it must not be read as physical residency. Driver cycle backings, alignment, transient retirement, measured bandwidth and a frame-time speedup are not established by these checks.

Compiler tests compare every observed intermediate against an unaliased reference over 300 reproducible branching graphs. Other cases cover chains, same-pass forks, exports/persistence, imported histories, same-pass sampling, culled readers, read-only depth, incompatible/invalid declarations, discarded contents, missing copy capability, incomplete callback tables, budgets, byte-counter overflow and structural cache invalidation. The standalone compiler still builds/runs without experimental 3D. AddressSanitizer/UndefinedBehaviorSanitizer pass (`/tmp/kin-attachment-versions-sanitized.log`).

Native GPU tests compare optimized/copy colour and depth versions, later readers and pinned old images across gamma, linear SDR and HDR; resize retains old output handles. Warmed frames keep cached compilation, zero copy requests in the handoff path, no new GPU resource handles beyond separately counted staging maintenance and no staging-capacity growth. Tests cover direct D32 array copies across layers, raw-copy lifetime, source/destination CPU replacement ordering and foreign devices. Re-importing a previous managed output under the same cached plan detaches its physical destination and preserves the old pixels. That conservative history path may allocate each frame; applications should use bounded ping-pong storage/resource sets when stable history allocations are needed.

The native full stock 3D dispatch passes in `/tmp/kin-attachment-versions-full3d-final.log` (164 seconds). Focused hardware results are twenty passes and five inherited NVIDIA/X11 live-device lifecycle skips (`/tmp/kin-attachment-versions-hardware-final.log`). All twenty-one selected software Vulkan tests pass, including the foreign-device probes (`/tmp/kin-attachment-versions-software-final.log`). Browser smoke passes a loaded 2D colour handoff, preserved pixels and explicit rejection of copy-dependent plans, alongside the stock 3D profile (`/tmp/kin-attachment-versions-web-smoke2.log`). The 199-file 3D formatting gate and whitespace checks pass; touched graph compiler/binding and compiler test files were also formatted with clang-format 18.

Four decoded stock RGBA captures remain byte-identical to predecessor `5cc8a4e`: Cornell/front and motion/still, pan and close at 320 x 240 with AO, contact shadows and TAA, using the reference command above. Captures are `/home/bamedeo/kin-build/rendering-foundations-native/attachment-versions-captures/{before,after}`. Build and validation logs are preserved under `/home/bamedeo/kin-build/rendering-foundations-native/attachment-versions-logs`. The earlier unrelated sol2/GCC 15 all-target build and full-2D DPI limitations remain documented; no full-project build or full-2D pass is claimed.

## Automated multi-scene evaluation

The new [evaluation tools](../tools/eval/README.md) were exercised with the native
Release/GCC 15.2 build on the RTX 4080 Laptop GPU. Audio and TTF were enabled for the
optional Bistro target; the existing local sol2/GCC compatibility hook remained
outside the repository. Both labels deliberately used the same binary: this is
measurement-system acceptance, not an engine speedup comparison.

A 160x96 smoke suite covers Sponza/nave, a four-frame moving Sponza/TAA clip,
Industrial Yard's three-frame replay, and a two-frame static Bistro clip after
600 settle frames. Performance uses three warm-up/six measured frames; Cycles uses
two independent eight-sample seeds. All 27 jobs pass, including per-frame CPU
records, captures, reviewed Sponza/Bistro references and FLIP/JOD scoring. Yard's
reference is explicitly its baseline capture, not Cycles. These short runs and
noisy references establish tool operation, not calibrated quality or speed.
The shared cache reuses all three reviewed references after the native binary
changes. Complete job inventories and frozen inputs are retained with the report.

Direct numerical compatibility checks against the existing Bistro scorer pass
for both display transforms (exact RGB8 output), FLIP means, still/clip JOD,
detail, static shimmer/stepping and moving temporal error. Scalar FLIP maps are
requested explicitly rather than interpreting the library's default colourized
map as error values. Linear EXR input without a reviewed display transform is
rejected. A virtual-environment interpreter retains its symlink path; resolving
it to the system interpreter would silently bypass the metric environment.

Persistent Sponza stress prewarms features for 130 frames, settles base rendering
for 600 frames, exercises two resize/TAA/AO phases of 130 frames, then restores
and settles for 600 frames: 1590 measured records per variant. Initial and final
owned payload plateaus are stable and equal at 410,974,752 bytes, with a 4096-byte
tolerance. This excludes hidden driver backing/residency and does not cover reload
or multiview lifetime tests. An earlier unconditioned baseline contained about
330 MB of expiring staging/pool slack; the final preconditioned test removes that
allowance before comparison.

Acceptance artifacts live outside Git at
`/home/bamedeo/kin-build/rendering-foundations-native/evaluation-validation/`:
`acceptance/report/index.html`, `final-stress/report/index.html`, and
`compatibility/result.json`. Run manifests record exact binaries, provider/code
hashes, camera/frame IDs, display assumptions, packages and render context.
Missing middle-frame repair, interrupted/incomplete-stage recovery, changed-input
resume refusal, arbitrary command adapters and shared truth reuse are covered by
the Python contract tests. Prior job records are replaced by their logical stage
when repaired, so stale unsupported results and duplicate scores cannot poison a
successful resumed report.

The full native stock 3D dispatch passes (`/tmp/kin-eval-full3d.log`, 142 seconds),
as do runtime, owned-session and scene checks. Browser rebuild/package and the
stock WebGL2 smoke pass (`/tmp/kin-eval-web-smoke.log`, Chromium/SwiftShader).
Four ordinary Cornell/motion RGBA captures match the preceding attachment-version
captures byte for byte. Benchmark mode separately verifies exact configured
warm-up/frame counts with TAA, avoiding the legacy capture settling extension.
The 199-file 3D format gate passes. This is not a full-project/all-target build.
## AO/prepass follow-up

The first AO profiling pass separates geometry, depth-chain construction, GTAO horizons,
both blur directions and reconstruction with nested GPU groups. A parent range includes
its children; summing them double-counts GPU time.

The retained rendering change reconstructs the existing half-resolution AO mode using
full-size depth and normals. It adds one dispatch and an R8 target, with original-sample
and zero-weight-neighbour shortcuts. Full-resolution output remains byte-identical at
1280×720 and 2560×1440; full-resolution defaults and sample counts are unchanged.

Three interleaved Bistro runs per configuration, RTX 4080 Laptop GPU, eight seconds
warm-up and eight seconds measured, same static afternoon view, GI/TAA on, VSync off:

| AO mode | 1280×720 FPS | 2560×1440 FPS |
|---|---:|---:|
| Full resolution | 208.2 | 78.8 |
| Original half resolution | 222.2 | 88.3 |
| Reconstructed half resolution | 218.0 | 88.7 |

Thermal/clock variation is substantial, especially at 720p; these are exploratory mode
comparisons. Reconstructed half AO observed +4.7%/+12.6% versus full AO, and -1.9%/+0.5%
versus old half AO. This is not a throughput improvement of the full-quality path.
Against the same two 512-sample Cycles seeds at 720p, full/old-half/new-half FLIP is
0.176637/0.178040/0.178128 and JOD is 7.469986/7.424574/7.426322. The new reconstruction
improves surface separation, not every aggregate metric. Half resolution remains an
explicit quality tradeoff; moving-camera and additional-scene checks precede any preset change.

Math-only horizon simplifications/unrolling had no convincing win. A shared-memory blur
was about 35% slower per blur pass at 1440p despite fewer unique texture fetches. Both
experiments were removed. Hardware counters would be needed to explain the cache/barrier
behaviour. The dominant full-quality AO target remains the horizon search.

Native build artifacts under `evaluations/bistro-ao/` retain the report, commands,
binary hashes, raw frame windows, quality jobs, GPU traces and rejected experiments.

### Full-resolution GTAO horizon/locality screening

Ten further candidates were screened at 1280×720 and 2560×1440 using a diagnostic
linker shim, with baseline and candidate GPU pipelines alternating inside one
process. Blocks contain 32 dispatches; the first four are excluded and complete
28-sample blocks are compared with neighbouring baseline blocks. This reduces
thermal drift between processes but does not remove clock/scheduling variance.

No runtime change was retained. The 16×4, 32×2 and 4×16 execution layouts showed
no dependable cross-resolution gain. Conservative rejection of taps beyond the
AO radius by their z-distance was about 5% slower in the 1440p horizon range.
Degree 1–4 inverse-cosine fits and `glslc -O`/`-Os` produced only small or mixed
changes. Apparent 1440p improvements were generally 1–2% of a 2.4–2.5 ms diagnostic
GTAO range (about 0.02–0.04 ms), with comparable variation in the controls.

The numerical candidates were rejected on timing, before full-scene still/video
quality qualification. No full-quality FPS gain or definite hardware bottleneck
is claimed. Resolution, sample budgets, production shaders and previous AO work
remain unchanged. A larger AO gain needs a separately evaluated algorithm-level
prototype; main material shading remains another measured optimization target.

The consolidated report and paired-block plots are under
`evaluations/gtao-study/`. Exact source, SPIR-V, binaries, commands, Vulkan traces
and individual range exports are under `gtao-locality`, `gtao-horizon`,
`gtao-approx` and `gtao-compiler` beside that directory. All shader/pipeline
selection hooks are diagnostic prototypes outside the engine.
