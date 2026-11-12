# Rendering foundations validation

The implementation branch starts at lighting snapshot `77b76c8`. Correctness, CPU tracking, memory accounting, public graphs and stock-phase scheduling are delivered in separate commits through `e1226c2`. Main's planning edits and the lighting worktree's uncommitted GI subvoxel changes were preserved. The [progress log](rendering_progress.md) distinguishes this delivery from the full [pipeline plan](rendering_pipeline_plan.md).

## Upload staging and packing experiments, 2026-11-12

**No renderer change was retained.** This investigation split the upload hotspot, tested serial batching and
worker packing, checked output parity, then rejected the implementation after the final design did not show
a repeatable Hard CPU benefit. The renderer and tests were restored to `cb0d7744`; the rebuilt Yard benchmark
is byte-identical to its starting binary. The branch remains `perf/render3d-cpu-parity`.

### What the finer profile found

**All measurements are CONTENDED.** Blender and every external workload stayed running. The same baseline
binary's two full-route controls measured caller-CPU p50 **2.260 / 2.275 ms**, process-CPU p50 **2.851 / 2.832 ms**
and frame p50 **4.315 / 4.756 ms**. Different runs and variants are kept separate below.

The temporary CPU-only probe covers shared texture/storage staging, excluding the separate vertex/index
upload ring. It measures space/allocation, mapping, fill callbacks, raw copies, unmapping and copy-command
recording, tagged by upload source and byte count. It uses thread-CPU and wall clocks every sixteenth update:
90 measured samples per 1,440-frame route after 300 configured warm-up frames. Four full-route profiles cover
Easy, Medium and two Hard runs. Scoped parents and per-frame exclusive arithmetic are checked; helper-thread
CPU is not separately attributed. The probe uses no synchronous GPU timestamp waits. Its three tier images
and non-timing counters match the frozen binary at phase 541.

Hard submits **127 retained-table writes per frame**, totaling **635,008 bytes** (4,961 128-byte records).
The other explicit batch is streamed under the existing table limit. The following are diagnostic caller-CPU
**means in ms** from the two Hard profiles, not normal-throughput percentiles:

| Scope | Hard range |
| --- | --- |
| Retained-table fill callbacks | 0.112–0.112 |
| Retained-table copy-command recording | 0.089–0.089 |
| Streamed main-instance fill callback | 0.092–0.093 |
| All raw staging copies | 0.029–0.029 |
| All mapping scopes | 0.024–0.025 |
| All unmapping scopes | 0.022–0.023 |
| Entire staged-upload scopes | 0.623–0.639 |

Rows overlap and must not be added. The entire staged-upload scopes include **0.199–0.209 ms outside their
named child phases**, largely the inserted scope transitions, clocks and event bookkeeping. Tiny per-call
scopes also include a clock-read floor. The inspected SDL Vulkan backend already holds transfer memory mapped;
its unmap operation is a no-op, and an uncycled map returns the active pointer. Cycling the first write can
still select another backing. Therefore the reported map/unmap scopes are not a 0.05-ms opportunity simply
removable by eliminating real OS mappings.

**Correction to the preceding recommendation:** the earlier 1.0–1.2-ms staging diagnostic is not a reliable
estimate of normal upload cost. It included many nested per-call timers and was collected under different
load. The finer profile shows smaller individual costs and material instrumentation overhead; those raw
diagnostic totals must not be used as projected production gains.

### Implementations tested

1. **Serial batching:** queue whole retained-table updates, pack them through one staging fill, unmap, then
   record each buffer copy in its original order. This amortizes staging setup and callback allocations while
   keeping each target buffer, cycle bit and payload unchanged. It did not improve Hard CPU cost.
2. **Parallel batching:** flatten pending records into a prefix-indexed stream and pack disjoint 1,024-record
   slices on the attached workers. This lets many short tables collectively cross the parallel threshold.
   Partial writes flush preceding whole writes; same-frame backing snapshots and failures retain their rules.
   The first five Hard pairs suggested a gain, but Easy/Medium showed little benefit and mixed tails.
3. **Restricted final design:** keep cold, small and unthreaded updates on the original immediate path.
   Batch only with workers and a validated layout containing at least 4,096 explicit-batch source records.
   A separate state-only diagnostic confirmed selection: Hard had batching enabled with 5,000 source records;
   Medium (800) and Easy (40) used the fallback. The final timing set still did not establish a Hard gain.

The retained buffer representation, float packing arithmetic, shaders, geometry selection and per-buffer
copy order stayed unchanged in these prototypes. No timing from a diagnostic binary is treated as normal
throughput. All experimental C++ and regression additions are archived in task output and removed from the
working source after the decision.

### Fresh uninstrumented results

The baseline and candidates use the same frozen application object and recipes: original independent Yard
submissions, 720p HDR, no AA/GI/AO/contact shadows, classic 1,024 spot maps, shadow caching enabled, four helpers,
affinity 0–15, 300 configured warm-up plus 1,440 measured frames. Pair order alternates. There are 50 normal
candidate runs in total: five serial pairs, eleven unrestricted-parallel pairs and nine final-design pairs.
Every paired frame has identical exported non-timing counters.

Changes below are **medians of paired percentage changes**, with negative values meaning lower cost. p50 is
the sample median; p95/p99 use sorted index `floor((N-1)*p/100)`. Variants are not pooled.

| Hard experiment | Pairs | Caller CPU change p50 / p95 / p99 | Process CPU p50 | Frame p50 |
| --- | --- | --- | --- | --- |
| Serial staging batch | 3 | +3.5% / +3.6% / +2.6% | +4.1% | -1.5% |
| Parallel batch, unrestricted | 5 | -5.7% / -13.3% / -15.6% | -3.0% | +0.5% |
| Final workload restriction | 3 | +0.4% / +5.7% / +10.9% | +3.3% | +0.9% |

The restricted design's three Hard caller-CPU p50 changes were **-4.9%, +0.4%, +8.6%**. Its unchanged hot
packing algorithm remained selected; the earlier favorable set cannot establish a repeatable improvement
for the final design under this contention. This does not prove batching can never help another workload.
It fails the acceptance criterion for adding this queue, bookkeeping and internal API here.

| Final workload restriction | Pairs | Caller CPU change p50 / p95 / p99 | Frame p50 |
| --- | --- | --- | --- |
| Easy | 3 | +2.3% / -10.8% / -6.6% | -4.6% |
| Medium | 3 | -1.9% / -2.5% / -1.9% | -0.2% |
| Hard | 3 | +0.4% / +5.7% / +10.9% | +0.9% |

### Correctness, restoration and evidence

Both parallel prototypes passed 14 focused runs each: NVIDIA core renderer plus six 3D suites, the six 3D
suites on software Vulkan, and the CPU suite. The 3D selections were direct cache, local graph, bounds,
draw commands, geometry and surface stages. All thirteen GPU runs in each set observed Khronos core and
synchronization validation, with no skips or validation errors. The mixed core-renderer suite chooses its
SDL/GPU backend per test; GPU checks remain required. This is focused coverage, not the full platform matrix.

The experimental regressions checked exact buffer bytes and overlapping copy order, cycling after an earlier
recorded read, reentrancy rejection/recovery, 4,800 records across twelve short tables with and without
workers, mixed full/partial/unchanged updates, exact pixels/picking against ordinary instance submissions,
and earlier-view preservation after same-frame edits. Existing local-graph tests also checked depth bits.
Each parallel prototype passed six exact-RGBA frozen-baseline capture pairs: Hard phases 1/541/961,
Easy/Medium phase 541, and Hard phase 541 with shadow image caching disabled. Counters matched too.

After rejecting the candidates, all five experimental C++/test files were restored exactly to the starting
commit and affected targets were rebuilt. Relinking the same application produced a **byte-identical baseline
benchmark**. The bounds, CPU, direct-cache, draw-command and local-graph test binaries are also byte-identical
to their previously validated starting versions. No production code, API, shader or test change remains.
The four unrelated dirty files were preserved.

Raw data, source patches/snapshots, commands, validation logs and provenance remain in
`out/runs/render3d-upload-packing/`: `experiment-summary.json`, the four `profile-*-profile.json` files,
`batch-serial.patch`, `batch-parallel-all.patch`, `final-candidate.patch`, `rejected-src/`,
`all-test-results.json`, `test-results.json`, `restoration.json` and `checks.json`. Generated artifacts are
not committed. The next useful CPU targets remain live-shadow fingerprinting and draw-span emission,
measured with whole-phase timers before introducing per-item scopes.

## CPU preparation profile and retained caster eligibility, 2026-11-12

The next CPU investigation profiles preparation, pass recording, uploads, submission and presentation before
changing scheduling. The selected optimization retains the existing static-caster eligibility byte mask under
the renderer's command/ref-layout validity checks. It removes a repeated mesh/material scan while keeping
current visibility, poses, mutable batches, fingerprints, depth commands and shaders on their existing paths.
There is no new public option, cache payload or GPU representation. The frozen baseline is `783f9c16` on
`perf/render3d-cpu-parity`.

### Controls and diagnostic scope

**Every result is CONTENDED.** Blender and all background workloads stayed running. Three unchanged-binary
Hard controls produced caller-CPU p50 **4.662, 6.112 and 7.099 ms**, with frame p50 **6.218, 7.262 and
8.233 ms**. The same binary later measured lower CPU times during matched candidate runs. These different
sessions cannot be used to claim extra optimization gains or a permanent CPU/GPU bottleneck.

Temporary diagnostic translation-unit overrides use scoped wall and thread-CPU clocks, with nested scopes
on every fourth frame and an update root on every frame. Full routes have 300 warm-up and 1,440 measured
frames; 360 measured frames receive nested scopes. Preparation, recording, storage upload/map/copy/submit
calls, and the subsequent 2D/present path are covered. A second probe splits shadow preparation and main
draw-span emission. Each event records its parent; exclusive durations subtract direct children before
aggregation. Helper-thread CPU is not attributed to these caller-thread stages. The application's existing
process-CPU column remains available separately.

Diagnostic scope overhead is material: sampled Hard frames used roughly **0.7–1.1 ms** more caller CPU than
their preceding three route frames. This adjacent-frame estimate also contains route/load variation; it is
not an exact overhead correction. Scoped timings are for locating work, not throughput claims. Probe images
and all non-timing counters match the frozen binary at route phase 541 on all three tiers. Production
comparison binaries contain neither scoped probes nor synchronous GPU timestamp waits.

The following are **inclusive caller-CPU means in ms**, not p50. Hard shows the range of two full-route
profiles. The five preparation/recording rows are disjoint; their remainder is other work in the renderer.

| CPU scope | Easy | Medium | Hard range |
| --- | --- | --- | --- |
| Culling and instance uploads | 0.203 | 0.886 | 1.946–2.446 |
| Local-shadow preparation | 0.115 | 0.700 | 2.162–2.520 |
| Draw planning/composition | 0.085 | 0.425 | 0.967–1.203 |
| Local-shadow recording | 0.042 | 0.175 | 0.436–0.567 |
| Main-pass recording | 0.077 | 0.178 | 0.226–0.292 |
| Other renderer work | 0.221 | 0.459 | 0.705–0.847 |
| Total renderer caller CPU | 0.742 | 2.822 | 6.442–7.874 |

In Hard, preparation dominates: culling/uploads and shadow preparation each consume roughly a third of
profiled renderer CPU, followed by composition at about 15%. The nested upload-staging scopes are substantial;
they include callback packing and copies and do not independently isolate memory-transfer bandwidth.
Across the two Hard profiles, SDL submit calls total only **0.064–0.079 ms** caller CPU per sampled frame.
Presentation, outside the renderer call, uses **0.140–0.166 ms** caller CPU and **0.166–0.322 ms** wall time.
These API/present figures overlap other scopes or lie outside the renderer total and must not be added to it.
The measured cost points to preparation rather than a large CPU submit call.

Two deeper Hard profiles identify source/caster classification at **0.499–0.507 ms**, live depth fingerprinting
at **0.405–0.441 ms**, live selection at **0.349–0.380 ms**, window compilation at **0.257–0.272 ms**, and
final command coalescing at **0.252–0.284 ms**. Classification includes the repeated static eligibility proof;
the probe does not isolate that scan alone. Main draw-span emission is another **0.427–0.554 ms**.

### Implementation and output parity

The existing mask now remains valid while the exact command revisions and validated ref layout remain
current. `cull_and_upload` invalidates it whenever the existing ref-update proof cannot be reused. Disabling
shadow image caching or entering a public local pass clears it. Exposed mutable instance spans always force
zero static membership. Mutable batches and skinned poses remain live and cannot enter the static mask.
No new owning handles, traversal cache or vector allocation is introduced.

The local-shadow test now changes caster membership through casts-shadow, opaque/cutout material transitions
and caller edits through an exposed span without another revision. It compares exact RGBA and every depth
bit against full redraw, alongside existing moving batches/poses, projection/culling changes, packed-atlas
reassignment and cache-off checks.

### Fresh uninstrumented comparison

Both binaries use the same frozen no-AA application object, recipes and flags: original independent Yard
submissions, 720p HDR, classic 1,024 spot maps, no AA/GI/AO/contact shadows, four helpers, affinity 0–15,
300 configured warm-up plus 1,440 measured frames, and shadow image caching enabled. There are three
alternating Hard pairs, one Easy guard and three Medium pairs. Medium was extended after its first guard
showed worse CPU tails. Every exported non-timing counter matches in every paired frame.

Cells are **p50 / p95 / p99 ms**, aggregated as medians of per-run percentiles. Changes are medians of paired
percentage changes; they need not equal ratios of the aggregate cells. p50 is the sample median and p95/p99
use sorted index `floor((N-1)*p/100)`.

| Tier | Pairs | Renderer caller CPU before | After | Paired change p50 / p95 / p99 |
| --- | --- | --- | --- | --- |
| Easy | 1 | 0.231 / 0.281 / 0.347 | 0.219 / 0.301 / 0.328 | -5.0% / +7.1% / -5.7% |
| Medium | 3 | 0.951 / 1.041 / 1.083 | 0.818 / 0.914 / 1.051 | -10.9% / -8.1% / -10.1% |
| Hard | 3 | 2.462 / 2.900 / 3.195 | 2.173 / 2.404 / 2.663 | -11.4% / -16.1% / -16.3% |

Hard caller-CPU p50 changes were **-4.8%, -11.7%, -11.4%**. Its median paired process-CPU p50 change is
**-9.2%**. Medium caller-CPU p50 improved in all three pairs, but its first pair's CPU p95/p99 regressed;
the repeated pairs improved both. Easy has one guard with mixed tails. These are local contended results,
not a guarantee about every scene or load.

| Tier | Frame before | After | Paired frame change p50 / p95 / p99 |
| --- | --- | --- | --- |
| Easy | 0.930 / 1.130 / 1.312 | 0.931 / 1.146 / 1.315 | +0.0% / +1.4% / +0.3% |
| Medium | 2.008 / 2.975 / 3.199 | 2.079 / 2.969 / 3.222 | +0.0% / -0.2% / -0.6% |
| Hard | 5.183 / 8.058 / 8.312 | 4.729 / 7.279 / 7.539 | -0.4% / +0.1% / +0.1% |

**Frame throughput is effectively flat by the median paired changes**, despite the CPU reduction. For Hard,
individual frame-p50 changes are **-0.4%, -8.8%, +0.7%**. The lower aggregate frame cells do not establish a
repeatable 8–9% speedup: paired changes are the relevant comparison under this drift. GPU attribution was
not repeated for this CPU-only change, and no GPU improvement is claimed.

### Verification and retained evidence

- Built the affected existing targets. Four focused suites passed on NVIDIA and the same four passed on
  software Vulkan: local graph, bounds, direct cache and draw commands. All eight GPU runs observed the
  Khronos validation layer with synchronization validation; no skips or validation errors. The CPU suite
  also passed. This is focused validation, not the full platform matrix.
- Six exact-RGBA before/after capture pairs passed: Hard phases 1/541/961, Easy/Medium phase 541 and Hard
  phase 541 with image caching disabled. All their non-timing counters match. The local-graph regression
  suite separately checks exact depth bits and output across changing static/live membership.
- Rebuilding after whitespace formatting produced byte-identical test binaries. All fourteen normal
  throughput runs stayed below the existing 450-second/1-GiB process guard. The four unrelated dirty files
  were preserved.

Raw reports, CSVs, scoped events, commands and binary/source hashes remain in
`out/runs/render3d-cpu-submission/`: `controls.json`, `cpu-profile-summary.json`, `candidate-summary.json`,
`candidate-provenance.json`, `candidate-capture-checks.json`, `test-results.json` and `checks.json`.
Generated artifacts are not committed. The remaining measured CPU opportunities are live shadow
fingerprints/selection, culling and upload packing, and main draw-span emission; isolate each before
changing its representation or scheduling.

## Discard-free opaque material shading, 2026-11-12

