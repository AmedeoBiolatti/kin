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

The complete native renderer dispatch now passes in one process after graph integration, following earlier partitioned runs, including the large cut, temporal, GI, animation, particle, cache and compressed-import cases. The final material fixes also pass the focused hardware/software fidelity suites and unlit specialization comparison. Large cut-parity fixtures now explicitly reserve enough room to exercise their cut paths; separate pressure tests verify complete fallback geometry, cascades, two views and occlusion without debug readback. A short existing GPU/CPU timing assertion was context-sensitive: the same case passed alone (~0.30 ms GPU versus ~0.38 ms CPU) and failed after the wider suite (~0.49 ms versus ~0.38 ms). Functional coverage now checks bounded per-copy upload work as well as image/count parity; timings remain reported. Performance conclusions belong in isolated repeated benchmarks under the plan's protocol.

Local native configuration: Release, GCC 15.2, experimental 3D, scene runtime, Jolt, scripting and image I/O; audio, TTF and 2D physics disabled. A build-only `-Wno-error=template-body` on scripting targets handles an existing sol2/GCC 15 diagnostic; repository and dependency sources were not altered for it.

## Stage 2 baseline

Before incremental updates, a CPU-only flat workload with 10,000 and 100,000 entities measured four transform flushes plus collection per frame, with static and 1-percent-edit cases. Five runs used 50 warm-up and 150 measured frames each. At 100,000 entities, p50 was about 10 ms for both cases. Builds were active on the machine, so this is a diagnostic baseline, not a controlled end-to-end speed claim. Retain the original binary for alternating before/after measurements and add hierarchy, tracked-write and raw-write correctness gates.

## Stage 2: tracked CPU work and memory accounting

`WorldRenderState3D` supports explicit `SceneMutationMode3D::Tracked`. Use `set<T>()`, `modified<T>()` after mutable writes, or `invalidate()` after external bulk changes. Structural observers include disabled entities and prefab sources. Stable topology/source caches propagate dirty roots and descendants, and static mesh/model extraction reuses retained batches. Sparse placement edits preserve independently collected snapshots. Native physics and root motion publish actual transform changes. `work_stats()` reports visits, updates, fallback collections, validation reuse and estimated metadata capacity.

The default `Conservative` mode preserves raw mutable pointers using the original tight transform loops. An experimental comparison cache regressed that path (~13 ms versus ~10 ms at 100,000 entities), so it was rejected. Animation, lights, particles, attachments and presentation callbacks still use full extraction; structural/material changes rebuild the static cache. This is an initial incremental path, not a claim that all ECS scans are removed.

Particle configuration validation is cached only in tracked mode, including failures and disabled emitter capacity. CPU regression tests cover static zero visits, sparse edits, descendants, raw writes, derived-transform tampering, reparenting, prefab source edits, observer destruction and particle validation invalidation.

`RendererBackendStats::gpu_memory` accounts for every Kin-owned SDL_GPU texture, buffer and transfer handle. It calculates format-block payload, mip chains, layers/depth and samples, and reports idle pooled textures separately, plus peaks and allocation/release/failure counts. These are descriptor payloads; SDL cycle backings, pending GPU retirement, alignment and driver allocations remain opaque. Buffers now share the device lifetime guard used by textures. Hardware and software Vulkan tests pass descriptor calculations, allocation/release accounting, 100 pool reuses, 100 target resizes and destruction after the renderer.

`kin_scene_work_bench` emits CPU timings with actual scan/update counters. Final before/after results are recorded below once validated.

## Stage 3: compiled graph and GPU ASCII

The backend-independent `ResourceGraphCompiler` validates single writers, initialization, feedback, dependencies, cycles and required features before execution. It provides stable scheduling, unused-pass culling, resource lifetimes, compatible allocation slots, export/history pins and exact descriptor caching. Reference options disable culling and reuse. `GraphResources` allocates public colour targets, storage textures and buffers transactionally and validates imports. Reuse concerns SDL objects; it does not imply arbitrary native heap aliasing.

`kin_ascii3d --test` passes on hardware and software Vulkan: optimized/reference pictures match exactly, four logical targets use three compatible physical slots, repeated frames reuse plans and allocate no new resource handles. The live tool is an example using public APIs and a GPU glyph shader with no CPU readback in its rendering path. Native custom shader support is required; WebGL2 capability rejection is explicit.

The existing 2D `RenderGraph` also uses compiled scheduling and diagnoses cycles/missing dependencies before callbacks. CPU graph and existing rendering tests pass.

The ASCII executable uses explicit Release runtime checks, stack operation/import tables and cached binding storage. Tests cover HDR working-format validation and real slot reuse, in addition to image parity and allocation stability. Graph-managed payload budgets reject oversized plans before allocation.
