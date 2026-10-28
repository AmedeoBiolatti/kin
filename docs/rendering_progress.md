# Rendering implementation progress

Work proceeds on `exp/rendering-foundations`, from committed lighting work `77b76c8`. The active GI subvoxel edits and main's planning changes are separate. See the [pipeline plan](rendering_pipeline_plan.md) for scope and acceptance gates.

## Stage 1: correctness and material fidelity

- `50c5e32`: conservative affine bounds across visibility, spatial groups, retained caches and LOD. Native image comparisons cover shear, mirrors, shadows, picking, skinning, retained instances and CPU/GPU clusters.
- `5dcdfed`: seam-aware flat-soup LOD, attribute simplification and versioned bakes, retaining the uniform-attribute memory path.
- Independent linear RGB emission, explicit legacy conversion, and matching native/browser/custom-shader/GI semantics. Strength-only GI changes retain voxel attributes; zero emission changes emitter membership.
- Independent per-map texture samplers and image sharing by colour/data role, including material-file inheritance/clearing, cutout shadows, picking and appearance caches.
- One camera-depth composition order for native transparent mesh copies and sprites. Only consecutive compatible packed records batch together. Additive light behind alpha foregrounds is attenuated correctly. Explicit particle `sort=false` retains its approximate whole-source ordering.
- Conservative cluster storage reservations: draws unable to reserve a complete finest cut render their full level-zero geometry in all dependent passes, with nonblocking CPU fallback counters. This may be substantially more expensive than an actual coarse cut. It is a safety policy, not a performance improvement; precise per-object GPU overflow fallback remains a candidate to measure.
- Flat reconstructed normals preserve a usable UV normal-map frame.

### Validation

Focused native fidelity tests pass on RTX 4080 Laptop Vulkan and software Vulkan. They cover gamma/linear composition, cross-material interleaving, smoke on both sides of glass, additive light behind alpha, RGB emission independent of blue base/vertex/instance colours, HDR strength, GI colour and intensity reuse, zero-factor membership, sampler binding changes on one shared image, 108 imported min/mag/wrap combinations, and tiny/exact-fit cluster budgets. Material asset sharing, variants and reload tests pass. CPU bounds and mesh-cluster suites pass, including the new flat-seam regression.

The browser build uses CI's Emscripten 6.0.13. The WebGL2 3D smoke test passes in Chromium/SwiftShader, including materials, skinning, sky, fog, shadows, resizing and input. It is functional coverage, not a browser performance measurement.

The native renderer dispatch has passed in partitioned runs, including the large cut, temporal, GI, animation, particle, cache and compressed-import cases. The final material fixes also pass the focused hardware/software fidelity suites and unlit specialization comparison. Large cut-parity fixtures now explicitly reserve enough room to exercise their cut paths; separate pressure tests verify complete fallback geometry, cascades, two views and occlusion without debug readback. A short existing GPU/CPU timing assertion was context-sensitive: the same case passed alone (~0.30 ms GPU versus ~0.38 ms CPU) and failed after the wider suite (~0.49 ms versus ~0.38 ms). Functional coverage now checks bounded per-copy upload work as well as image/count parity; timings remain reported. Performance conclusions belong in isolated repeated benchmarks under the plan's protocol.

Local native configuration: Release, GCC 15.2, experimental 3D, scene runtime, Jolt, scripting and image I/O; audio, TTF and 2D physics disabled. A build-only `-Wno-error=template-body` on scripting targets handles an existing sol2/GCC 15 diagnostic; repository and dependency sources were not altered for it.

## Stage 2 baseline

Before incremental updates, a CPU-only flat workload with 10,000 and 100,000 entities measured four transform flushes plus collection per frame, with static and 1-percent-edit cases. Five runs used 50 warm-up and 150 measured frames each. At 100,000 entities, p50 was about 10 ms for both cases. Builds were active on the machine, so this is a diagnostic baseline, not a controlled end-to-end speed claim. Retain the original binary for alternating before/after measurements and add hierarchy, tracked-write and raw-write correctness gates.

Stages 2 (incremental CPU work and memory accounting), 3 (compiled graph and ASCII) and 4 (stock migration and quality calibration) remain in progress. Do not interpret proposed APIs or quality defaults in the plan as implemented.

Imported `KHR_materials_unlit` now ignores PBR emission fallback data. Native unlit emission retains legacy glow and supports independent RGB; both generic and specialized paths are checked. In-place emissive texture updates invalidate GI attributes without changing the texture handle.