The reference is frozen **`52722b4c`**, including static shadow depth reuse and compact CPU preparation. Both versions use the same no-AA application object, `cache_shadows=1`, unchanged render settings and source content. The production change selects a discard-free stock fragment variant for proven eligible retained-material batches. It adds no public setting, render target, geometry reduction, light reduction or shader-compiler optimization flag.

### Main-pass diagnosis

Before changing the renderer, a task-local copy of the existing shader-loader object was linked before the unchanged engine archive. It can replace only selected stock mesh fragment modules from a task directory. Its control modules match all twelve original SPIR-V binaries byte-for-byte; control pixels and every non-timing counter match the frozen executable. This loader and all substituted diagnostic shaders remain generated artifacts, outside production code.

Each following exploratory case is one full Hard route using the synchronous GPU diagnostic. **These ablations alter output and are not quality-preserving speedups.** The two controls expose considerable contention drift; their independent percentiles cannot be subtracted into a precise phase-cost breakdown.

| Diagnostic | Main GPU p50 / p95 / p99 ms | Unchanged local-shadow GPU p50 ms |
| --- | --- | --- |
| Full shader, initial control | 4.232 / 9.770 / 9.946 | 1.387 |
| Base/emission without lighting or surface maps | 0.940 / 1.380 / 1.520 | 1.254 |
| Base, normal and PBR maps; lighting removed | 1.331 / 4.476 / 4.599 | 1.492 |
| Local-light evaluation removed | 1.758 / 3.940 / 5.044 | 1.629 |
| Shadow reception off; map rendering retained | 2.909 / 5.180 / 5.491 | 1.630 |
| Forced early tests, diagnostic only | 3.499 / 5.119 / 5.526 | 1.630 |
| glslc -O, diagnostic only | 3.701 / 6.771 / 6.937 | 1.630 |
| Full shader, final control | 5.868 / 11.051 / 11.329 | 1.631 |

The flat/material controls indicate that shading, particularly local-light evaluation and shadow sampling, is a larger opportunity than geometry/raster work alone in this route. Further probes tried explicit PCF unrolling and optimized SPIR-V, and confirmed one sampled image for each against the frozen control. They were not shipped. Removing an unreachable opaque alpha-discard branch was selected to avoid its fragment-control dependency and permit earlier depth/write optimization while preserving all lighting/filter arithmetic. No fragment-invocation, occupancy or bandwidth counters were collected, so the exact hardware contribution is unmeasured.

### Production behavior and safety

Four optional native retained-material variants cover base, fog, local lights and lights+fog. They retain the same texture samples and floating-point shading expressions, omitting only the stock alpha-discard block and mask-only alpha assignment. The renderer proves that the **entire compiled batch** is `Blend3D::Opaque`, not just its representative. Existing mixed opaque/cutout batches retain the general shader and their batching/order. The mesh must be immutable and have constant vertex alpha across all uploaded base/LOD vertices; the base texture must be absent or normalized RGBA8/sRGB. Constant vertex alpha avoids negative extrapolation at MSAA pixel centers, while normalized texture sampling cannot introduce signed alpha.

Zero and fractional alpha remain unchanged; opaque does not imply forcing alpha to one. Signed floating-point base textures, varying/mutable vertex alpha, masks, custom shaders and surface shaders keep the existing path. Ordinary non-material-table draws also keep their original shaders. New variants/pipelines fall back to the general shader when unavailable. The production path does **not** force early-fragment-test mode. The twelve old stock shader binaries remain byte-identical; binary inspection confirms zero discard instructions in each new module versus one in its general counterpart, with identical floating-arithmetic opcode counts. This count is supporting evidence, not a replacement for pixel comparisons.

### Fresh matched measurements

**All results are CONTENDED.** Blender and every external workload stayed running; clocks, thermals and load were uncontrolled. Primary timings use the NVIDIA GeForce RTX 4080 Laptop GPU; software Vulkan is used for validation. The existing Release/Ninja/GCC build, renderer `-O2`, GLSL compiler settings and shared application object were reused. Original independent Yard submissions use 720p HDR, no AA/GI/AO/contact shadows, classic 1,024 spot maps, four helpers, affinity 0–15, configured 300 warm-up and 1,440 measured route frames. Three alternating Hard pairs and one Easy/Medium guard pair were collected for both normal throughput and separate GPU attribution.

Every timing cell is **p50 / p95 / p99 ms**; Hard cells are medians of three per-run percentiles. Percentage changes are medians of per-pair changes, which can differ from ratios of independently aggregated cells. p50 is the sample median; p95/p99 use sorted index `floor((N-1)*p/100)`.

| Tier | Pairs | Main GPU before | After | Paired main p50 change |
| --- | --- | --- | --- | --- |
| Easy | 1 | 0.929 / 1.164 / 1.388 | 0.779 / 0.965 / 1.121 | -16.1% |
| Medium | 1 | 2.378 / 4.077 / 4.263 | 1.887 / 2.680 / 2.912 | -20.6% |
| Hard | 3 | 5.144 / 9.788 / 9.969 | 3.995 / 6.473 / 6.709 | -22.3% |

Individual Hard main-GPU p50 changes: -22.0%, -22.8%, -22.3%.

| Tier | Unchanged local-shadow GPU before | After | 3D span-sum before | After |
| --- | --- | --- | --- | --- |
| Easy | 0.070 / 0.071 / 0.071 | 0.070 / 0.070 / 0.071 | 1.025 / 1.260 / 1.486 | 0.874 / 1.061 / 1.217 |
| Medium | 0.197 / 0.200 / 0.201 | 0.199 / 0.223 / 0.225 | 2.602 / 4.299 / 4.488 | 2.115 / 2.911 / 3.146 |
| Hard | 1.630 / 1.635 / 1.636 | 1.632 / 1.773 / 1.905 | 6.799 / 11.455 / 11.628 | 5.719 / 8.142 / 8.416 |

GPU measurements use the existing synchronous Vulkan timestamp library: 13 nonnested bottom-of-pipe pass spans and a wait on each marked submission. They can include contention/dependency gaps and exclude unmarked uploads, 2D composition and presentation. **These are not normal throughput.** CPU/GPU times and independent pass percentiles must not be added. Unchanged local shadows provide a contemporaneous load control.

| Tier | Normal frame interval before | After | Paired frame p50 change |
| --- | --- | --- | --- |
| Easy | 2.126 / 2.800 / 3.334 | 1.969 / 2.697 / 3.226 | -7.4% |
| Medium | 6.874 / 11.551 / 12.161 | 4.479 / 7.190 / 8.012 | -34.8% |
| Hard | 11.104 / 27.284 / 28.828 | 10.127 / 14.553 / 15.190 | -8.8% |

Individual Hard normal frame-p50 changes: -3.4%, -8.8%, -33.3%.

| Tier | Caller CPU before | After | Renderer process CPU before | After |
| --- | --- | --- | --- | --- |
| Easy | 0.867 / 1.068 / 1.124 | 1.007 / 1.142 / 1.195 | 1.003 / 1.214 / 1.285 | 1.152 / 1.304 / 1.344 |
| Medium | 3.291 / 3.660 / 3.810 | 3.316 / 3.780 / 4.002 | 3.607 / 4.023 / 4.202 | 3.658 / 4.148 / 4.368 |
| Hard | 8.236 / 9.335 / 9.739 | 8.589 / 9.771 / 10.237 | 9.698 / 10.966 / 11.504 | 10.064 / 11.378 / 11.908 |

Normal runs have no GPU-query waits or screenshot readback. CPU timings cover `Renderer3D::render`, excluding scene updates and application/2D/presentation work; process CPU includes helpers. CPU results are mixed. Absolute CPU/frame values differ substantially from earlier campaigns, so comparisons should use the fresh pairs within this campaign. Easy/Medium are single guards. Frame tails and load drift must be assessed independently; no fixed-clock or exhaustive stability conclusion follows.

### Validation and limits

- **16 focused suite runs passed**, no skips: eight NVIDIA GPU checks and six Lavapipe GPU checks with Khronos synchronization validation, plus two CPU suites. No validation errors. This is not the full platform matrix.
- The extended draw-command suite compares retained/batched output against ordinary submission using the original shaders. It covers mixed opaque/cutout batches, zero/fractional tint and texture alpha, constant and varying vertex alpha, negative RGBA16F texture alpha, MSAA, HDR, all four fog/light variants, equal-depth order and picking, texture edits, poses, overlays, source moves and budget fallbacks.
- **Nine active-cache RGBA pairs match exactly** across Easy/Medium/Hard at route phases 1 / 541 / 961. Three additional cache-off pairs at phase 541 match exact RGBA. Captures explicitly occur at the third measured render for active-cache checks; cache-off captures use the second. Every exported non-timing CSV column matches before/after on all measured primary frames, including geometry, lights, shadow work/cache activity, draw counts, instance/material uploads and logical resource bytes.
- Per-version normal and GPU diagnostic non-timing metrics match; all 13 pass groups and CSV-to-submission mappings are checked. Percentiles, source/binary/recipe/application-object/library hashes and capture equality were independently recomputed.
- Four optional shader modules and a one-time mesh-alpha scan are added. Batch eligibility is computed during existing plan construction and stays within the existing metadata budget. No new per-frame uploads, GPU buffers or render targets are introduced. Driver code memory, cold shader compilation/loading and isolated startup cost were not measured; logical resource bytes are not physical VRAM/RSS. No exact floating-point HDR attachment-bit comparison or hardware invocation-counter attribution is claimed.

Artifacts: `out/runs/render3d-main-shading/` contains frozen baseline/candidate and source hashes, diagnostic loader/source/shader identities, exploratory ablation/probe CSVs, raw matched timings/timestamps, per-run process/telemetry records, 30 PNGs (24 final comparison captures plus six diagnostic controls/probes), JUnit reports, shader instruction checks and `summary.json` / `checks.json`. The diagnostic loader is absent from the production link. Generated output is not committed. The four unrelated dirty files recorded at task start were preserved byte-for-byte.

## CPU preparation for cached local shadows, 2026-11-12

The reference is frozen **`82a380f8`**, with static opaque shadow-depth reuse enabled. Both executables use **`cache_shadows=1`**, the same existing no-AA application object and the same quality settings. This comparison measures CPU preparation changes on top of depth reuse; it is distinct from enabling caching versus forced redraw.

Previously the shadow cache rebuilt a per-map byte stream containing static caster transforms, and enabling it disabled the existing deferred static-selection program. An admitted gather plan now records whether every saved direct row has immutable opaque depth inputs. Its source identity/revision, selected projection/frustum/reach and culling policy stand in for the repeated static records. Dynamic batches, poses and other live rows still contribute their detailed inputs. The existing compiled selection program stays source-relative and compact until command/list emission, avoiding expansion into per-frame `Found` rows and transform serialization. No additional cache layer, dependency, public option or application-object rebuild was added.

If a saved direct selection contains a cutout, mutable mesh or other ineligible row, it retains the detailed path. Exposed instance spans, multi-LOD/cluster meshes and unsupported retention also keep existing behavior. Temporal shadow-content history retains its original full fingerprint bytes; compact and full payload formats are explicitly distinguished when switching paths. Source revision changes conservatively invalidate the compact key even if they leave depth unchanged. The gather key now also includes `Scene3D::cull`, so a culling-policy change cannot reuse the previous selection. Public graph producers, shaders, shadow resolution/filtering and the 64 MiB auxiliary-atlas cap are unchanged.

**All results are CONTENDED.** Blender and all background workloads were left untouched; clocks, temperature and system load were uncontrolled. Existing Release/Ninja/GCC, renderer `-O2`, build directory and application object were reused. Settings: 720p HDR, no AA/GI/AO/contact shadows, classic 1,024 spot maps, original independent Yard submissions, four helpers and affinity 0–15. Configured warm-up is 300 frames, followed by the full 1,440-frame route. There are three alternating Hard normal pairs and one Easy/Medium guard pair; GPU attribution uses one separate synchronous pair per tier. All cells are **p50 / p95 / p99 ms**. Hard normal cells are medians of three per-run percentiles; percentage changes are medians of paired changes. p50 uses the sample median; p95/p99 use sorted index `floor((N-1)*p/100)`.

### Normal CPU and frame intervals

| Tier | Caller CPU before | After | Paired caller p50 change |
| --- | --- | --- | --- |
| Easy | 0.399 / 0.473 / 0.525 | 0.246 / 0.361 / 0.381 | -38.5% |
| Medium | 1.940 / 2.120 / 2.255 | 0.974 / 1.115 / 1.361 | -49.8% |
| Hard | 7.146 / 8.025 / 8.621 | 2.542 / 3.067 / 3.402 | -64.4% |

| Tier | Frame interval before | After | Paired frame p50 change |
| --- | --- | --- | --- |
| Easy | 1.080 / 1.361 / 1.580 | 1.050 / 1.348 / 1.580 | -2.8% |
| Medium | 2.690 / 4.402 / 4.608 | 2.692 / 4.396 / 4.598 | +0.1% |
| Hard | 7.878 / 11.753 / 12.737 | 5.960 / 11.629 / 11.826 | -18.3% |

Individual Hard caller-p50 changes: -57.1%, -64.4%, -64.8%. Individual Hard frame-p50 changes: -18.3%, -27.2%, -11.9%.

| Tier | Renderer process CPU before | After | Peak process RSS before / after |
| --- | --- | --- | --- |
| Easy | 0.458 / 0.568 / 0.620 | 0.308 / 0.460 / 0.497 | 141.0 / 141.1 MiB |
| Medium | 2.084 / 2.282 / 2.429 | 1.109 / 1.265 / 1.542 | 153.0 / 153.4 MiB |
| Hard | 7.780 / 8.773 / 9.333 | 3.103 / 3.675 / 4.046 | 187.3 / 195.6 MiB |

CPU columns cover `Renderer3D::render`, excluding scene updates and application/2D/presentation work; process CPU includes helper threads. Normal runs have neither timestamp-query waits nor screenshot readback. These are whole-render CPU timings, not isolated fingerprinting durations. CPU improvements need not translate proportionally to frame time: GPU shading, presentation and contention still contribute. Medium frame p50 is essentially flat despite lower CPU cost, consistent with a remaining GPU limit. Hard frame tails vary across pairs; this does not establish a uniform frame-tail improvement. The raw per-pair results remain in `summary.json`.

### GPU preservation guard

| Tier | Local-shadow GPU before | After | Unchanged main GPU before | After |
| --- | --- | --- | --- | --- |
| Easy | 0.069 / 0.070 / 0.070 | 0.068 / 0.070 / 0.070 | 0.926 / 1.166 / 1.382 | 0.898 / 1.158 / 1.391 |
| Medium | 0.195 / 0.198 / 0.200 | 0.196 / 0.199 / 0.201 | 2.302 / 4.107 / 4.487 | 2.389 / 4.112 / 4.431 |
| Hard | 1.400 / 1.629 / 1.633 | 1.576 / 1.630 / 1.632 | 4.278 / 9.292 / 9.890 | 4.993 / 9.767 / 9.949 |

GPU values are single guard pairs using the existing synchronous Vulkan timestamp library: 13 nonnested bottom-of-pipe pass spans, with a wait on marked submissions. They are **not normal throughput** and can include preemption/dependency gaps; uploads/2D/presentation outside the markers are excluded. The unchanged main pass is a contemporaneous load control. CPU and GPU timings must not be added.

### Correctness, residency and limits

- **15 focused suites passed**, no skips: eight NVIDIA GPU suites and five Lavapipe GPU suites with Khronos synchronization validation, plus two CPU suites. No validation errors. This is not the full platform matrix.
- The new regression exceeds direct-retention admission with 320 static copies plus moving batch/skinned casters. Hot fingerprint payload and expanded selection sizes are each **less than one quarter** of the detailed reference. It compares exact final RGBA and every wanted stock map texel’s **32 depth bits**, with full gather and partial reuse disabled for the reference. It exercises source edits/addition, light and camera changes, culling policy, cache re-entry and direct cutout fallback on both GPU backends, including a mask texture upload that leaves the scene revision unchanged. The earlier static-depth invalidation, packed-atlas, source-removal, cache-off and warmed-resource regressions also pass.
- **Nine active-cache 720p RGBA pairs match exactly**, across all tiers at route phases 1 / 541 / 961. Captures explicitly occur at the third measured render, with both versions asserting active partial reuse in that frame. Three additional cache-off pairs at phase 541 match exact RGBA and every exported non-timing column, with zero auxiliary-atlas bytes.
- Every measured primary before/after route frame preserves source configuration, main geometry, lighting, poses and motion, and conserves **actual + statically omitted shadow triangles/casters**. After the first two cold route frames, actual/omitted shadow geometry and atlas bytes also match exactly. All non-timing columns match between each version’s own normal and GPU diagnostic runs. Pass inventories, frame-to-submission mapping and all exported percentiles were independently recomputed.
- Compact fingerprinting can cause a one-time full-map miss on a payload-format transition. During stationary warm-up, this can bake the auxiliary atlas earlier, which then expires under the existing idle policy; the moving route can therefore need two cold frames (observe, bake) instead of one. Full geometry is drawn whenever retained depth is unavailable, preserving output. Steady atlas storage remains **8 / 16 / 32 MiB** for Easy/Medium/Hard, within the existing cap. CPU selection-program payloads remain inside the existing 4 MiB gather budget; descriptor bytes and RSS are distinct measures.
- A separate sampled-stack profile was attempted, but `perf record` was denied for both `cpu-clock:u` and `task-clock:u` by host permissions. `perf stat` counting works, but sampling did not; no system settings were changed. No sampled phase-percentage attribution is claimed. Phase isolation, loading/startup, device loss, fixed-clock isolation and exhaustive long-duration stability remain unmeasured.

