# Compiled resource graphs

`kin/renderer/resource_graph.hpp` is a backend-independent compiler. `kin/renderer/graph_resources.hpp` binds its resources through public renderer APIs. Both are usable without experimental 3D. The stock native 3D renderer uses the same compiler for its coarse phase schedule; ordinary games still call `Renderer3D::render()`.

Graph compilation is separate from shader compilation, GPU pipeline creation and temporal/asset caches.

A definition contains named logical resources and passes. A resource has one writer; reads depend on that writer regardless of declaration order. Imported values are initialized and read-only. Write another logical version for a result, and import last frame's history separately from this frame's output. Reading and writing the same logical resource in a pass is rejected: feedback needs ping-pong resources.

```cpp
ResourceGraphDesc graph;
graph.resources = {
    {.name = "source", .kind = GraphResourceKind::ColorTarget,
     .size = {640, 360}, .imported = true},
    {.name = "result", .kind = GraphResourceKind::ColorTarget,
     .size = {640, 360}, .exported = true},
};
graph.passes = {{.name = "filter", .reads = {0}, .writes = {1},
                 .required_features = graph_feature::raster | graph_feature::shaders}};

ResourceGraphCompiler compiler; // keep across frames
GraphResources resources;      // keep with this renderer
const auto& plan = compiler.compile(graph, available_graph_features(renderer));
const std::array imports{GraphImport{.resource = 0, .texture = source}};
if (!plan || !resources.prepare(renderer, graph, plan, imports))
    return; // inspect plan.diagnostics or resources.diagnostics()

const std::array<std::function<bool()>, 1> operations{{[&] {
    auto target = renderer.scoped_render_target(resources.target(1));
    renderer.clear(colors::black);
    renderer.draw_shader_surface({0, 0, 640, 360}, shader, params, resources.texture(0));
    return true;
}}};
if (execute_resource_graph(plan, operations))
    renderer.draw_texture(resources.texture(1), {0, 0, 640, 360});
```

Declare every access accurately. Callbacks use the existing public raster/compute/3D operations and supply their own parameters and bindings. Compilation validates the declarations; it cannot inspect arbitrary C++ callback code or insert synchronization inside an opaque operation. A callback failure stops subsequent operations and does not roll back commands already recorded.

The compiler diagnoses duplicate names, invalid IDs, multiple writers, missing initialization, feedback, missing/disabled dependencies, cycles and unavailable required features before callbacks. `after` adds explicit dependencies, including between side effects. Exports and side-effect passes are roots for unused-pass culling. Unsupported work that is unused may be culled. `GraphCompileOptions{.cull_unused=false, .reuse_slots=false}` provides the reference schedule/resource plan.

The optional `max_slot_payload_bytes` compile limit rejects a graph before allocation if its managed slots exceed the requested payload budget. Imports, backend-owned tokens and hidden driver storage are outside this limit; callers choose an explicit fallback.

The plan contains a stable topological order, first/last use intervals, compatible physical slots, planned payload and culling counts. Exported and persistent resources receive pinned slots. Transient slots can share SDL resource objects only after the previous value's last use. This is object reuse, not overlapping native heap allocations. SDL may allocate hidden cycle backings; use the device ledger and external telemetry when assessing memory.

`GraphResources::prepare()` validates the exact definition/plan and imported dimensions/formats, then stages allocations before publishing new bindings. Failure keeps the previous bindings. Keep resources on one renderer and use handles created by that renderer. Imports retain their supplied handles. Exports are current-frame textures/buffers, not immutable snapshots across later executions. Release a pipeline with `clear()` when its retained capacity is no longer useful.

Public colour targets use the renderer's working format. Set `GraphResourceDesc::hdr` to match `renderer.hdr()`; HDR target payload is RGBA16F. A working-colour-space change recreates managed targets. Storage textures use the declared `TextureFormat`; buffers use `bytes`. Tokens describe dependencies between backend-owned operations and have no graph-managed allocation. Depth/MSAA/array resources and fine-grained stock allocation migration remain future extensions; a token is not an allocatable depth texture.

Stable plans update imported bindings without allocating new CPU/GPU storage. Exact descriptor/feature/compile-option comparison caches the plan; camera, time, callback parameters and imported contents do not invalidate it. Resize or topology/feature changes do. Use a compiler/resource set per retained pipeline variant to avoid replacing one cached variant with another every frame. `compiler.stats()` reports compilation count, hits and CPU compilation time.

Build and run `kin_ascii3d` for the public-API 3D → ASCII → output example. Its GPU shader samples one scene colour per cell and selects compact glyph strokes. `kin_ascii3d --test` compares optimized/reference pixels, verifies target reuse, tests HDR declarations and checks stable allocations. It currently requires native custom shader support; capability checks reject unsupported backends before execution. The rendering path performs no CPU readback.

The legacy 2D `RenderGraph` caches its dependency order through this compiler. Missing dependencies and cycles now throw an actionable error before any callback, instead of silently ignoring a dependency or recursing indefinitely. Toggle behaviour and pass statistics remain available.

The native 3D migration currently exposes thirteen backend operations as dependency tokens. Their existing internal depth, shadow, GI, temporal and output work remains atomic and owns its resources. `Scene3D::debug.graph_reference` runs the original fixed order for parity testing. `RenderDebugStats3D` reports pass counts, cache reuse and CPU compilation/recording time. Stock transient allocation aliasing, independently replaceable internal geometry/shadow nodes and a general finite-propagation simulation are still separate work in the pipeline plan.
