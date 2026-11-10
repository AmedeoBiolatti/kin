# Area-light BVH empty-bin fix, 2026-11-08

Branch `fix/gi-bvh-empty-bins` starts from fetched current main `66972da` with default Exact-area lighting.
The production change is two lines: `Box::grow(Box)` ignores an empty input. Empty SAH bins previously expanded
prefix/suffix bounds to +/-FLT_MAX, making costs infinite and suppressing useful spatial splits. Triangle
geometry, lighting formulas, budgets and all 131 shader payloads remain unchanged. No previous GI prototype is
included.

A mesh/instance sparse-cluster regression fails twice before the fix and passes after it. Existing geometry,
emitter and transform tests pass. An independent float64 plane/Gram oracle agrees with a packed BVH walk for
8192 closest rays. The synthetic triangle-test count falls from 554902 to 75098 (86.5% less work), while node
visits change 351892→342400 and stored nodes increase 171→339. These are traversal counts, not frame-time gains.

| Case | Main median ms | Fixed median ms | Median paired time change | Median paired speedup |
|---|---:|---:|---:|---:|
| Cornell | 1.1624 | 1.1412 | -2.41% | 1.02× |
| Bistro day | 3.6700 | 3.7285 | +2.03% | 0.98× |
| Bistro night | 4.3519 | 3.8765 | +0.82% | 0.99× |
| Bistro + area panel (stress) | 229.3479 | 105.1519 | -54.10% | 2.18× |

Times are application intervals, not GPU timestamps: three alternating uncontended repetitions with process,
clock/temperature monitoring. Clocks are unlocked and full run-median ranges are retained. Effect estimates use matched within-repeat ratios,
not a ratio of aggregate medians; thermal drift is visible in the inactive controls. Normal cases use
1200 warmup +1200 measured frames. Area stress uses 64+128, a short steady-work diagnostic. Small differences on
the inactive controls do not establish a broad speedup.

The normal Bistro day/night static fixtures have active GI but no eligible explicit area emitter, so the
changed hierarchy is inactive. Both are pixel-identical in 16 paired images. The separate stress adds one
untextured emissive panel to the frozen Bistro geometry (six area pieces, one GI emitter, 972463 occluder
triangles). It exercises the expensive path; it is not normal Bistro gameplay or an engine-ranking result.
The harness fixes camera/placements/exposure and omits HUD, simulation and auto-exposure.

Cornell's two-reference error/SSIM and 64-frame variation are essentially unchanged at 2400/4800 renders. Across
72 paired images, mean full-image RGB difference is 0.000549 levels and maximum difference is 2. The stress has
mean difference 0.017913 and maximum 25 in eight images at age 256. Triangle order changes floating-point work and
the existing four-interval shadow approximation; no independent stress truth qualifies the direction of its
small shadow changes. The original 72 frozen main PNGs still match exactly.

Four targeted CPU/GI tests pass on NVIDIA and software Vulkan. Real Khronos core/synchronization validation is
verified in the three GPU cases; the geometry suite is CPU-only. No validation errors were observed. All 24
measured windows have zero allocation/release counter changes. Recorded managed payload rises by 544 bytes
in Cornell and 10.07 MiB (0.63%) in the area-panel stress, and is unchanged in ordinary Bistro.

Keep this bounded fix: it restores useful spatial splits, with a clear stress-case gain and a small local
Cornell gain. It does not establish a general GI quality or normal Bistro performance improvement.

The [artifact report](../out/gi-bvh-empty-bins-20261108/README.md) provides the full distributions, images,
reference scores, commands, fixture scope, source/build hashes, dependencies, telemetry and replay scripts.
Local artifacts are ignored by Git and verified by `seal.py --verify`.