Artifacts: `out/runs/render3d-shadow-static-cpu/` contains frozen baseline/source hashes, candidate and shared application-object hashes, recipes/link command/GPU library hash, raw normal and timestamp CSVs, per-run process/load evidence, 24 PNG captures, JUnit reports, exact-depth/hot-key regression results, permission-denial evidence and `summary.json` / `checks.json`. Preliminary 120-frame smoke runs are retained separately from the full-route comparison. Generated output is not committed; the four unrelated dirty files recorded at task start were preserved byte-for-byte.

## Static opaque local-shadow depth reuse, 2026-11-12

The reference is frozen renderer **`3aece4f5`**, with the position-only shadow mesh already enabled. Both binaries share one no-AA application object and the same cache CLI/zero-initialized telemetry scaffolding; the reference retains the original full-map recording. The primary comparison explicitly enables `Lights3D::cache_shadows` in **both**. This is a fresh cache-enabled comparison, not a continuation of the earlier cache-off timings.

Mixed native D32F spot/point maps can now retain opaque rigid depth separately, restore it at the original atlas coordinates with an unfiltered texel fetch, then rasterize live casters. Two identical source/projection observations admit a bake. Source edits, changed light projections/layouts, and overwritten atlas regions invalidate it. Mutable batches, exposed instance spans, skinning/morphs, multi-LOD/cluster meshes, cutouts and depth-affecting surfaces stay live. Fully unchanged maps retain the existing whole-map cache. Sun/moon and explicit graph local producers retain their existing paths.

The auxiliary atlas is capped at **64 MiB**, falls back to full recording when unavailable, and releases when caching is disabled (or after idle retention). Resolution, light fitting, filtering and caster selection are unchanged. The Industrial Yard sample keeps its existing forced-redraw default; use **`--cache-shadows 1`** to exercise reuse. New `local_shadow_static_*` counters distinguish baked/reused maps, omitted geometry and resident bytes; `local_shadow_bytes` includes the auxiliary atlas.

**All measurements are CONTENDED.** Blender and all background workloads were left untouched; clocks, thermals and system load were uncontrolled. Settings: 720p HDR, no AA/GI/AO/contact shadows, classic 1,024 spot maps, independent original Yard submissions, four helpers, affinity 0–15, configured 300 warm-up and 1,440 measured route frames. Existing Release/Ninja GCC, renderer `-O2`, and build directory were reused. Three alternating Hard pairs and one Easy/Medium guard pair were collected for each mode. Cells below are **p50 / p95 / p99 ms**; Hard cells are medians of three per-run percentiles. Changes are medians of paired percentage changes. p50 is the sample median; p95/p99 use sorted index `floor((N-1)*p/100)`.

### GPU shadow attribution

| Tier | Pairs | Before | After | Paired p50 change |
| --- | --- | --- | --- | --- |
| Easy | 1 | 0.180 / 0.180 / 0.181 | 0.070 / 0.070 / 0.070 | -61.3% |
| Medium | 1 | 1.237 / 1.238 / 1.239 | 0.196 / 0.199 / 0.201 | -84.1% |
| Hard | 3 | 6.235 / 6.237 / 6.238 | 1.632 / 1.634 / 1.708 | -73.8% |

Individual Hard shadow-p50 changes: -76.0%, -73.8%, -73.8%.

| Tier | Unchanged main GPU before | After | 3D span-sum before | After |
| --- | --- | --- | --- | --- |
| Easy | 0.929 / 1.164 / 1.385 | 0.930 / 1.167 / 1.382 | 1.133 / 1.370 / 1.590 | 1.025 / 1.261 / 1.478 |
| Medium | 2.381 / 4.101 / 4.280 | 2.380 / 4.082 / 4.258 | 3.642 / 5.362 / 5.541 | 2.601 / 4.301 / 4.478 |
| Hard | 5.124 / 9.794 / 9.959 | 5.142 / 9.804 / 9.984 | 11.384 / 16.052 / 16.219 | 6.800 / 11.463 / 11.641 |

These are separate synchronous bottom-of-pipe spans at 13 nonnested pass markers, with a wait on each marked submission. The sum excludes unmarked uploads, 2D composition and presentation; preemption/dependency gaps can contribute. **This diagnostic is not normal throughput.** CPU/GPU and independent pass percentiles must not be added. The unchanged main pass is included as a contemporaneous load control. Easy/Medium are single guard pairs; this is not fixed-clock isolated evidence.

### Normal frame intervals and CPU

Normal runs use neither timestamp-query waits nor screenshot readback.

| Tier | Frame before | After | Paired frame p50 change |
| --- | --- | --- | --- |
| Easy | 1.474 / 1.871 / 2.155 | 1.346 / 1.762 / 2.009 | -8.7% |
| Medium | 4.396 / 6.818 / 7.156 | 3.691 / 6.175 / 6.524 | -16.0% |
| Hard | 11.609 / 17.060 / 17.328 | 8.213 / 12.644 / 12.915 | -29.3% |

| Tier | Caller CPU before | After | Renderer process CPU before | After |
| --- | --- | --- | --- | --- |
| Easy | 0.436 / 0.489 / 0.519 | 0.417 / 0.482 / 0.558 | 0.512 / 0.617 / 0.653 | 0.498 / 0.625 / 0.688 |
| Medium | 1.869 / 2.031 / 2.208 | 2.234 / 2.634 / 2.956 | 2.009 / 2.185 / 2.358 | 2.392 / 2.814 / 3.131 |
| Hard | 6.976 / 7.879 / 8.470 | 7.556 / 8.753 / 9.109 | 7.582 / 8.540 / 9.219 | 8.206 / 9.517 / 9.910 |

CPU columns measure time inside `Renderer3D::render`, excluding scene updates and application/2D/presentation work. Process CPU includes helper work. Partial depth reuse still pays current caster gathering, instance/list preparation and whole-map fingerprinting; it primarily removes GPU geometry work.

### Forced-redraw policy guard

A separate **single contended Hard pair** compares the previous benchmark policy (`cache_shadows=0` on the frozen reference) with the candidate using `cache_shadows=1`. This includes the CPU cost of enabling the existing whole-map fingerprinting, so it must be read separately from the cache-enabled implementation comparison above.

| Hard guard | Before, forced redraw | After, partial caching |
| --- | --- | --- |
| Frame interval | 11.557 / 18.635 / 19.087 | 8.728 / 12.764 / 13.017 |
| Caller CPU | 2.193 / 2.433 / 2.532 | 7.990 / 8.627 / 8.878 |
| Renderer process CPU | 2.739 / 3.046 / 3.212 | 8.718 / 9.429 / 9.645 |

### Work, memory and output parity

| Tier | Shadow triangles before, p50 | Actually rasterized after, p50 | Static triangles omitted, p50 | Extra atlas |
| --- | --- | --- | --- | --- |
| Easy | 1,113,312 | 28,056 | 1,085,256 | 8 MiB |
| Medium | 8,086,164 | 848,712 | 7,237,452 | 16 MiB |
| Hard | 40,941,344 | 10,277,396 | 30,663,948 | 32 MiB |

- **15 focused suites passed**, no skips: eight NVIDIA GPU suites and five Lavapipe GPU suites with Khronos synchronization validation, plus two CPU suites. No validation errors. These are focused checks, not the full platform matrix.
- The local-shadow regression compares **all 32 depth bits of every wanted stock spot/point texel**, and exact final RGBA, against the original full geometry route with partial reuse disabled. It covers nonzero packed origins, moving batches/poses, cutouts, fixed-caster changes/removal, light projection/resolution changes, repacking, culling policy and cache-off. Both native NVIDIA and software Vulkan passed. Warmed rendering has no extra non-staging GPU allocations or staging growth.
- **Three 720p RGBA pairs at route frame 541 are byte-identical while partial reuse is active**, with the new reuse/skipped-geometry counters asserted in the captured frame. Three additional cache-off pairs match every exported non-timing column and exact RGBA, with zero auxiliary atlas bytes.
- Nine further 720p RGBA pairs are byte-identical during the stationary warm-up phases **0 / 540 / 960** across all tiers. The harness default capture happens at configured warm-up frame 300; their CSV rows describe the following route phases 1 / 541 / 961. These stationary captures do not establish active partial-cache-hit parity; the explicit frame-541 captures and depth regression above do.
- On every measured primary before/after route frame, source configuration, main geometry, lighting, skinning and motion match. **Actually drawn + statically omitted shadow triangles/casters equal the reference**, including the initial bake. GPU diagnostic and normal non-timing columns match within each version. All 13 pass groups and CSV-to-submission mapping were checked; all reported percentiles were recomputed.
- The atlas descriptor bytes are not physical VRAM/RSS. Geometry/skin/list uploads are not eliminated by this change; the extra atlas is the principal retained GPU payload tradeoff. No loading/startup, isolated thermal, device-loss or exhaustive long-duration stability claim is made.

Artifacts: `out/runs/render3d-shadow-static-depth/` contains frozen baseline/candidate executable and source hashes, shared application-object hash, recipes, compiler/link commands, GPU library hash, per-run logs/process records/telemetry, raw CSV/timestamps, 30 PNG captures, JUnit reports, exact-image/depth gates and `summary.json` / `checks.json`. The four unrelated dirty files recorded at task start were preserved byte-for-byte. Generated output is not committed.

## Position-only local-shadow geometry, 2026-11-12

The reference is frozen renderer **`32f387ea`**, including map-wide shadow batches
and direct index scatter. Immutable rigid meshes now prepare an additional
position-only stream and shadow indices once at creation. Bit-identical positions
are welded across normal/UV seams; nearby floats and signed zero stay distinct.
Every triangle retains its ordered position values, winding and LOD range,
including degenerates. The stock shadow shader and transform math are unchanged.
Vertex input stride is **36 → 12 bytes**; the topology check verifies a box has
**24 → 8 vertices**, with the same 12 triangles. Hardware vertex-invocation or
bandwidth counters were not collected.

Opaque local maps use this representation in both ordinary and shared-group
recording. Cutouts, skins, morphs, depth-affecting surfaces and cluster cuts keep
the original streams; other passes, including sun shadows, are unchanged.
Small meshes reuse the existing fenced buffer pool, padding allocations to retain
both vertex strides' alignment. Large meshes use tracked dedicated buffers.
Optional buffer/pipeline failures fall back to the original stream. Uploads use
existing staging at creation; no per-frame topology compilation/uploads are added.
Extra mesh-creation work and resident geometry remain tradeoffs; this campaign
measures warmed rendering, not a controlled loading/startup comparison.

**All results are contended.** Blender and every external workload stayed running;
CPU/GPU clocks, thermals and load remain uncontrolled. Original independent Yard
submissions use 720p HDR, no AA/GI/AO/contact shadows, classic 1,024 spot maps,
four helpers, affinity 0–15, 300 warm-up and 1,440 measured route frames.
Whole-map shadow-image caching stays off in both. Release/Ninja GCC and renderer
`-O2` settings are unchanged; the existing build and no-AA application object
were reused. Frozen reference executables remain intact.

### GPU pass attribution

Three alternating Hard pairs and one Easy/Medium guard pair use the existing
synchronous Vulkan timestamp library. Every cell is **p50 / p95 / p99 ms**;
Hard cells are medians of three per-run percentiles. p50 uses the sample median;
p95/p99 use sorted index `floor((N-1)*p/100)`. Percentage changes are medians of
per-pair changes, rather than ratios of independently aggregated table cells.

| Tier | Pairs | Shadow GPU before | After | Paired shadow p50 change |
| --- | --- | --- | --- | --- |
| Easy | 1 | 0.241 / 0.279 / 0.285 | 0.255 / 0.281 / 0.287 | +5.9% |
| Medium | 1 | 1.984 / 3.510 / 4.356 | 2.321 / 4.418 / 4.611 | +17.0% |
| Hard | 3 | 8.322 / 13.121 / 15.829 | 7.474 / 9.234 / 12.031 | -14.3% |

The individual Hard shadow-p50 changes are -10.2%, -14.6%, -14.3%.
The unchanged main shading pass is retained as a contemporaneous load control:

| Tier | Main GPU before | After | 3D span-sum before | After |
| --- | --- | --- | --- | --- |
| Easy | 1.264 / 2.622 / 3.012 | 1.634 / 3.058 / 3.338 | 1.549 / 2.909 / 3.384 | 1.931 / 3.377 / 3.666 |
| Medium | 4.229 / 8.931 / 10.572 | 5.490 / 10.849 / 11.721 | 6.542 / 12.093 / 14.565 | 7.928 / 13.989 / 15.500 |
| Hard | 6.335 / 18.890 / 20.271 | 6.202 / 12.826 / 18.707 | 14.926 / 31.538 / 33.906 | 13.682 / 21.783 / 31.087 |

These are bottom-of-pipe spans at 13 nonnested pass markers, with a wait for each
marked submission. Their sum excludes unmarked uploads, 2D composition and
presentation; contention, preemption and dependency gaps can contribute.
**This diagnostic is not normal throughput.** CPU/GPU and independent pass
percentiles must not be added. Easy/Medium are single guard pairs. These
observations do not establish an isolated gain under fixed clocks/load. The
Easy/Medium shadow guards are slower, while their unchanged main passes also
slow by about 29–30%; those single pairs are inconclusive about the optimization.

### Normal CPU and frame intervals

Normal pairs use neither timestamp-query waits nor screenshot readback. There
are ten full-route normal runs, separate from the ten GPU runs above.
Caller CPU measures renderer thread execution, excluding blocked waits;
frame interval includes runtime/presentation and scheduling. Cells again show
p50 / p95 / p99 ms, aggregated as above.

| Tier | Caller CPU before | After | Frame interval before | After |
| --- | --- | --- | --- | --- |
| Easy | 0.351 / 0.514 / 0.585 | 0.334 / 0.432 / 0.476 | 2.660 / 4.091 / 4.665 | 2.576 / 3.219 / 3.749 |
| Medium | 0.997 / 1.244 / 1.447 | 0.965 / 1.138 / 1.238 | 6.003 / 8.963 / 9.378 | 6.076 / 9.504 / 10.024 |
| Hard | 4.070 / 4.862 / 5.467 | 4.063 / 4.912 / 5.612 | 34.343 / 52.790 / 55.319 | 33.960 / 54.104 / 56.110 |

| Tier | Paired caller CPU p50 change | Paired frame p50 change |
| --- | --- | --- |
| Easy | -4.8% | -3.2% |
| Medium | -3.2% | +1.2% |
| Hard | -0.2% | -5.8% |

Individual Hard frame-p50 changes are -6.1%, -5.8%, -0.9%. Independently aggregated
frame medians are 34.343 → 33.960 ms (-1.1%),
which differs from the median paired change because the median comes from
different repeats in each version. Hard frame p95/p99 medians are higher after;
this campaign does not demonstrate better frame-time tails.

Raw repeats, process CPU, all paired p95/p99 changes and telemetry remain in
`summary.json` and per-run artifacts. Normal frame gains need not track the
shadow span proportionally, and differences from earlier campaigns cannot be
attributed to this optimization by comparing their absolute times.

### Correctness and stability checks

All **15 focused suites pass without skips**: eight GPU suites on RTX 4080
Laptop Vulkan, five on Lavapipe, and two CPU suites. All thirteen GPU-suite
executions have loader evidence of Khronos validation with synchronization and
shader-access analysis, with zero validation-error markers. The CPU-only
`kin-rendering` suite is counted as a functional pass, not Vulkan validation.
The local-graph suite independently checks exact ordered positions for indexed
and implicit topology, LOD extras, seams, degenerates and float boundaries.
Its GPU regression requires active compact geometry, tests nonzero pooled offsets
and dedicated buffers, and compares all 32 depth bits at sampled texels against
opaque-white cutouts forcing the original full-vertex path. It covers three LODs,
instancing, reversals, mixed streams and all seven maps of a spot plus a point.

