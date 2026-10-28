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

## Stage 4: stock operations and quality calibration

The native stock renderer now schedules 13 coarse operations through the same public compiler, with cached plans per view and CPU compilation/recording counters. Their backend-owned resources are represented by dependency tokens; internal pre-pass, main-pass, GI/history allocation and synchronization remain within those operations. Fine-grained resource migration and stock transient aliasing remain later work. `debug.graph_reference` executes the original fixed order for comparison. A two-view HDR/TAA/AO/contact-shadow/sun-shadow/picking regression matches the fixed order exactly. The complete native renderer suite passes.

The reference tool accepts explicit filtering, mip-bias and specular-AA candidates and records effective settings, graph counters and memory payload. `quality_matrix.py` records source, binary and content identities, alternates candidate order, preserves logs and optionally compares with Cycles. Candidate version 1 is not an engine preset. Ground truth now supports independent seeds and defaults to no denoising, adaptive sampling or sample clamping. Moving-camera calibration and the active lighting work's final GI snapshot are still required before selecting new defaults.

Imported `KHR_materials_unlit` now ignores PBR emission fallback data. Native unlit emission retains legacy glow and supports independent RGB; both generic and specialized paths are checked. In-place emissive texture updates invalidate GI attributes without changing the texture handle.

The static incremental path also covers model parts, including multiple parts in a shared material bucket and shared prefab renderers. Bindings retain part indices rather than duplicate part matrices. Asset/model/material/particle-texture resolvers publish changed runtime bindings, so native reload helpers invalidate extraction and validation caches. Regression coverage includes these resolver changes.

ASCII checks use explicit runtime validation in Release builds, including optimized/reference equality, actual target handle reuse and HDR working-format transitions. The example uses stack operation/import tables and cached binding storage to avoid steady preparation allocations.

The graph and memory examples pass with Release checks enabled on hardware/software Vulkan. WebGL2 has also been rebuilt and its smoke fixture passes after graph integration. Quality results and calibration limits are recorded in [rendering measurements](rendering_measurements.md).

## Stage 5: standalone geometry in public graphs

`Renderer3D::render_geometry` records opaque/cutout world geometry into caller-owned sampled D32F depth and signed octahedral RG16F world normals. The new `RenderAttachment` graph resource kind allocates explicit float/depth textures with exact payload accounting, compatible lifetime slots and colour-space-independent reuse. Extreme RGBA16F sizes are rejected before payload arithmetic can wrap. The compiler remains usable without experimental 3D.

The native stock prepass now shares its geometry recorder with this operation and uses a cached typed geometry subgraph for its pinned view-owned outputs. The outer thirteen-phase schedule and stock TAA normal/motion packing remain intact. The operation has an exact unjittered camera, independent clearing and validation for formats, extents, attachment usage, device ownership and attachment feedback. It retains ordinary culling, skinning, retained batches and CPU/GPU LOD cuts while leaving stock view history untouched.

[kin_outline3d](../tools/outline3d/main.cpp) renders a live depth/normal outline pipeline entirely through public APIs, with a shader owned by the example. Its `--test` path checks nonempty surfaces/edges, exact optimized/reference pixels, stable handles, cached plans, stable GPU handle allocation and zero device-idle waits. [The geometry contract](render_graph.md#geometry-operations-and-data-attachments) documents normal decoding, reverse-Z depth, ownership and supported coverage.

This is a first reusable geometry operation. The standalone path still prepares/culls its submission on each call. It excludes blended/wireframe/overlay/particle geometry and normal-map/material-fragment effects; it has no occlusion history or MSAA. Shared prepared submissions, replaceable shadow/main operations and full stock transient allocation migration remain subsequent work. WebGL2/SDL_Renderer reject this operation explicitly.

Validation: the complete native renderer dispatch passes; ten focused hardware suites pass, with the separate live-foreign-device lifecycle probe skipped as documented; all six selected software Vulkan suites pass, including that ownership probe. The WebGL2 smoke fixture passes with explicit unsupported-geometry checks. The generic graph compiler builds and passes without experimental 3D, and the 190-file 3D formatting check passes. Four saved stock captures match decoded RGBA bytes exactly. The [measurement notes](rendering_measurements.md#separate-device-lifecycle-finding) record the unresolved two-device window-release crash and its 2D-only reproduction.