All **nine decoded-RGBA Yard capture pairs match exactly**, at route frames
1, 541 and 961 across the three tiers. Every measured normal/GPU before/after
pair matches every exported non-timing work/cache/resource/upload CSV column,
including selected geometry, draw counts, skinning, motion and shadow work.
Those CSV columns do not account for the added mesh buffers or creation uploads;
extra resident geometry is still a cost of this representation.
Hard remains **1,096 shadow draws / 40,941,344 shadow triangles**. Hardware GPU
runs also match each version's own normal control at every corresponding frame.
Every GPU run has 1,741 complete contiguous 13-pass groups, with measured CSV
frames 301–1740 mapping to submissions 302–1741.

The 38 bounded benchmark/capture processes all complete. These checks cover
sampled output and focused runtime/resource behavior, not every scene, temporal
artifact or platform. Evidence, binaries/source/library/recipe hashes, commands,
JUnit/layer logs, raw timings and final checks are under
`out/runs/render3d-shadow-positions/`. No full engine/platform matrix is claimed.

## Current branch versus main, 2026-11-12

This is a fresh paired comparison of local `main` **`6aadd75e`** against renderer
**`32f387ea`** on `perf/render3d-cpu-parity` (the comparison documentation starts
at `b9606dec`). It reuses the frozen binaries whose hashes match their original
build provenance: same Release/Ninja GCC configuration, renderer `-O2`, same
no-AA application object and unchanged sample/public API sources. No rebuild,
renderer edit, configuration change or temporary instrumentation was needed.
Main predates the CPU preparation/culling/draw/shadow redesigns and map-wide
shadow GPU batches. This measures their combined result, not individual gains.
The main observed gain is caller CPU: Hard falls from 23.584 to 2.773 ms p50,
with an 88.3% median paired reduction. Frame-time gains are substantially smaller.

**Contended:** Blender and all other background workloads remained running.
CPU/GPU clocks, temperatures and system load are uncontrolled. These fresh
numbers must not be combined with the earlier Godot/Unreal campaign as if
all engines ran under the same load.

### Normal frame and CPU timings

The existing Industrial Yard fixture uses independent objects, 1280×720 HDR,
no AA/GI/AO/contact shadows, nominal classic 1,024 spot maps, four helpers,
affinity 0–15, and 300 warm-up plus 1,440 measured route frames. Whole-map shadow
image caching is off in both. Retention/gather/draw-command controls are enabled
in both; their implementations and admission behavior differ. Both use the same
hidden presentation and bounded frames-in-flight policy.

Three alternating pairs per tier, with rotated tier order, produce **18 normal
runs / 25,920 measured frames**. Each table cell is **p50 / p95 / p99 ms**,
aggregated as the median of three per-run percentiles. The sample median is
used for p50 and sorted index `floor((N-1)*p/100)` for p95/p99. Normal timings
exclude screenshot readback and synchronous GPU timestamp queries.

| Tier | Main frame interval | Current frame interval |
| --- | --- | --- |
| Easy | 3.863 / 6.961 / 7.486 | 2.704 / 6.835 / 7.322 |
| Medium | 9.694 / 20.217 / 20.868 | 8.790 / 19.615 / 20.370 |
| Hard | 28.054 / 64.634 / 78.937 | 26.436 / 51.740 / 53.785 |

Caller CPU measures actual renderer caller-thread execution, excluding blocked
waiting; process CPU includes helper execution during the renderer span.
The two CPU channels overlap and must not be added. Application scene-build
time is a separate exported scope.

| Tier | Main caller CPU | Current caller CPU | Main process CPU | Current process CPU |
| --- | --- | --- | --- | --- |
| Easy | 0.925 / 1.202 / 1.333 | 0.361 / 0.464 / 0.498 | 1.218 / 1.537 / 1.669 | 0.474 / 0.575 / 0.617 |
| Medium | 4.057 / 4.575 / 4.805 | 1.030 / 1.196 / 1.287 | 4.816 / 5.366 / 5.704 | 1.175 / 1.359 / 1.469 |
| Hard | 23.584 / 29.212 / 32.796 | 2.773 / 3.227 / 3.440 | 30.511 / 37.134 / 41.230 | 3.447 / 3.999 / 4.238 |

Changes below are the **median of three per-pair percentage changes**, current
relative to main; negative means lower cost. They need not equal percentage
changes calculated from the independently aggregated timing cells above.

| Tier | Frame p50 change | Caller CPU p50 change | Process CPU p50 change |
| --- | --- | --- | --- |
| Easy | -16.3% | -61.0% | -61.1% |
| Medium | +1.4% | -74.6% | -75.6% |
| Hard | -16.0% | -88.3% | -88.9% |

Hard caller-CPU p50 changes in the individual pairs are -87.7%, -88.3%, -92.9%.
Repeat variation remains material:

| Tier | Main frame p50 range | Current frame p50 range |
| --- | --- | --- |
| Easy | 2.677–4.714 | 2.514–3.947 |
| Medium | 8.029–10.477 | 8.138–9.887 |
| Hard | 26.941–46.024 | 22.620–30.998 |

These are observations under contention, not isolated speedup guarantees.
The raw per-run values and p95/p99 paired changes are retained in `summary.json`.
The additional retained records/gather cache on Hard cost only a few MiB,
a minor accounting change here; individual resource counters and process RSS
remain in the raw data.

### Work and sampled output parity

Every paired measured frame has identical selected main triangles/instances,
culled counts, light/map counts, shadow triangles/caster copies, skin matrices,
visible skinned copies, recipe populations, moving-transform sums and camera
route. The implementations legitimately differ in command counts, cached work,
resident memory and uploads; those are measured rather than required to match.

| Tier | Main / current main-pass draws | Main / current shadow draws | Unchanged shadow triangles |
| --- | --- | --- | --- |
| Easy | 44 / 44 | 166 / 42 | 1,113,312 |
| Medium | 162 / 162 | 1,140 / 292 | 8,086,164 |
| Hard | 8,710 / 317 | 4,712 / 1,096 | 40,941,344 |

All **9/9 sampled image pairs are decoded-RGBA identical**, at route frames
1, 541 and 961 across all three tiers.
The [paired-image gallery](../out/runs/render3d-main-refresh-32f387ea-6aadd75e/quality-gallery.html)
provides the full captures. This verifies sampled output, not every possible view
or material combination.
The branch also contains the per-surface shadow-state bug fix; these Yard samples
do not exercise every affected surface combination. Frame-time tails do not
establish flicker or general runtime stability.

### Separate hardware GPU diagnostics

One additional full-route GPU pair per tier uses the same Vulkan timestamp
library and the same 13 nonnested bottom-of-pipe pass markers in both versions.
These **synchronous diagnostics are separate from normal throughput**. Waiting
for marked submissions changes pacing; spans can include external contention,
preemption and dependency gaps. The span sum excludes unmarked uploads, 2D
composition and presentation. CPU/GPU and independent pass percentiles must
not be added. A single pair per tier cannot establish a repeatable GPU speedup.
Cells are p50 / p95 / p99 ms.

| Tier | Main 13-span sum | Current 13-span sum | Main shadow span | Current shadow span |
| --- | --- | --- | --- | --- |
| Easy | 2.019 / 4.232 / 4.456 | 1.903 / 4.011 / 4.448 | 0.245 / 0.302 / 0.308 | 0.239 / 0.260 / 0.267 |
| Medium | 8.796 / 16.410 / 17.463 | 8.661 / 16.313 / 17.898 | 3.746 / 4.401 / 6.932 | 3.901 / 4.302 / 4.517 |
| Hard | 20.354 / 46.422 / 49.911 | 24.763 / 41.423 / 55.835 | 11.986 / 20.792 / 20.834 | 13.919 / 27.937 / 29.475 |

This single-pair check does not demonstrate a GPU speedup. Hard marked-span p50
is higher on current (24.763 versus 20.354 ms), while Easy/Medium totals are
close. These observations neither establish a GPU regression nor recover an
isolated optimization effect under changing contention.

Each of the six GPU runs has 1,741 contiguous complete 13-pass groups. CSV
frames 301–1740 map to submissions 302–1741. Every exported non-timing work,
cache, resource and upload column matches that version's fresh normal control
at every measured frame. The main/current geometry checks also pass between
the two GPU runs. Full main-shading and other pass distributions are retained.

All **42 processes** completed: 18 normal, 18 capture and six GPU diagnostic
runs. The existing RAM guards remain; the process deadline is 450 seconds.
Commands, before/after telemetry, source/binary/harness/library/recipe hashes,
raw timings, exact-image checks and final validation are under
`out/runs/render3d-main-refresh-32f387ea-6aadd75e/` (`provenance.json`,
`summary.json`, `capture-checks.json`, `route-checks.json`, `checks.json`).
No engine code changed, so no C++ rebuild or engine test suite was required.

## Fresh cross-engine comparison, 2026-11-12

Current Kin `32f387ea` on `perf/render3d-cpu-parity` is compared with installed
Godot **4.7.2.stable.official.ed1daf0bf** (Forward+) and Unreal
**5.8.4-58902052+++UE5+Release-5.8** (Shipping, Vulkan).
Kin includes the shadow/culling/instance/draw-plan redesigns, map-wide stock
shadow batching and direct shadow-index scatter. Existing frozen executables
were reused; no renderer, shader or engine settings were changed during timing.

**All measurements are contended.** Blender and every external workload stayed
running, as requested. This i9-13900HX / RTX 4080 Laptop host has uncontrolled
CPU/GPU clocks, temperature and background load. Rotation reduces order bias;
it does not make the load constant. These results describe this campaign,
not isolated hardware performance or an intrinsic engine stability ranking.

### Frame intervals

Cells are **p50 / p95 / p99 milliseconds**, lower is better. Each cell is the
median of three per-run percentiles, each from a complete 1,440-frame route.
p50 uses the sample median; p95/p99 use sorted index `floor((N-1)*p/100)`.
The 27 normal runs contain **38,880 measured frames**. No screenshot readback
or Kin synchronous GPU-query instrumentation runs during these measurements.

| Tier | Kin `32f387ea` | Godot 4.7.2 | Unreal 5.8.4 Shipping |
| --- | --- | --- | --- |
| Easy | 4.429 / 7.390 / 8.618 | 3.716 / 6.557 / 6.958 | 3.515 / 23.053 / 70.609 |
| Medium | 17.073 / 29.594 / 32.480 | 14.524 / 18.657 / 21.223 | 9.487 / 85.884 / 89.394 |
| Hard | 34.673 / 59.876 / 61.971 | 61.933 / 66.067 / 79.431 | 23.100 / 115.904 / 128.049 |

Kin's displayed Hard median lies between Godot and Unreal. Unreal has lower
aggregate medians in all three tiers but much larger observed upper percentiles.
Repeat variation is material, especially on Medium:

| Tier | Kin p50 range, ms | Godot p50 range, ms | Unreal p50 range, ms |
| --- | --- | --- | --- |
| Easy | 2.247–4.855 | 2.845–4.599 | 2.795–3.774 |
| Medium | 17.072–24.177 | 14.326–32.094 | 8.682–49.031 |
| Hard | 29.872–36.131 | 61.846–62.151 | 15.536–25.925 |

For completeness, the following are medians of the three matched-block
percentile ratios; a value above one means higher Kin frame time. They are
**not** ratios of the independently aggregated table cells:

| Tier | Kin / Godot p50/p95/p99 | Kin / Unreal p50/p95/p99 |
| --- | --- | --- |
| Easy | 0.963 / 0.818 / 1.030 | 1.260 / 0.343 / 0.122 |
| Medium | 1.175 / 1.381 / 1.397 | 1.800 / 0.305 / 0.336 |
| Hard | 0.558 / 0.910 / 0.780 | 1.564 / 0.517 / 0.484 |

Easy illustrates why a stable ordering cannot be claimed: its median paired
Kin/Godot p50 ratio is 0.963, while the ratio of aggregate medians is 1.192.
Both calculations are valid descriptions of different aggregations; neither
removes changing host load. Do not interpret this refresh as an isolated
speedup over any historical campaign.

### Workload and adapter limits

All engines consume the same hashed Industrial Yard recipes, meshes and full
camera route at 1280×720. Each run has 300 stationary warm-up frames, then
1,440 measured frames. CPU affinity is 0–15; Kin uses four helpers, Godot a
four-worker pool with `--render-thread safe`, and Unreal its own scheduling
within that affinity. Independent static objects are used; engine batching and
culling remain active. The 215 shared structure objects include six glass
windows. The tiers contain:

| Tier | Props | Skinned actors | Smoke instances | PBR materials | Moving props | Point / shadowed spot lights |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| Easy | 2,000 | 32 | 128 | 16 | 40 | 8 / 2 |
| Medium | 8,000 | 128 | 512 | 64 | 800 | 16 / 4 |
| Hard | 20,000 | 256 | 2,000 | 128 | 5,000 | 16 / 8 |

The recipes share ten meshes / 4,378 unique source triangles. Equal source
populations and instantiated triangle budgets do not imply equal visible
geometry, engine-counter meanings or rasterized work.

The matched feature policy is HDR/ACES, no AA, GI, AO or contact shadows,
uncapped/vsync off, classic spot shadows with nominal 1,024 resolution.
Unreal's spotlight resolution selection, filters and bias differ; the cap and
texels-per-pixel control are recorded. It uses no Nanite assets (the global
`r.Nanite` setting remains enabled), no Lumen/SSR or occlusion queries, and
zero pending PSOs on every measured frame. Kin's fixture keeps whole-map shadow
image caching **off**; Godot/Unreal retain their normal caches, including
Unreal `r.Shadow.CacheWholeSceneShadows=1`. These are actual adapter policies,
not identical shadow implementations. Kin is hidden with bounded frames in
flight; Godot/Unreal are windowed. Presentation and pacing differences remain
in the frame-interval comparison.

Every fresh Godot run uses the previously corrected **MULTIPLY-emission** pack,
SHA-256 `a880915af7626061e8559a25fe93dc2637d4baacd4f7b4ee54bf45e200c70bf4`.
The original speed campaign's ADD-emission pack had a known material mismatch.
No other Godot adapter behavior changed; old/new Godot timings are therefore
not a controlled performance comparison.

### CPU channels

All cells below are **p50 / p95 / p99 ms**, aggregated as above. Timer scopes
are different across engines and must not be added or used for a direct CPU
speed ratio. Kin's caller timer measures actual caller-thread CPU in the
renderer span, excluding blocked waits; process CPU includes helpers in that
span. These exclude the separate application scene-build scope.

| Tier | Caller CPU | Process CPU incl. helpers |
| --- | --- | --- |
| Easy | 0.473 / 0.535 / 0.626 | 0.565 / 0.672 / 0.744 |
| Medium | 1.366 / 1.571 / 1.739 | 1.546 / 1.764 / 1.949 |
| Hard | 3.561 / 4.088 / 4.716 | 4.463 / 5.081 / 5.674 |

Godot reports script scene-update wall time and its measured render CPU for the
subviewport. These do not cover the same work as Kin's caller/process timers:

| Tier | Script scene-update wall | Reported viewport render CPU |
| --- | --- | --- |
| Easy | 0.205 / 0.231 / 0.285 | 1.662 / 1.855 / 2.136 |
| Medium | 2.449 / 2.597 / 3.172 | 9.893 / 10.753 / 11.194 |
| Hard | 13.968 / 14.440 / 16.414 | 42.884 / 45.057 / 46.195 |

Unreal scene-update is adapter wall time. Its thread timers are latest-completed
stat-unit values; they can lag, overlap and include waiting. In particular,
RHI-thread time is not pure CPU execution:

| Tier | Scene-update wall | Game thread | Render thread | RHI thread |
| --- | --- | --- | --- | --- |
| Easy | 0.034 / 0.047 / 0.054 | 0.601 / 0.831 / 0.949 | 1.573 / 2.084 / 2.346 | 2.400 / 22.053 / 69.582 |
| Medium | 0.234 / 0.310 / 0.333 | 1.135 / 1.374 / 1.516 | 2.909 / 3.462 / 3.897 | 7.759 / 84.091 / 87.502 |
| Hard | 1.105 / 1.355 / 1.445 | 2.403 / 2.844 / 3.011 | 5.755 / 6.911 / 7.472 | 20.170 / 112.720 / 124.402 |

Unreal frame interval correlates with its reported RHI timer at Pearson
0.998–1.000 and GPU timer at 0.941–0.996 across the nine normal runs. This
suggests RHI/GPU pacing or contention contributes to its tails; delayed timer
scopes prevent causal attribution. The adapter accumulates CSV rows in memory
and writes them once at finish, rather than flushing a file every frame.
No engine crashes or bounded-run terminations occurred in the 45 processes.
That and frame-time tails do not establish general runtime or temporal stability.

### GPU diagnostics

These normal-run asynchronous counters have **different GPU scopes**: Godot's
subviewport versus Unreal's latest-completed RHI GPU frame. Cells are
p50 / p95 / p99 ms; they are retained as engine diagnostics, not interchangeable
GPU work measurements:

| Tier | Godot subviewport GPU | Unreal RHI GPU frame |
| --- | --- | --- |
| Easy | 3.672 / 6.472 / 6.798 | 3.042 / 22.836 / 70.403 |
| Medium | 10.678 / 18.352 / 21.047 | 9.567 / 85.471 / 88.839 |
| Hard | 34.502 / 62.538 / 78.821 | 22.603 / 116.553 / 126.052 |

Kin hardware timestamps were collected in **three separate, later synchronous
diagnostic runs**, one full route per tier. The existing Vulkan forwarding
library waits for marked submissions and records 13 nonnested bottom-of-pipe
pass spans. Their sum excludes unmarked uploads, 2D composition and present;
contention, preemption and dependency gaps can contribute. These values are
**not normal throughput**, are not a decomposition of the frame table, and
must not be directly ranked against Godot/Unreal GPU counters. In particular,
Kin's Easy 12.965 ms diagnostic sum and 4.429 ms normal frame median come from
different runs/load and measurement policies. CPU/GPU and independent pass
percentiles must not be added.

| Tier | Kin 13-span sum | Main shading | Local shadows |
| --- | --- | --- | --- |
| Easy | 12.965 / 14.853 / 17.542 | 11.999 / 13.878 / 14.526 | 0.816 / 0.958 / 3.136 |
| Medium | 46.115 / 66.850 / 70.106 | 23.289 / 43.639 / 45.735 | 22.788 / 24.563 / 25.112 |
| Hard | 83.579 / 122.859 / 127.298 | 34.668 / 64.715 / 68.897 | 50.240 / 58.632 / 61.067 |

Within the Hard diagnostic, local shadows account for 56.9% of mean marked
span time and main shading 43.0%. This supports further shadow-GPU investigation;
it does not isolate vertex, raster, bandwidth or shader cost. Each diagnostic
has 1,741 contiguous 13-pass groups; measured CSV frames 301–1740 map to
submissions 302–1741. All exported non-timing work/cache/resource/upload columns
match the corresponding fresh normal Kin control at every measured frame.

### Quality, validation and evidence

All nine three-engine normal-route blocks passed shared source/configuration,
per-frame route and moving-transform checks. Godot/Unreal actual motion and
skinning readbacks pass twice per run. Draw/triangle counters remain
engine-specific and are not required to match.

All **15 fresh captures** were inspected: all three tiers at route frame 60,
and Medium warehouse/elevated views at frames 540/1140. Framing, large structures
and populated-yard silhouettes correspond; no major missing structure was
observed at these viewpoints. Corrected Godot emission brings its materials
closer to Kin. Unreal has a darker sky/shadow appearance; emissive highlights,
smoke contrast/opacity, specular response and shadow edges vary. These are
sampled visual observations, not pixel parity or an objective quality score.
Still images cannot establish flicker or temporal stability.

Artifacts are under `out/runs/render3d-engine-refresh-32f387ea/`: `summary.json`,
all raw CSVs/logs, exact commands and before/after telemetry, `provenance.json`
with binary/source/recipe/cooked-asset hashes, `route-checks.json`,
`quality-checks.json`, `unreal-timing-relationships.json`, `checks.json` and the
[15-image gallery](../out/runs/render3d-engine-refresh-32f387ea/quality-gallery.html).
The task reused existing binaries and needed no C++ rebuild or engine-suite run;
validation covers the measurement scripts, data, assets and sampled output.

## GPU work

### Map-wide stock local-shadow GPU batches, 2026-11-12

The reference renderer is `ce7cbb90` (the GPU refresh is documentation-only
`650958fb`). Local-shadow preparation now joins compatible stock depth commands
across locality windows within each map. It reuses existing exact depth-state
IDs and preserves source/copy order inside each group. Depth-affecting surfaces,
transformed-UV cutouts, skinning, clusters and other levels remain barriers.
No shader math, selected geometry, resolution, filtering or image-cache policy
changes. Hard median shadow draws fall **4,712 → 1,096 (76.7%)**; shadow triangles
remain **40,941,344**, and caster copies **80,274**. Easy draws fall 166 → 42;
Medium 1,140 → 292. GPU buffers and upload sizes are unchanged; reusable CPU
command/index scratch and its memory accounting are added.

**This improves the shadow GPU span at an observed CPU cost.** Blender and all
external workloads stayed running throughout. Temperature, clocks and load are
uncontrolled; every result is **contended**, and there is no isolated FPS claim.

Ordinary alternating process pairs had very large timing swings. A temporary
same-process probe therefore rotated the old stream, 256-entry grouping and
map-wide grouping on successive renders. Three full routes use starting phases
0/1/2: each mode has 480 measured frames per run, 1,440 across the three runs,
and covers every route frame exactly once. Its old/global draw counts match the
frozen controls at every corresponding frame; all other exported non-timing
columns match. The temporary selector was removed from the final source.

Hard map-wide shadow p50 falls **31.6%** by median paired within-run
reduction, with per-run changes -31.6%, -34.6%, -21.5%.
The unchanged main pass serves as a contemporaneous control; clocks remain
uncontrolled. Bounded grouping is retained
only as diagnostic evidence; the final implementation needs no new switch.

| Hard interleaved mode | Shadow GPU p50/p95/p99, ms | Main GPU p50, ms | GPU 3D span-sum p50, ms |
| --- | --- | ---: | ---: |
| Original windows | 82.265 / 103.971 / 108.448 | 33.302 | 118.102 |
| 256-entry groups | 56.343 / 66.047 / 67.940 | 33.143 | 90.430 |
| Map-wide groups | 56.279 / 65.787 / 67.591 | 33.430 | 88.925 |

These are medians of three per-run percentiles, each from 480 mode samples.
The existing Vulkan forwarding library takes bottom-of-pipe hardware timestamps
at all 13 nonnested pass markers and waits for marked submissions. The span sum
excludes unmarked uploads, 2D composition and presentation. This diagnostic is
not normal throughput, and CPU/GPU values must not be added. Settings remain
720p HDR, no AA/GI/AO, independent Yard submissions, classic 1,024 spot maps,
four helpers, affinity 0–15, and 300 warm-up plus 1,440 measured route frames.

The separate-process campaign below shows the timing variation. Hard paired
shadow-p50 changes were −44.8%, +20.5% and
+42.9%, motivating the interleaved probe. Easy and Medium are single guard pairs.

| Tier | Separate-process shadow GPU before p50/p95/p99, ms | After, ms |
| --- | --- | --- |
| Easy | 0.280 / 0.312 / 0.327 | 0.377 / 0.447 / 0.513 |
| Medium | 9.887 / 11.008 / 14.927 | 9.824 / 10.975 / 11.380 |
| Hard | 13.219 / 25.174 / 35.988 | 14.388 / 34.523 / 37.820 |

Normal runs have no timestamp-query library or image readback. Hard caller
CPU p50 increases in all three pairs, with **8.4% median paired increase**
(individual changes +2.8%, +35.5%, +8.4%). This is an observed contended cost,
not an isolated attribution to compaction. Reduced recording work does not
make the new CPU index regrouping free. Percentiles below are medians of
per-run percentiles; dividing those medians gives a different percentage.

| Tier | Normal pairs | Caller CPU before p50/p95/p99, ms | After, ms |
| --- | ---: | --- | --- |
| Easy | 1 | 0.635 / 0.904 / 1.035 | 0.522 / 0.755 / 0.828 |
| Medium | 1 | 1.733 / 2.423 / 2.828 | 1.859 / 2.623 / 2.906 |
| Hard | 3 | 3.871 / 5.427 / 6.085 | 4.471 / 5.687 / 6.123 |

Every measured frame in the ten full-route before/after pairs (five hardware
GPU pairs and five normal pairs) matches every exported non-timing column
except the intended `local_shadow_draws` change. Nine decoded-RGBA image pairs
match exactly at route phases 1, 541 and 961 across Easy/Medium/Hard. A focused
regression compares all 32 depth bits at probed texels with a small independent
map, across repeated stock states, reversed submissions and special barriers.
Nine NVIDIA renderer suites passed; four relevant suites passed again after
the regression/shader-probe rebuild. Four Lavapipe suites passed with no skips.
These sampled parity and focused stability checks do not cover every scene.

Evidence, frozen binary/source hashes, commands, telemetry, all raw timings,
mode-mapping checks and exact-image hashes are in
`out/runs/render3d-gpu-shadow-batches/`. The retained change preserves the
existing compiler, gather-cache programs, shadow fingerprints and source-ordered
union packing; only its final stock command/index stream is compacted.

### GPU baseline pass attribution, 2026-11-12

The reference is the frozen `ce7cbb90` binary on `perf/render3d-cpu-parity`.
Three rotated runs per tier use the original independent Industrial Yard
submissions: 1280×720 HDR, **no AA/GI/AO**, classic 1,024 spot maps, four helpers,
CPU affinity 0–15, 300 warm-up frames and the full 1,440-frame route. Blender
and all background jobs remain running; clocks, temperature and load are
uncontrolled: these results are **contended**. Before/after telemetry is stored
for each run, rather than assumed constant throughout it.

The existing diagnostic Vulkan forwarding library records bottom-of-pipe hardware
timestamps at the 13 existing, nonnested `kin3d` pass markers. It waits for each
marked submission to complete. These are **synchronous pass-span attribution
runs, not normal throughput measurements**. GPU contention/preemption and
pipeline dependencies can contribute to a span. The total below sums those
3D spans; it excludes uploads, 2D composition and presentation outside markers.
CPU marker durations and fence timings are not substituted for GPU timestamps,
and CPU/GPU columns must not be added.

| Tier | GPU 3D span-sum p50/p95/p99, ms | Main shading, mean share | Local shadow maps, mean share |
| --- | --- | ---: | ---: |
| Easy | 1.220 / 2.203 / 2.522 | 81.4% | 16.6% |
| Medium | 3.755 / 6.670 / 7.034 | 65.3% | 34.0% |
| Hard | 12.561 / 23.318 / 32.747 | 45.6% | 54.2% |

| Tier | Main shading p50/p95/p99, ms | Local shadow maps p50/p95/p99, ms |
| --- | --- | --- |
| Easy | 0.989 / 1.907 / 2.174 | 0.198 / 0.202 / 0.204 |
| Medium | 2.384 / 5.193 / 5.485 | 1.342 / 1.823 / 1.885 |
| Hard | 5.618 / 9.931 / 12.099 | 6.703 / 9.966 / 15.061 |

Percentiles are medians of three per-run percentiles. Shares are medians of
per-run mean-pass/mean-total ratios; independently aggregated shares need not
sum to exactly 100%. Pass percentiles are not additive. The full thirteen-pass
breakdown and per-frame values remain in the task artifacts.

Across the three runs, total-span p50 ranges are **1.155–1.393 ms** for Easy,
**3.743–4.456 ms** for Medium and **11.883–13.280 ms** for Hard. The earlier
v0.3.0 GPU profile used 4× MSAA and an older renderer/submission path. These
fresh no-AA results establish a new baseline; they do not establish an isolated
old-to-new GPU speedup or a cross-engine ranking.

**Hard now warrants local-shadow GPU investigation first:** shadow-map rendering
accounts for about 54% of its measured spans, versus 46% for main shading.
Its median workload submits **4,712 shadow draws / 40,941,344 shadow triangles**
and **317 main draws / 5,868,509 main triangles** per measured frame. Main shading
is the larger target on Easy and Medium, at about 81% and 65%. These timestamps
do not distinguish vertex processing, raster/depth work, bandwidth or texture
sampling within a pass. No authored quality settings or renderer implementation
were changed.

Every one of the **12,960 measured frame/pass sets** contains all 13 pass names
in the expected order with finite, nonnegative hardware durations. Each full
run has 1,741 contiguous marked submissions: 301 unmeasured renders and 1,440
measured renders. Yard starts at frame zero, so measured CSV frames 301–1740
map explicitly to submissions 302–1741. The complete exported non-timing
work/cache/resource/upload columns match the current uninstrumented controls.
Three decoded-RGBA captures, one per tier at route frame one, match the existing
normal captures exactly. Timestamp logs identify the RTX 4080 Laptop GPU with
1 ns timestamp period and 64 valid bits.

Commands, current binary/library/recipe hashes, telemetry, mapping checks, raw
pass timings, p50/p95/p99 tables and exact-image checks are under
`out/runs/render3d-gpu-refresh-ce7cbb90/`. This refresh reuses the existing binary
and profiling library; it requires no engine rebuild or shader/renderer edits.

## CPU work

### Direct shadow-index scatter, 2026-11-12

The reference is `6b2e3872`, with map-wide stock local-shadow batching enabled.
Window compilation now emits command counts and read-only source spans.
Grouping determines final offsets, then the original source stream scatters
directly into the GPU index list. Every active index is written once; the
second temporary CPU index list and its reorder copy are removed. The common
scatter path keeps a source pointer, remaining count and base in local variables;
commands fitting inside that span avoid the boundary loop and metadata reload.

Cached static programs retain their existing representation and budget. Cold
admission replaces temporary row spans with one span into the owned program.
Current-call spans read immutable cached data or independent live selections.
Source-ordered union slots, joints, cluster runs, compatibility IDs, specialized
barriers, small-pass order, shadow fingerprints and image-cache policy retain
their rules. GPU shaders, commands, resources and upload sizes are unchanged.

The final source reuses the existing Release/Ninja build and renderer `-O2`
settings. All Blender/background jobs stayed running. CPU frequency, GPU clocks,
thermals and system load remain uncontrolled: **all results are contended**.

Normal comparisons use independent Yard submissions, 720p HDR, no AA/GI/AO,
classic 1,024 spot maps, four helpers, affinity 0–15, 300 warm-up frames and
the full 1,440-frame measured route. Three alternating Hard pairs and one
Easy/Medium guard pair use frozen binaries, without diagnostic logging,
timestamp-query waits or image readback during timing.

One reference attempt reached the original 300-second process limit after
1,299 measured frames. Its incomplete data is preserved and excluded; the
remaining runs use a 450-second limit with the same memory bound and scene
settings. This was a bounded-process termination, not an engine crash.

Hard caller CPU p50 changes by **-1.09%** using the median
paired change; 2 of three pairs improve. Individual changes are +11.86%, -7.50%, -1.09%.
This does not establish recovery of the earlier contended 8.4% increase.
Percentiles below are medians of per-run percentiles; dividing those medians
gives a different percentage from the paired calculation.

| Tier | Normal pairs | Caller CPU before p50/p95/p99, ms | After, ms |
| --- | ---: | --- | --- |
| Easy | 1 | 0.486 / 0.560 / 0.614 | 0.474 / 0.565 / 0.649 |
| Medium | 1 | 1.472 / 1.678 / 1.775 | 1.457 / 1.670 / 1.737 |
| Hard | 3 | 3.757 / 4.571 / 5.887 | 3.717 / 4.114 / 6.628 |

A temporary same-process diagnostic alternates the unmodified reference
compiler and fused compiler on successive renders. Two full Hard routes flip
the starting phase: 720 measured samples per mode per run, 1,440 per mode in
total, with every route frame exercised once by each. `CLOCK_THREAD_CPUTIME_ID`
times batch-key setup, window/program compilation, grouping and scatter. It
excludes selection, packing, recording, fingerprint work and logging. This is a
compiler slice of caller CPU; it must not be added to the caller CPU table.

Hard compiler p50 falls **2.5%** by median paired within-run
change (-3.0%, -1.9%). The first source-span prototype had essentially
flat compiler time; caching the active cursor removes its per-command overhead.
Its earlier results remain under `out/runs/render3d-shadow-direct-scatter/`.

| Hard compiler | CPU p50/p95/p99, ms |
| --- | --- |
| Reference | 0.480 / 1.196 / 1.559 |
| Fused cursor | 0.468 / 1.163 / 1.546 |

Compiler percentiles are medians of two per-run percentiles. Easy and Medium
diagnostics use only 120 measured route-prefix frames per run (60 per mode),
after the same warm-up; they are correctness/overhead guards, not full-route
performance repeats. Their median paired compiler-p50 changes are +1.0% for
Easy and -0.4% for Medium.

The diagnostic also reports `local_copy_bytes`, the capacity-based index/span
scratch metric, independently of the retained gather-program payload. The table
uses median mode values across the two runs. It describes that diagnostic’s
shared allocation history; it is not total process memory or GPU memory. Small
passes can retain more span metadata, while Hard removes a large index mirror.

| Tier | Reference reported scratch, KiB | Fused reported scratch, KiB |
| --- | ---: | ---: |
| Easy | 49.2 | 104.0 |
| Medium | 416.8 | 306.0 |
| Hard | 1858.0 | 1294.8 |

Two independent 64-bit fingerprints cover projections/view ranges, depth
commands, the active caster list, union-copy mapping, joint indices and cluster
runs. They match across all **2,583 corresponding render pairs**, including cold
and warmed states: 1,741 Hard and 421 each Easy/Medium. Each paired render
exercises opposite compiler modes. This preserves the command/index stream of
the prior GPU optimization, including **1,096 Hard shadow draws**. It does not
re-estimate the earlier GPU timing gain.

Every exported non-timing column matches in all five normal full-route pairs,
with no exceptions. Nine decoded-RGBA pairs match exactly at route phases 1,
541 and 961 across all tiers. Nine focused NVIDIA suites and four Lavapipe
suites pass with no skips. The direct-cache regression now exercises repeated
small/full/zero gather-budget transitions with mixed live rows and point-face
holes, checking pixels, picks, geometry, draw counts and budget limits. Existing
local-graph tests cover all 32 depth bits at probed texels and specialized paths.

Temporary compiler selection, clocks and fingerprint logging are removed from
the final source. Frozen source/binary hashes, raw measurements, telemetry,
commands, stream checks and exact-image checks are under
`out/runs/render3d-shadow-direct-scatter-fast/`.

### Dense per-view outputs and stable index compaction, 2026-11-12

The reference is `39c30d0a`. Draw references no longer duplicate per-view
first/count/extra values. Single-level draws reuse the existing three dense
level-output words; multi-level draws reserve three separate aggregate words
before their per-level arrays. Main ordering, packing and sun recording read
that current output view. General multi-level reduction retains the original
sums; single-level reduction uses its already-complete counts.

Resident camera-only prefix runs now write contiguous result triples without
mutating every large draw reference. Stable index compaction writes the next
reserved slot and advances only for accepted copies; later accepted copies
overwrite rejected candidates, producing the exact original active index stream.
The input's complete retained-copy count proves the reserved-space bound. Counts,
offsets and visibility refresh every render. Existing key/shape/backing rebinds,
16 MiB layout admission, worker boundaries, source order, arithmetic, selected
geometry, GPU commands, resources and uploads retain their rules. This introduces
no visibility-result cache or new switch.

Two temporary full-route probes separated inclusive caller CPU into source
refresh/retention, work layout, bounds setup, classification, prefixes/statistics,
retained-index/history work, packing schedule and packing/uploads. Prefix p50
was **0.209–0.228 ms**, versus **0.082–0.103 ms** for classification; work-layout
setup was about **0.001 ms**. Source refresh/retention was **0.367–0.473 ms**;
a finer probe found direct scene-table claims about **0.0013 ms**, key/GC work
**0.0105 ms**, and live batch retention/bounds **0.459 ms**. These are diagnostic
phase medians from different contended runs, inclusive of nested calls; they
are not additive or directly comparable to exclusive sampled phase shares.
All temporary timers/logging were removed before freezing the normal candidate.

Five alternating Hard pairs show **7.2% median paired caller-CPU p50 reduction**,
improving in four of five; individual changes are **−30.1%, −32.6%, −2.6%, +30.2%
and −7.2%**. Process CPU falls **5.4%** by the same calculation. Conditions are
original independent Yard submissions, 720p HDR, no AA/GI/AO, classic 1,024 spot
maps, four helpers, affinity 0–15, and 300 warm-up plus 1,440 measured route
frames. Blender/background jobs stay untouched; clocks, thermals and load are
uncontrolled: results are **contended**. PMU collection and image readback run
separately from normal timing.

| Tier | Pairs | Caller CPU before p50/p95/p99, ms | After, ms | Paired p50 reduction |
| --- | ---: | --- | --- | ---: |
| Easy | 1 | 0.343 / 0.475 / 0.532 | 0.285 / 0.387 / 0.454 | 16.8% |
| Medium | 1 | 1.360 / 1.777 / 2.002 | 1.125 / 1.356 / 1.597 | 17.3% |
| Hard | 5 | 3.574 / 4.657 / 5.130 | 2.729 / 4.257 / 5.103 | 7.2% |

Percentiles are medians of per-run percentiles; paired reductions compare
adjacent run medians. Dividing the independently aggregated Hard p50 values
gives a different percentage. Easy and Medium are single guard pairs. The wide
Hard range prevents a stable isolated-speedup claim. Hard frame p50 has a median
paired reduction of 2.9%, with changes spanning −84.8% to +265.8%; normal Hard
processes took roughly 32–165 seconds as GPU load changed. These observations
do not establish isolated throughput or a new cross-engine ranking. CPU gains
do not compound with earlier campaigns.

Every exported non-timing per-frame work/cache/resource/upload column matches,
with no exceptions. The CPU `prepared_layout_bytes` metric reflects the changed
private layout, but the frozen Yard application exports no layout-byte column
in CSV or its run-report state. Hard peak process RSS, including startup and
driver memory, is 196.2 → 242.0 MiB; no total-process memory reduction is claimed.

Two separate forward/reverse inherited P-core controls, after both binaries
completed the normal campaign, show **+0.118% and +0.146% retired user
instructions**, essentially flat. User-cycle counts fall **10.06% and 3.49%**.
Event running coverage is 99%. Each includes startup, 300 warm-up plus **720
measured frames (half the route)**, helpers, driver threads, scene assembly and
presentation. These are whole-process counts; cycles are not clock-normalized,
and no isolated reduction in executed instructions is claimed. There is no
full-route extrapolation. Every exported work column matches in these controls.

Fresh before/after `cpu-clock:u` profiles each cover one full 1,440-frame Hard
route at 499 Hz with 16 KiB DWARF user stacks. Actual render-call windows exclude
assembly, warm-up and presentation; caller, Kin workers and other threads are
separate. **Camera/instance preparation falls from 22.93% to 18.36% of caller
samples**. These are normalized shares, not phase medians or an independent
speedup estimate. Remaining caller shares are local-shadow preparation 21.03%,
draw planning/lookup 20.47%, shadow recording 11.88% and input validation 10.05%.

Caller samples number 2,887/2,130, with 1.35%/0.75% unresolved phase attribution.
Every exported profile work column matches its normal control. Normal timing
above uses the adjacent unsampled controls.

Nine focused NVIDIA suites and four llvmpipe suites pass. The direct-cache suite
adds repeated fully hidden/visible views and culling changes while switching sun
shadows through zero, one and four cascades. Exact pixels, picking and work match
the streamed reference. Existing suites cover three-level LOD, CPU/GPU clusters,
transparent packing, skins/history, explicit prepared views, source mutation,
budget fallback and preparation lifetime. All **10,080 paired route frames**
preserve every exported non-timing work column. Nine decoded-RGBA pairs across
three tiers at route phases 1/541/961 are pixel-identical.

The rejected conditional-empty-write prototype completed Hard caller-p50 changes
of +5.1%, +9.8% and −2.5%. All raw results and its frozen binary remain under
`out/runs/render3d-cull-work-redesign/`, alongside the diagnostic phase probes;
they are not pooled with the retained candidate. Its proposed load-dependency
mechanism was an inference. Frozen hashes, commands, raw controls, exact captures,
instruction counts and fresh profiles for the dense-output version are under
`out/runs/render3d-cull-dense-outputs/`.

### Reuse exact depth-state groups during shadow recording, 2026-11-12

The reference is `54e0a2aa`. Large multi-map local-shadow recording now reuses
the exact depth-state groups already interned by selection preparation. Different
source representatives in light maps and locality windows resolve once into a
compact record holding the current pipeline, retained/streamed instance table,
mesh and level-zero index range. Ordinary two-buffer commands use direct buffer
comparisons/assignments through the same `DrawBinder` state as general commands.
This removes repeated material/ref classification, range lookup and variable
buffer-array comparison/copy work while preserving pipeline-triggered storage
rebinding and pooled vertex/index offsets.

The table requires at least two drawn maps and 128 selected entries. Masked,
depth-affecting surface, skinned, clustered and higher-level commands retain the
general recorder. Current pipeline and table handles refresh before use every
recording call; failed pipeline allocations retain per-entry retries. This
introduces no persistent result cache, GPU allocation, selection/sort/merge change,
command reordering or shader/uniform-addressing change. Source lookup costs four
bytes per ref and records 32 bytes per reserved slot on the measured x64 build;
vector capacity is retained as scratch outside the selection-plan budget.

Five alternating Hard pairs show **4.0% median paired caller-CPU p50 reduction**,
improving in all five; individual reductions are **2.8%, 7.3%, 3.6%, 4.0% and
8.9%**. Process CPU falls **3.5%** by the same calculation. Conditions are
original independent Yard submissions, 720p HDR, no AA/GI/AO, classic 1,024 spot
maps, four helpers, affinity 0–15, and 300 warm-up plus 1,440 measured route
frames. Blender/background jobs stay untouched; clocks, thermals and load are
uncontrolled: results are **contended**. PMU collection and image readback run
separately from normal timing.

| Tier | Pairs | Caller CPU before p50/p95/p99, ms | After, ms | Paired p50 reduction |
| --- | ---: | --- | --- | ---: |
| Easy | 1 | 0.479 / 0.550 / 0.648 | 0.475 / 0.528 / 0.594 | 0.7% |
| Medium | 1 | 1.389 / 1.557 / 1.961 | 1.361 / 1.490 / 1.575 | 2.1% |
| Hard | 5 | 3.821 / 4.391 / 5.114 | 3.669 / 3.978 / 4.267 | 4.0% |

Percentiles are medians of per-run percentiles; paired reductions compare
adjacent run medians. Easy and Medium are single guard pairs. Hard frame p50
regresses a median 1.3%, with paired changes spanning −10.7% to +10.3%; Medium
frame p50 regresses 4.8%. These contended frame results do not establish an
isolated throughput gain or a new cross-engine ranking. CPU gains do not compound
with earlier campaigns.

Every non-timing per-frame work/cache/resource/upload column matches, with no
counter exceptions. Hard peak process RSS, including startup and driver memory,
is 198.0 → 241.3 MiB; the first candidate process has that peak, with subsequent
candidate runs at 196.2–196.4 MiB. No total-process memory reduction is claimed.

Two separate forward/reverse inherited P-core controls, after both binaries
completed the normal campaign, show **1.893% and 1.868% fewer retired user
instructions**, at 99–100% event running coverage. Each includes startup,
300 warm-up plus **720 measured frames (half the route)**, helpers, driver
threads, scene assembly and presentation. These are whole-process user counts,
not isolated render-phase counts or a clock-normalized speedup. There is no
full-route extrapolation. Every non-timing work column matches in these controls.

Fresh before/after `cpu-clock:u` profiles each cover one full 1,440-frame Hard
route at 499 Hz with 16 KiB DWARF user stacks. Actual render-call windows exclude
assembly, warm-up and presentation; caller, Kin workers and other threads are
separate. **Shadow recording falls from 20.12% to 10.53% of caller samples**.
These are normalized user-CPU shares, not phase medians; the paired total-CPU
reduction above remains the timing result. Remaining caller shares are
camera/instance preparation 24.26%, local-shadow preparation 21.51%, draw
planning/lookup 16.96% and material input validation 9.25%.

Caller samples number 2,724/2,659, with 1.06%/1.05% unresolved phase attribution.
Every profile work column matches its normal control. Normal timing above uses
the adjacent unsampled controls.

Nine focused NVIDIA suites and four llvmpipe suites pass. The existing direct-cache
fixture now interleaves over 1,025 multi-copy source rows with transformed cutouts,
both culling variants, retained live batches and skinned draws across spot/point
maps, cache hits/misses and source/projection changes. It compares exact pixels,
picks and shadow work with the streamed reference, including sparse wanted cube
faces. Other focused suites cover surface stages, LOD/clusters, preparation
lifetimes, planar views, GI and attachment graphs. All **10,080 paired route
frames** preserve every non-timing work column. Nine decoded-RGBA pairs across
three tiers at route phases 1/541/961 are pixel-identical.

The earlier per-ref prototype completed three Hard pairs with 1.85%, 0.37% and
1.26% caller-p50 reductions before being superseded by reuse of existing depth-state
groups. Those runs and frozen binary remain separately under
`out/runs/render3d-shadow-recording-sources/`; they are not pooled into the final
results. Commands, source/binary hashes, raw paired percentiles, exact captures,
instruction controls and fresh profiles for the retained version are under
`out/runs/render3d-shadow-recording-classes/`.

### Shared live-shadow caster selection, 2026-11-11

The reference is `861e5cdc`. When at least two wanted fixed-local shadow views
hit their existing static-selection plans, live single-level, unclustered
casters now share one source-ordered stream. Each four-sphere packet loads and
transposes once and reuses the existing sphere arithmetic across precompiled
view tests. Compact per-range opcodes retain current group plane/radius proofs.
One 64-bit visibility mask per classified source copy replaces per-view bucket
bytes. Final view/ref spans are sized once, counts become output cursors, and
one source pass distributes indices in their original order. Broad ranges split
by logical sphere/view test count to preserve worker parallelism.

Current bounds/projections refresh every render; this introduces no result cache.
Fresh/uncached projections, single-view, moon/GI, LOD and clustered cases keep
the general gather. Static admission, fingerprints, source/copy order, selected
casters, GPU depth commands, shaders, uploads and resource allocations retain
their rules. A four-light SIMD prototype was rejected after two full-route
caller-CPU regressions despite lower instruction counts; the retained version
keeps the existing four-caster lanes.

Five alternating Hard pairs show **6.8% median paired caller-CPU p50 reduction**,
improving in all five; individual reductions are **6.4%, 6.8%, 9.5%, 5.9% and
7.8%**. Process CPU falls **7.8%** by the same calculation. Conditions are
original independent Yard submissions, 720p HDR, no AA/GI/AO, classic 1,024 spot
maps, four helpers, affinity 0–15, and 300 warm-up plus 1,440 measured route
frames. Blender/background jobs stay untouched; clocks, thermals and load are
uncontrolled: results are **contended**. PMU collection and image readback run
separately from normal timing.

| Tier | Pairs | Caller CPU before p50/p95/p99, ms | After, ms | Paired p50 reduction |
| --- | ---: | --- | --- | ---: |
| Easy | 1 | 0.478 / 0.539 / 0.593 | 0.492 / 0.584 / 0.632 | −3.0% |
| Medium | 1 | 1.520 / 2.025 / 2.135 | 1.397 / 1.559 / 1.740 | 8.1% |
| Hard | 5 | 4.234 / 4.774 / 5.386 | 3.959 / 4.398 / 4.812 | 6.8% |

Percentiles are medians of per-run percentiles; paired reductions compare
adjacent run medians. Easy and Medium are single guard pairs. Easy's caller
p50 regresses 0.014 ms (3.0%), so this does not establish an improvement at every
tier. Hard frame p50 regresses in all five pairs, by a median 2.3%, with a
0.8–23.5% range. These contended frame results do not establish an isolated
throughput gain or a new cross-engine ranking. CPU gains do not compound with
earlier campaigns.

All non-timing per-frame work/cache/resource/upload columns match except the
documented CPU scratch counters: `local_task_bytes` covers shared ranges,
clipping opcodes and count/cursor payload; `local_bucket_bytes` covers visibility
masks. Hard task metadata falls **98,304 → 33,792 bytes (65.6%)**. Logical instance
tests remain 42,048 per frame. Peak process RSS, including startup/driver memory,
is 198.2 → 196.1 MiB for Hard; the scratch reduction is not a total-process memory
claim.

Two separate forward/reverse inherited P-core controls, after both binaries
completed the normal campaign, show **3.507% and 3.504% fewer retired user
instructions**, at 99–100% event running coverage. Each includes startup,
300 warm-up plus **720 measured frames (half the route)**, helpers, driver
threads, scene assembly and presentation. These are whole-process user counts,
not isolated render-phase counts or a clock-normalized speedup. There is no
full-route extrapolation. All work columns match apart from CPU scratch fields.

An earlier first-candidate-process diagnostic retired 76.13 versus 24.08 billion
instructions (a 216% regression); the following warm pair reduced instructions
3.52%. Its cause is unresolved. Both initial pairs remain in
`initial-instruction-diagnostics.json`; the steady comparison above uses the
repeated forward/reverse warm controls, not the first-process outlier. Startup
performance is not established by the steady comparison.

Fresh before/after `cpu-clock:u` profiles each cover one full 1,440-frame Hard
route at 499 Hz with 16 KiB DWARF user stacks. Actual render-call windows exclude
assembly, warm-up and presentation; caller, Kin workers and other threads are
separate. **Local-shadow preparation falls from 22.99% to 17.47% of caller
samples**. Worker shadow-preparation shares are 56.22% → 50.98%. These are
normalized user-CPU shares, not phase medians. Remaining caller shares are
shadow recording 20.75%, camera/instance preparation 20.55%, draw planning/lookup
17.74% and retained batch updates 7.82%.

Caller samples number 3,154/2,891, with 1.14%/1.38% unresolved phase attribution.
Profile work columns match their normal controls apart from the documented
scratch fields. Normal timing above uses the adjacent unsampled controls.

Nine focused NVIDIA suites and four llvmpipe suites pass. The existing local-graph
suite adds an independent scalar oracle over **8,192 sphere packets**, covering
both reusable-packet APIs and the original pointer API, 0–7 planes, partial
proofs, bounded/unbounded tests, tangencies, nextafter boundaries, signed zero,
NaN and infinity. The same oracle passes native SIMD and forced-portable builds.
The direct-cache suite adds moving live casters with cached static rows and
nonconsecutive wanted point-light cube faces. Existing mixed fresh/cached,
projection mutation, budget, skin/pose, LOD, pick, lifetime and same-frame-view
cases remain covered by the focused suites. All **10,080 paired route frames**
preserve work columns apart from the CPU scratch accounting above. Nine
decoded-RGBA pairs across three tiers at route phases 1/541/961 are pixel-identical.
Commands, frozen source/binary hashes, raw paired percentiles, rejected-prototype
notes, instruction controls and fresh profiles are under
`out/runs/render3d-live-shadow-packets/`; the rejected prototype's raw runs are
under `out/runs/render3d-live-shadow-stream/`.

### Axis masks for exact light-grid construction, 2026-11-11

The reference is `9348edcf`. Projected light bounds are Cartesian boxes over
X tiles, Y tiles and depth slices. CPU construction now records light membership
once per axis; intersecting the three masks gives a cell's exact light set.
Ascending set bits emit IDs in the original point-then-spot order, preserving
floating-point accumulation order. Box volumes determine the exact total index
count before allocation. At most 512 selected lights stay below the 12-bit
per-cell count limit, so saturation is unreachable; totals at or above the
20-bit index limit retain the original fallback.

The builder writes packed headers and indices sequentially, replacing per-box
counting, scattered writes and per-cell cursors. It retains the light,
sphere, box and mask arrays and avoids shrinking/zeroing the index payload
before filling it. Mask scratch uses `(X + Y + Z) × ceil(lights / 64)` words:
512 logical bytes for the Hard route's 24 lights at 720p, at most 4 KiB for
512 lights at that extent, excluding retained capacity. Current projection,
jitter, depth slicing, light records, shader ABI, GPU allocations/uploads and
all other rendering work are unchanged. There is no result cache or new switch.

Five alternating Hard pairs show **7.1% median paired caller-CPU p50 reduction**,
improving in all five; individual reductions are **5.3%, 7.1%, 7.0%, 9.1% and
13.1%**. Process CPU falls **6.2%** by the same calculation. Conditions are
original independent Yard submissions, 720p HDR, no AA/GI/AO, classic 1,024 spot
maps, four helpers, affinity 0–15, and 300 warm-up plus 1,440 measured route
frames. Blender/background jobs stay untouched; clocks, thermals and load are
uncontrolled: results are **contended**. PMU collection and image readback run
separately from normal timing.

| Tier | Pairs | Caller CPU before p50/p95/p99, ms | After, ms | Paired p50 reduction |
| --- | ---: | --- | --- | ---: |
| Easy | 1 | 0.496 / 0.680 / 0.758 | 0.360 / 0.429 / 0.493 | 27.4% |
| Medium | 1 | 1.575 / 1.925 / 2.073 | 1.107 / 1.375 / 1.556 | 29.7% |
| Hard | 5 | 4.294 / 5.128 / 5.705 | 3.989 / 4.641 / 5.208 | 7.1% |

Percentiles are medians of per-run percentiles; paired reductions compare
adjacent run medians. Easy and Medium are single guard pairs, not repeatability
studies. Hard frame p50 has a 9.9% median paired reduction, with individual
reductions ranging from −16.8% to 17.1%; Medium's frame p50 regresses 12.7%.
These contended frame results do not establish an isolated throughput gain or
a new cross-engine ranking. CPU gains do not compound with earlier campaigns.

Every non-timing per-frame work/cache/resource/upload column matches, with no
counter exceptions. Peak process RSS, including startup and driver allocations,
is 199.9 → 241.6 MiB for Hard; no total-process memory reduction is claimed.

Two separate forward/reverse inherited P-core instruction/cycle controls show
**9.565% and 9.596% fewer retired user instructions**, at 99–100% event running
coverage. Counts include helpers, driver threads, startup, warm-up, scene
assembly and presentation, not isolated render-phase counts or a clock-normalized
speedup. All non-timing work columns match in these controls too.

Fresh before/after `cpu-clock:u` profiles each cover two complete Hard routes at
499 Hz with 16 KiB DWARF user stacks. Actual render-call windows exclude assembly,
warm-up and presentation; caller, Kin workers and other threads are separate.
**Light-grid construction falls from 16.85% to 2.82% of caller samples.** These
normalized user-CPU shares are not phase medians. Remaining caller shares are
local-shadow preparation 23.95%, camera/instance preparation 19.32%, shadow
recording 18.36% and draw planning/lookup 15.33%.

Caller samples number 5,425/4,684, with under 1% unresolved phase attribution
in each. Normal timing above uses the fresh adjacent controls, not sampled runs.

Nine focused NVIDIA suites and four llvmpipe suites pass. The existing local-graph
suite adds an independent cell-by-cell oracle comparing **193 exact packed-grid
cases**: dense and sparse coverage, empty boxes, all 64-bit word boundaries up
to 512 lights, changing dimensions/counts, reuse, and just below/equal/above the
index limit. No additional test executable is introduced. All **10,080 paired
route frames** preserve every non-timing work/cache/resource/upload column.
Nine decoded-RGBA pairs across the three tiers at route phases 1/541/961 are
pixel-identical. Commands, frozen hashes, raw paired percentiles, instruction
controls and fresh profiles are under `out/runs/render3d-light-grid-redesign/`.

### Compiled opaque spans and direct batch emission, 2026-11-11

The reference is `db636c6f`. Stable source order, scene/pass/decal classes and
material-table compatibility now compile into contiguous opaque spans. The main
pass emits final batches directly, resolving one pipeline per eligible span.
Within a span only live instance-table boundaries need another compatibility
check; consecutive camera-index ranges copy together after explicit gap checks.
This removes temporary per-source main items, block rotations, repeated static
compatibility decisions and individual index inserts. Transparent meshes and
particles still sort by current depth in a separate stream. Current visibility,
pipeline generations, target variants, textures, samplers and fallback admission
remain live. Hidden incompatible spans permit the same visible adjacency as
before; equal-depth ties, decals, overlay blocks and all GPU work keep their order.

Five alternating Hard pairs show **8.2% median paired caller-CPU p50 reduction**,
improving in four of five pairs; individual reductions are **6.1%, −1.4%, 17.6%,
8.2% and 16.4%**. Process CPU falls **6.4%** by the same calculation. The unchanged
workload is independent Yard submissions, 720p HDR, no AA/GI/AO, classic 1,024
spot maps, four helpers, affinity 0–15, and 300 warm-up plus a full 1,440-frame
route per run. Blender/background workloads stay untouched; clocks, thermals
and load are uncontrolled: results are **contended**. Normal timing excludes
PMU sampling and image readback.

| Tier | Pairs | Caller CPU before p50/p95/p99, ms | After, ms | Paired p50 reduction |
| --- | ---: | --- | --- | ---: |
| Easy | 1 | 0.537 / 0.720 / 0.778 | 0.490 / 0.671 / 0.711 | 8.8% |
| Medium | 1 | 1.721 / 2.080 / 2.234 | 1.591 / 1.901 / 1.991 | 7.6% |
| Hard | 5 | 4.889 / 5.780 / 6.239 | 4.139 / 5.109 / 5.577 | 8.2% |

Percentiles are medians of per-run percentiles; paired reductions compare
adjacent run medians, so dividing independently aggregated p50s gives a different
result. Easy/Medium are single guard pairs, not repeatability studies. Hard frame
p50 has a 0.9% median paired increase; no frame-throughput gain or new engine
ranking is established. These numbers do not compound with earlier campaigns.

Hard median pipeline-cache hits fall **8,718 → 325**, while eight batched main draws,
317 main draws and 4,712 local-shadow draws remain unchanged. CPU plan capacity
grows only **192 bytes** (2,290,172 → 2,290,364 bytes) and shares the existing
4 MiB cap. Per-frame GPU-resource and upload bytes match. Peak process RSS, which
includes startup and driver allocations, is 195.6 → 241.8 MiB in this campaign;
no total-process memory reduction is claimed. The composition scratch counter
now includes both vectors. `transparent_sort_ms` now includes direct batching
and its material/index preparation, so use end-to-end CPU for old/new comparisons.

Two separate forward/reverse inherited P-core user-instruction/cycle controls
show **4.957% and 4.979% fewer retired instructions**, with 99% event running
coverage. Counts include helpers, driver threads, startup, warm-up, assembly and
presentation; they are not isolated render-phase counts or clock-normalized
speedups. Both controls preserve the same non-timing work columns, except the
documented pipeline lookup counters and CPU plan capacity.

Fresh `cpu-clock:u` profiles each cover two complete Hard routes at 499 Hz with
16 KiB DWARF user stacks. Actual render-call windows exclude assembly, warm-up
and presentation; caller, Kin workers and other threads are separate. Combined
draw planning/lookup falls **23.29 → 14.11%** of caller samples (ordering itself
18.35 → 13.87%, lookup 4.94 → 0.24%). Caller samples number 7,292/6,667, with
under 0.7% unresolved phase attribution in each. These are normalized user-CPU
shares, not phase medians. Remaining caller shares are local-shadow preparation
18.49%, camera/instance preparation 18.31%, shadow recording 17.20% and light grid
15.96%. The span interpreter and auxiliary visible-ref filtering account for
most remaining draw-planning work.

Eleven focused NVIDIA suites and four llvmpipe suites pass. The draw-command
regression exercises hidden incompatible rows, camera changes exposing them,
world decal rounds and overlay transparency with equal-depth winners. Existing
coverage includes texture/sampler edits, live poses, source moves, raw spans,
MSAA/HDR/TAA, surfaces, budgets and multiple views in one GPU frame. All **10,080
before/after route-frame comparisons** preserve every non-timing work/resource/
upload/cache column except documented pipeline-lookup counts and CPU plan bytes.
Nine decoded-RGBA pairs at route phases 1/541/961 across all tiers are exact.
Evidence, commands, frozen hashes, raw percentiles, instruction controls and
fresh profiles are under `out/runs/render3d-draw-span-ranges/`.

### Resident spans in live-culling metadata, 2026-11-11

The reference is `862a9892`. Existing compiled one-copy resident runs now skip
unchanged refs when assigning transformed-bound scratch. Local-shadow flat-row
prefixes and caster flags are built together, deriving complete runs in bulk;
overlay rows remain in the flat mapping with no local-caster membership. When
all wanted shadow views hit their selection caches, direct runs also bypass
live-piece enumeration. Any fresh view keeps the complete caster-piece path.
Visibility arithmetic, source/copy order, cache admission, memory capacities and
all GPU work are unchanged; no new cache or per-instance payload is added.

Five alternating Hard pairs show **6.6% median paired caller-CPU p50 reduction**,
range **4.8–19.0%**, improving in all five pairs. Process CPU falls **5.2%** by the
same calculation. Conditions are original independent Yard submissions, 720p
HDR, no AA/GI/AO, classic 1,024 spot maps, four helpers, affinity 0–15, and 300
warm-up plus a full 1,440-frame route per run. Blender/background workloads stay
untouched; clocks, thermals and load are uncontrolled: results are **contended**.
Neither PMU counters nor image readback run during normal timing.

| Tier | Pairs | Caller CPU before p50/p95/p99, ms | After, ms | Paired p50 reduction |
| --- | ---: | --- | --- | ---: |
| Easy | 3 | 0.349 / 0.495 / 0.564 | 0.333 / 0.544 / 0.577 | 4.5% |
| Medium | 1 | 2.027 / 2.404 / 2.650 | 1.608 / 1.848 / 2.000 | 20.7% |
| Hard | 5 | 3.860 / 4.555 / 4.913 | 3.481 / 4.231 / 4.668 | 6.6% |

Percentiles are medians of per-run percentiles; paired reductions use adjacent
run medians, so dividing independently aggregated p50s gives a different result.
The first Easy guard regressed 4.9%; two added alternating pairs improved
11.6%/4.5%. All three are retained, and tails remain mixed. Medium is one guard
pair, not a repeatability/scaling study. Hard frame p50 has a 0.9% median paired
increase (median run medians 11.142 → 12.024 ms); no frame-throughput gain or new
cross-engine ranking is established. These gains do not add directly to earlier
percentages from different measurement windows.

Two separate forward/reverse inherited P-core user-instruction/cycle controls
show **3.062% and 3.060% fewer retired instructions**, at 99% event running
coverage. They cover the whole process, including helpers, driver threads,
startup, warm-up, assembly and presentation, not isolated render-phase counts
or a clock-normalized speedup. Every non-timing per-frame work column matches.

Fresh before/after `cpu-clock:u` profiles each cover two complete Hard routes at
499 Hz with 16 KiB DWARF user stacks. Actual per-frame render-call windows exclude
assembly, warm-up and presentation; caller and Kin workers are separate. Local
shadow preparation falls **26.24 → 19.08%** of caller samples. Camera/instance
preparation is **14.87 → 15.30%** of the smaller total; combined **41.11 → 34.38%**.
These normalized user-CPU shares are not phase medians. Caller samples number
6,772/6,237 with under 0.8% unresolved in each. Normal timings above use the fresh
paired controls; the baseline profile's geometry reference is a previous normal
route of the same frozen binary.

Ten focused NVIDIA suites and four llvmpipe suites pass. Regressions cover
mixed caster/non-caster resident runs, one fresh plus one cached map, completely
visible/hidden frames, and 512 retained overlay refs with unchanged main-scene
shadow membership/cache reuse. All **12,960 before/after route-frame comparisons**
preserve every non-timing work/upload/resource/cache column, and nine decoded-RGBA
pairs across the three tiers at phases 1/541/961 are exact. Evidence, raw paired
percentiles, commands, frozen hashes and profiles are under
`out/runs/render3d-live-cull-spans/`.

### Resident prefixes and retained-batch slot hints, 2026-11-11

The reference is `0eacb145`. Complete one-copy retained runs now feed their
visibility bytes directly into draw counts, prefixes and retained indices;
unused piece-result rows are neither written nor consumed. Their source order,
visibility arithmetic and counters are unchanged. Multi-copy tails, groups and
LOD/cascade consumers keep the general path. Explicit batch refs also keep their
previous table slot as a hint, validated against the current buffer, batch ID
and colour after collection/eviction. A claimed table with bounds bypasses a
second pool scan; colour changes retain the original shared-bounds transfer.
These changes add no cache payload, GPU work or quality-setting changes.

Five alternating Hard pairs show **7.8% median paired caller-CPU p50 reduction**,
range **2.9–15.4%**; all five pairs improve. Process CPU falls **8.3%** by the same
calculation. Blender and all background workloads stay running: these results
are **contended**, with uncontrolled clocks, thermals and host load. Conditions
are original independent Yard submissions, 720p HDR, no AA/GI/AO, classic 1,024
spot maps, four helpers and affinity 0–15; each run has 300 warm-up and a full
1,440-frame route. Neither PMU counters nor image readback run during timing.

| Tier | Pairs | Caller CPU before p50/p95/p99, ms | After, ms | Paired p50 reduction |
| --- | ---: | --- | --- | ---: |
| Easy | 1 | 0.560 / 0.753 / 0.794 | 0.544 / 0.716 / 0.766 | 2.7% |
| Medium | 1 | 2.126 / 2.568 / 2.820 | 2.072 / 2.595 / 2.866 | 2.5% |
| Hard | 5 | 5.161 / 6.188 / 6.972 | 4.919 / 5.933 / 6.529 | 7.8% |

The Hard percentile columns are medians of per-run percentiles; paired reductions
compare adjacent run medians, so dividing independently aggregated p50s gives a
different value. Easy/Medium are single guard pairs and do not establish
repeatability or better tails. Hard frame p50 has a **0.2% median paired increase**
(median run medians 16.317 → 16.588 ms), with noisy tails. No frame-throughput
gain is established, and these absolute timings do not refresh an engine ranking
or add directly to earlier optimization percentages.

A prefix-only screening had essentially flat caller timings (+0.2% median
paired, only two of five pairs improving). The final timing result is for the
combined change; it does not establish a separate speedup for either component.
Raw preliminary controls remain in `out/runs/render3d-resident-prefix/`.

Separate forward/reverse controls compare baseline, prefix-only and combined
binaries using inherited P-core user-instruction/cycle counters on affinity
0–15. Combined retired instructions fall **52.227 → 48.432 billion** and
**52.230 → 48.447 billion**, reductions of **7.27% and 7.24%** (7.3% median).
The prefix-only controls retire 6.6% fewer instructions, supporting reduced work
despite their flat elapsed CPU results. PMU running coverage is 99% in each
control. These are whole-process counts including helpers, driver threads,
startup, warm-up, assembly and presentation, not isolated rendering-phase
counts or a clock-normalized speedup. Their per-frame work columns also match.

Ten focused NVIDIA checks and four llvmpipe checks pass, including a 513-copy draw whose last piece
must not be treated as an independent ref, and a shared batch alternating colour
tables without duplicating/rebuilding its bounds. All **10,080 before/after
route-frame comparisons** preserve every non-timing work/upload/resource/cache
column, and nine decoded-RGBA pairs across Easy/Medium/Hard at route phases
1/541/961 are exact. Final artifacts, commands, binary hashes and the measured
source patch are in `out/runs/render3d-batch-slot-hints/`.

### Stable culling/retention ref updates, 2026-11-11

The reference is `033b9b43`, including the shadow and culling redesigns and the
surface-shadow compatibility correction. The new path compiles live-update and
direct-retention ref indices with the existing command layout. Exact command
revisions avoid resetting/rebinding every ref and rechecking every material's
retention eligibility. Current camera planes still refresh; batch, skin and
spatial-group refs keep live updates. Culling/shadow enablement changes refresh
static flags. Direct-source tables are validated after explicit batch claims,
with rebind/clear on eviction, slot/bound-storage changes or retention fallback.
Prepared consumers and material/command revisions take the full refresh path.
Optional index payload counts within the existing 16 MiB layout limit and is
discarded before rejecting a layout for memory pressure. GPU work is unchanged.

Five alternating Hard pairs, each 300 warm-up plus a full 1,440-frame route,
show **14.0% median paired caller-CPU reduction**, range **1.8–26.1%**; all five
pairs improve. Process CPU, including helpers, falls **11.8%** by the same
paired-median calculation. These are **contended** observations: Blender and all
external workloads remain running, with uncontrolled clocks, thermals and load.
The workload is original independent Yard submissions, 720p HDR, no AA/GI/AO,
classic 1,024 spot shadows, four helpers and affinity 0–15.

| Tier | Pairs | Caller CPU before p50/p95/p99, ms | After, ms | Paired p50 reduction |
| --- | ---: | --- | --- | ---: |
| Easy | 1 | 0.698 / 0.887 / 1.040 | 0.680 / 0.918 / 0.988 | 2.6% |
| Medium | 1 | 2.802 / 3.267 / 3.620 | 2.417 / 2.914 / 3.153 | 13.8% |
| Hard | 5 | 5.870 / 7.091 / 7.745 | 5.390 / 6.633 / 6.990 | 14.0% |

The Hard percentile columns are medians of per-run percentiles. Paired reductions
use adjacent before/after run medians, so they differ from dividing the table's
independently aggregated p50s. Easy/Medium are guard pairs, not repeatability
studies. Hard frame p50 is essentially flat (median run medians 11.998 →
12.001 ms), with noisy tails; no frame-throughput improvement is established.

Separate before/after `cpu-clock:u` profiles cover two full Hard routes each at
499 Hz with 16 KiB DWARF stacks. Actual per-frame render-call windows exclude
assembly, warm-up and presentation. Direct retention falls from **6.76%** of
caller samples to **zero samples** after the change; this is below sampling
resolution, not zero cost. Camera culling/instance preparation is **20.05 →
17.53%**, and their combined share with retained-batch updates is **30.96 →
22.58%**. These are normalized user-CPU sample shares, not exact phase timings.
There are 7,428/6,502 caller samples, with under 0.5% unresolved in each profile.
The remaining culling and batch-update shares are 17.53% and 5.04%; live bounds,
visibility, packing and uploads still execute. Local shadow preparation is the
largest remaining caller category at 26.78%.

Ten focused NVIDIA checks and four llvmpipe checks pass. New regressions compare
warm metadata against forced fresh layouts across camera/cull/shadow toggles,
then interleave prepared consumers with native retention. Existing coverage
includes material revisions, live batches/poses/groups, mutable spans, moves,
same-frame consumers and memory/table pressure. All **10,080 before/after route
frame comparisons** preserve every recorded non-timing work/upload/resource/cache
column; nine decoded-RGBA pairs across all tiers and phases 1/541/961 are exact.
Artifacts, hashes, commands, per-pair timings and separate profiles are under
`out/runs/render3d-ref-refresh/`.

### Shadow recording redesign, 2026-11-11

The reference is `6a1a5d61`, with both preceding shadow-preparation and culling
redesigns active. Native stock shadow draws now keep the projection and addressing
mode bound per map, carrying each direct/list offset in the draw's first-instance
argument. The shaders, draw order, geometry, rasterization, cache ownership and
memory budgets are unchanged. Surface shaders retain their original per-draw
uniforms and instance IDs; GPU cluster-indirect arguments keep their layout.
Sun recording also resolves its eight stock pipeline variants per call, and
ordinary shadow materials bypass surface-shader resolution.

Separate link-instrumented executables measure caller CPU from depth-only pass
begin through end, including SDL/driver recording. Three alternating full Hard
routes produce recording p50 pairs **0.928 → 0.626**, **0.948 → 0.588** and
**0.934 → 0.683 ms**: **32.5% median paired reduction**, range **26.9–38.0%**.
All retain 4,712 indexed draws/frame while vertex-uniform pushes fall
**4,712 → 8**, or **99.83%**. The probe adds counter/timer overhead, particularly
to the reference uniform calls; it is absent from normal timing binaries.

Five uninstrumented alternating Hard pairs show **6.8% median paired caller-CPU
reduction** and **6.0% process-CPU reduction**. Caller reductions are **−30.4%,
+8.7%, −3.4%, +6.9%, +6.8%**: only three pairs improve. Pooled caller p50/p95/p99
are **6.649/8.213/8.870 → 6.402/7.899/8.556 ms**. Blender and other workloads stay
running; clocks, thermals and host load are uncontrolled. Continuous GPU telemetry
is retained. These are contended observations, with no stable frame-throughput
claim. Easy/Medium have one guard pair each, not a scaling study.

Ten focused NVIDIA GPU checks and two llvmpipe checks pass, including independent
shadow-coverage unions across stock, masked, skinned, clustered, instanced and
offset-surface casters in both source orders. All 10,080 normal paired route
frames preserve work, uploads, resources and cache counters; nine separate
decoded-RGBA pairs across all tiers and phases 1/541/961 are exact. Artifacts,
raw per-run percentiles, probe sources and reproduction commands are in
`out/runs/render3d-shadow-recording-redesign/`.

The new fixture also exposed an existing local-shadow interning issue: stock
and offset-surface casters sharing a mesh can merge because the depth-state key
omits surface shader/parameter state. A shared-mesh probe fails identically on
the reconstructed `6a1a5d61` reference and candidate. This recording change leaves
that preparation issue separate; its initial surface-transition regression used
a distinct mesh to isolate recording behavior.

#### Shadow batch compatibility correction

The subsequent fix keeps each depth-affecting surface draw separate instead of
using the compact stock key to infer equality of shader parameters, maps or
per-draw instance IDs. Stock cutouts with nonidentity UV transforms also retain
their own draws because the stock key omits those uniforms. Ordinary opaque and
identity-UV stock cutout batching keep the existing key and compiled-plan path;
the map-scoped recording redesign remains active.

The regression now uses the same mesh for stock and surface casters. Independent
object-map coverage unions verify source-order reversals, different parameters
on one offset shader, a pure cutout surface sampling different non-base maps,
and stock cutout UV transforms. A larger fixture forces retained full-window
shadow-plan reuse, compares against explicit uncached shadow production, and
repeats after a material edit while checking that ordinary opaque draws still
merge. The original shared-mesh fixture fails before the fix. Seven focused
NVIDIA checks and two llvmpipe checks pass. One ordinary-material full-route
pair per tier preserves every recorded work/upload/resource/cache counter over
4,320 paired frames; three separate decoded-RGBA pairs are exact. These are
correctness controls, with no new performance estimate. Evidence is in
`out/runs/render3d-shadow-batch-state-fix/`.

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

#### Receiver-normal history investigation, 2026-11-04

Private overlays from post-port `8921ffb` isolate the remaining Cornell gap.
The old resolve depth-tests a bilinearly filtered GI-history sample without
checking the surface normal. Repeated jittered reprojection mixes lighting
across differently oriented surfaces, creating bright wall/block-edge bands.
Per-tap depth rejection alone barely changes the image. Requiring all four
previous full-resolution normals in each half-resolution history tap to agree
with the current receiver (`dot > .98`) removes most of those bands. Accepted
taps are renormalized and retain the original .85 history blend.

| Fresh 720p Cornell variant | Room SSIM | Room MAE | Frame p50 | Run p50 range |
| --- | ---: | ---: | ---: | ---: |
| Post-port baseline | 0.93377 | 7.514 | 1.054 ms | 0.860–1.157 ms |
| Per-tap depth only | 0.93380 | 7.521 | 1.059 ms | 0.867–1.067 ms |
| History disabled | 0.96674 | 4.524 | 1.052 ms | 0.888–1.059 ms |
| Per-tap depth and normals | 0.96949 | 4.380 | 1.085 ms | 0.884–1.088 ms |
| + Subvoxel emitter handling | 0.97000 | 4.290 | 1.069 ms | 0.945–1.148 ms |
| + Nearby ordinary-surface planes | 0.97053 | 3.927 | 1.117 ms | 1.031–1.217 ms |
| Complete zero-source-point prototype | 0.97280 | 3.263 | 1.415 ms | 1.371–1.535 ms |

The normal check reduces room MAE **41.7%**, closing **73.7%** of the MAE gap
to the full zero-point prototype. It reuses TAA's previous-normal target with
one extra sampler binding and shader reads/math, but no added allocation, ray
or dispatch. All generic overlays retain native emission and main's ratio
irradiance estimator; the emitter rows form a cumulative ladder on the normal
check. They do not use authored geometry tables. The full prototype still does,
and its remaining advantage cannot be attributed to one change by this study.

Quality uses the same geometry, source radiance, settings, two settled phases
and two denoised Cycles seeds as the preceding study. Three fresh serial timing
repeats per variant use 1,200 warm-up and 2,400 measured whole-frame intervals.
Five additional adjacent baseline/fix pairs alternate order: their median
within-pair difference is **+0.0276 ms / +2.6%**, with individual differences
from **−1.3% to +10.4%**. Mains remains connected, but GPU temperatures reach
84–89°C and graphics/memory clocks vary substantially under the observed 80 W
cap. These are exploratory costs; small differences and emitter-row ordering
do not establish precise overhead or speed gains. No build, scoring, capture
readback, profiler or other test application overlaps throughput measurement.

Fourteen raw GPU history cases pass. Matching history yields 2.69922 for the
known 2.7 blend; wrong-normal history returns current 1 instead of the old
resolve's incorrect 2.69922. Old-resolve and history-disabled negative controls
fail their intended cases. Twelve public renderer lifecycle cases pass,
including cuts, resets, resize, toggles, multiview and moving smooth/thin
receivers. Two 64-frame stationary phases show room RMS output variation
0.4755 → 0.4450; the flat upper wall is unchanged (0.6787 → 0.6789). Disabling
history increases variation. These are functional/stationary checks, not
moving-camera reference scores or broad scene qualification.

The receiver-normal history check is the strongest next generic port candidate.
This investigation changes no production renderer source or defaults. Report,
eight-way image comparison, 45 quality/cost jobs, six stationary clips, source/
binary receipts and archive/payload isolation checks are retained under
`out/gi-remaining-quality-20261104/`.

The receiver-normal history candidate is now implemented alongside the earlier
visibility/reset fixes. Resolve binds the existing previous TAA normal target,
requires its valid history, and performs the evaluated per-tap depth/normal
rejection. All 128 embedded shader payloads match the tested normal candidate
exactly, and both fresh Cornell phases match its RGBA output byte for byte.
The new `kin-render3d-gi-surface-history` CTest includes the fourteen raw history
contracts. It, the existing GI resolve/history eligibility checks, and the CPU
renderer CTest pass; all seventeen selected stock GI/TAA/prepass GPU callbacks
pass in isolated processes. Validation is `out/gi-surface-port-20261104/`.

#### Full zero-point remainder, 2026-11-04

The history fixes are merged into local main at `4055cd2`. A reverse-ablation
study rebuilds the complete zero-source-point prototype on that same archive:
all 129 shader payloads and both settled Cornell captures match the frozen
prototype exactly. The ray budget stays 4,096 × 128 per render; zero refers to
explicit source integration, not probe rays. Except for the main-only DC check
and aggregate runtime variants, each row below removes one component from full.
Effects are conditional and cannot be added together.

| Fresh 720p variant | Room SSIM | Room MAE | Frame p50 | Run p50 range |
| --- | ---: | ---: | ---: | ---: |
| Merged main | 0.96949 | 4.3800 | 1.187 ms | 0.896–1.197 ms |
| Full zero-point prototype | 0.97280 | 3.2626 | 1.570 ms | 1.215–1.634 ms |
| Full, no guided rays | 0.97052 | 3.4620 | 1.580 ms | 1.312–1.604 ms |
| Full, no angular/DC correction | 0.97255 | 3.4838 | 1.395 ms | 1.268–1.403 ms |
| Full, no exact footprint caches | 0.97277 | 3.3059 | 1.563 ms | 1.420–1.577 ms |
| Full transport, main one-pass resolve | 0.97275 | 3.2811 | 1.566 ms | 1.430–1.615 ms |
| Runtime geometry, uniform rays, main resolve | 0.97112 | 3.5670 | 1.451 ms | 1.392–1.548 ms |
| Main plus angular/DC correction alone | 0.97021 | 4.2682 | 1.250 ms | 1.249–1.375 ms |

Guidance is the largest measured stability contribution. Full uses 96 ordinary
and 32 emitter-bound rays; removing guidance uses 128 ordinary rays, including
their distance/classification samples. Across two 64-frame stationary clips,
room RMS output deviation rises 0.1396 → 0.3182 without guidance, upper-wall
deviation 0.1481 → 0.4134, and wall 8×8 block-mean deviation 0.0607 → 0.3127.
Rotations have a smaller effect within this guided chain: room deviation rises
to 0.1453 without them, while MAE rises only to 3.2728. These are output sRGB
deviations including TAA jitter/feedback, not motion qualification or pure GI noise.

Angular/DC correction lowers full-chain pixel error but adds measurable work.
Five extra adjacent full/no-DC pairs alternate order: removing correction lowers
frame cost by a median **11.9%**, with **6.0–15.9%** individual reductions, while
MAE increases **6.8%**. Main-only correction now improves error **2.6%** after
the receiver-history fix, unlike the earlier pre-fix experiment. Guided/MIS
radiance must retain compatible integration: blindly restoring an unweighted
ratio denominator gives MAE 13.6068. With uniform rays, ratio and 4/N integration
instead score essentially identically (MAE 3.46207/3.46203).

Exact footprint caches account for only **1.3%** of full-chain MAE in this scene.
Filtering absolute versus normalized gradients and feedback-confidence changes
have tiny settled-Cornell effects; the .98/.9999 history threshold images match.
Removing the extra zero-source composite changes MAE about **0.6%**, with no
resolved frame-time win in the drifting matrix. Extending ordinary single-face
planes unconditionally is rejected (MAE 9.2572). The runtime uniform/one-pass
aggregate uses no authored footprint/emission tables or source bounds and closes
about **73%** of the remaining main/full MAE gap, but remains slower than main.
Runtime emitter bounds with proper invalidation are the next useful guiding
integration target; the full prototype currently freezes its bound to this scene.

A private reuse optimization preserves angular correction: normalize the sample
directions and compute angular retention once before the eight-probe loop; read
each probe's three mean-light metadata channels once for all lighting directions.
Only resolve/feedback shader payloads change. Both Cornell phases remain **RGBA
byte-identical** to full. Five further adjacent full/optimized pairs alternate
order and show **8.0% median lower frame cost**, with **6.9–10.2%** reductions in
all five pairs. This is a useful performance candidate for the experimental
angular path; it is not merged into production by this investigation.

Quality keeps the same fixture, settings, two settled phases and two Cycles
seeds, without exposure fitting. Native matrix timings use three serial varied-
order repeats, 1,200 warm-up + 2,400 measured whole-frame intervals after a
separate 10,000-frame conditioning run. Mains remains connected under the
observed 80 W cap, but clocks and thermals drift; small matrix differences are
not precise speed claims. No build, profiler, capture readback or scorer overlaps
throughput. Four Nsight captures return API events but no GPU timing table after
the offscreen harness exceeds its 8,192 timestamps/frame limit. CPU marker
durations are not substituted for GPU pass times; exact pass costs remain unknown.

The report, 99 accepted quality/cost jobs, six stationary clips, paired controls,
eighteen archive/payload isolation checks and source/binary receipts are retained
under `out/gi-zero-remainder-20261104/`. This follow-up changes no production
renderer source or defaults. It is one static scene/backend/GPU investigation.

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
