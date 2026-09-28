# ECS Systems

Kin provides a small native system layer over Flecs through `kin::EcsWorld`.
Flecs remains the query/execution backend; Kin owns stable ids, phase/order
metadata, scheduling controls, profiling, and inspection snapshots.

## Registering Native Systems

Use `world.systems().register_native<...>()` for C++ systems that should be
visible to Kin tooling.

```cpp
world.systems().register_native<Position, const Velocity>({
    .id = "game.move",
    .name = "Move",
    .phase = kin::SystemPhase::Update,
    .order = 10,
    .reads = {"Velocity"},
    .writes = {"Position"},
    .execution_policy = kin::SystemExecutionPolicy::MainThreadOnly,
    .run = [](kin::EcsEntity entity,
              Position& position,
              const Velocity& velocity,
              kin::SystemContext& ctx) {
        position.x += velocity.x * ctx.dt;
        position.y += velocity.y * ctx.dt;
    },
});
```

Component constness matters and is validated for native systems. Mutable
component parameters are write access; `const` parameters are read access. Empty
tag components should be registered as read-only terms, for example
`register_native<const MyTag>()`.

Native validation rules:

- each mutable typed component must appear in `writes`
- each `const` typed component must appear in `reads`
- a `const` typed component must not appear in `writes`
- extra declared reads/writes are allowed for external resources or future
  non-query access

System `reads` and `writes` are still component names. They are validated against
typed native system parameters today and can later be cross-checked against the
`EcsComponentRegistry` metadata described in `docs/ecs_components.md`.

## Registering Lua Systems

Script scenes can register Kin task systems backed by cached query plans:

```lua
function on_load(scene)
  scene:register_system("lua.damage", {
    phase = "update",
    all = {"Health"},
    reads = {"Health"},
    writes = {"Health"}
  }, function(entity, dt)
    local health = entity:get("Health")
    entity:patch("Health", { hp = health.hp - 1 })
  end)
end
```

Lua systems are registered as `SystemKind::Script`, scene-owned systems. They
run through `world.run_frame()` / Kin scheduling and can read/write components
through `world.components()` metadata. Script reload/destruction removes
scene-owned script systems before the Lua state is released.

## Running Systems

`EcsWorld::progress(dt)` is unchanged and still calls raw Flecs progress.
Kin-facing systems should be run through:

```cpp
world.run_system("game.move", dt);              // one system by id
world.run_phase(kin::SystemPhase::Update, dt);  // one Kin phase
world.run_frame(dt);                            // all Kin phases in order
```

`run_frame()` executes phases in this order:

```text
input
pre_update
update
post_update
pre_physics
physics
post_physics
animation
audio
pre_render
render
post_render
editor
```

Within a phase, systems are sorted by `order`, registration sequence, then id.
Scheduled phase/frame runs use the graph scheduler described below. Manual
`run_system()` bypasses graph batching and runs one enabled system directly.

## Scheduling Fields

V1 supports two lightweight scheduling gates:

```cpp
world.systems().register_native<Marker>({
    .id = "game.periodic",
    .phase = kin::SystemPhase::Update,
    .interval_seconds = 0.25f,
    .rate = 2,
    .run = [](kin::EcsEntity, Marker&, kin::SystemContext&) {},
});
```

Rules:

- `rate = 1` means every scheduled phase tick.
- `rate = N` runs on every Nth scheduled phase tick, starting with the first.
- `interval_seconds = 0` means no interval gate.
- `interval_seconds > 0` accumulates scheduled `dt` and runs once when the
  accumulator reaches the interval; leftover time is carried forward.
- When both `rate` and `interval_seconds` are set, both gates must be due.
- `run_phase()` and `run_frame()` apply rate/interval gates.
- `run_system()` is a manual/editor/debug run and bypasses rate/interval gates,
  but still skips disabled systems.

Invalid descriptors are rejected:

- empty `id`
- missing native `run` callback
- duplicate `id`
- `interval_seconds < 0`
- `rate < 1`
- native typed access that is missing from, or contradicted by, `reads`/`writes`

## Graph Scheduling

Kin builds a dependency graph from system metadata before scheduled execution.
The first implementation executes batches serially, but the graph shape is the
same shape future threaded execution will use.

```cpp
world.systems().rebuild_schedule();
const kin::SystemScheduleSnapshot& schedule = world.systems().schedule_snapshot();
```

`run_phase()` and `run_frame()` rebuild the active schedule after filtering out
disabled and not-due rate/interval systems. `rebuild_schedule()` builds an
inspection schedule for all enabled systems without consuming rate/interval
state.

`run_frame()` is serial by default. New code can opt into eligible parallel
batches explicitly:

```cpp
world.systems().run_frame(dt, kin::SystemExecutionMode::ParallelBatches);
```

`ParallelBatches` runs independent Kin task systems concurrently when the whole
batch is marked parallel eligible. Eligible native Flecs-backed systems may also
partition their matched entities across Flecs workers when adaptive thresholds
are met. Batches still execute in deterministic graph order.

Conflict rules:

- read/read systems can share a batch.
- write/read, read/write, and write/write create dependency edges.
- explicit `before` / `after` entries create dependency edges.
- `order` is not a global barrier; independent systems with different order
  values can share a batch.
- when an edge is needed, deterministic order is phase, `order`, registration
  sequence, then id.

Systems with no `reads` and no `writes` are treated as unsafe for future
parallel execution. They remain valid, but the graph isolates them in their own
serial position and reports a schedule diagnostic.

Unknown explicit dependency ids make the schedule invalid. Cycles make the
schedule invalid. Scheduled execution fails without running systems when the
active schedule is invalid.

Snapshots expose graph state:

```cpp
struct SystemScheduleSnapshot {
    bool valid;
    kin::SystemExecutionMode requested_execution_mode;
    kin::SystemExecutionMode effective_execution_mode;
    std::vector<std::string> diagnostics;
    std::vector<SystemDependencyEdge> edges;
    std::vector<SystemBatch> batches;
};
```

`SystemBatch` and `SystemSnapshot` include parallel eligibility flags. A batch
is marked parallel eligible only when every system in that batch is explicitly
eligible and the graph did not isolate it for safety.

Systems default to `SystemExecutionPolicy::MainThreadOnly`. To mark a system as
eligible for future threaded execution:

```cpp
world.systems().register_task({
    .id = "game.read_ai",
    .reads = {"AIState"},
    .execution_policy = kin::SystemExecutionPolicy::ParallelEligible,
}, [](kin::SystemContext&) {});
```

Parallel-eligible systems must satisfy the thread-safety contract:

- `reads` and `writes` must name registered Kin components.
- relation pair access must be declared in `relations`; use
  `kin::read_relation("targets")` or `kin::write_relation("targets")` for the
  common cases.
- `SystemStructuralMutationPolicy::RawWorldAllowed` is serial-only and rejected for
  parallel-eligible systems.
- structural mutation should use `SystemStructuralMutationPolicy::CommandBufferOnly` and
  `ctx.commands`.

Parallel eligibility is execution metadata for task systems and native
Flecs-backed systems. A system is not reported as parallel eligible when it is
main-thread-only, has no read/write metadata, has explicit `before`/`after`
dependencies, declares `mutation_notes`, or allows raw world mutation.

Relation declarations participate in graph conflicts. Read/read relation access
can batch; write/read, read/write, and write/write relation access creates a
dependency edge just like component write conflicts.

`SystemSnapshot` includes `batch_index`, `parallel_eligible`, `execution_policy`,
`structural_mutation_policy`, `mutation_notes`, relation access metadata, last execution
mode, last batch execution mode, command-buffer stats, and per-system schedule
diagnostics for the last built schedule.
Parallel rejection reasons are available both as human-readable strings and as
structured `{system, code, message}` diagnostics.

## Controls And Inspection

Systems can be controlled by stable id:

```cpp
world.systems().disable("game.move");
world.systems().enable("game.move");
world.systems().remove("game.move");
```

System add/remove/enable/disable and runtime errors emit retained ECS events
through `world.events()`.

Snapshots expose editor-facing state:

```cpp
std::vector<kin::SystemSnapshot> systems = world.systems().snapshots();
kin::SystemSnapshot move = world.systems().snapshot("game.move");
```

Each snapshot includes id, name, kind, enabled state, phase, order, interval,
rate, read/write metadata, schedule diagnostics, batch index, owner scope,
source file, last error, matched entity count, run count, last duration, average
duration, and max duration.

When a `ProfileSession` is active, Kin records system timings as:

```text
<id>    category: ecs.system
```

## Threading

Kin supports opt-in concurrent execution for eligible Kin task systems and
eligible native Flecs-backed systems inside a single dependency batch. The
scheduler still runs phases and batches in deterministic order, and
`run_frame(dt)` remains serial unless
`SystemExecutionMode::ParallelBatches` is requested.

Kin uses a hybrid backend for parallel batches:

- Kin owns graph construction, execution-mode selection, worker orchestration,
  profiling aggregation, and editor diagnostics.
- Flecs provides per-worker stages and read-only multithreaded world mode for
  native system execution. Native systems are partitioned across their matched
  entities with Flecs workers; independent native systems in the same graph batch
  still start in deterministic system order.
- Kin command buffers provide deterministic structural mutation for task
  systems and are flushed after each batch.

Systems receive a deferred ECS command buffer through `SystemContext`:

```cpp
world.systems().register_task({
    .id = "game.spawn",
    .writes = {"Enemy"},
    .execution_policy = kin::SystemExecutionPolicy::ParallelEligible,
}, [](kin::SystemContext& ctx) {
    kin::EcsDeferredEntity enemy = ctx.commands->create_entity("enemy");
    ctx.commands->set<Enemy>(enemy, Enemy{});
});
```

`ctx.commands` supports typed `create_entity`, `destroy`, `add<T>`,
`remove<T>`, `set<T>`, and `modified<T>` commands. Commands can target existing
entities by `EcsEntity` or `EcsId`, or entities created earlier in the same
buffer by `EcsDeferredEntity`.

Flush rules:

- manual `run_system()` flushes after the system succeeds.
- scheduled `run_phase()` and `run_frame()` flush after each graph batch.
- parallel batches give each worker an isolated Kin command buffer and flush
  successful buffers on the main thread in deterministic system order.
- native Flecs systems in parallel batches execute on per-worker Flecs stages;
  Flecs staged commands merge before Kin command buffers flush. `ctx.commands`
  is intentionally `nullptr` for native callbacks while they are partitioned
  across Flecs workers.
- a failed system's command buffer is discarded.

Command buffer snapshots report cumulative queued, flushed, discarded, and failed
command counts per system. The last schedule snapshot also reports per-batch
command counts, execution mode, execution decision, worker count, estimated work,
decision reason, and duration.

Parallel execution is adaptive under `SystemExecutionMode::ParallelBatches`.
Safety metadata decides whether a system or batch is allowed to run in parallel;
`SystemExecutionTuning` decides whether it is worth doing right now:

```cpp
world.systems().set_execution_tuning({
    .min_parallel_systems = 2,
    .min_native_entities = 1024, // tune DOWN for games below ~1k entities per query
    .max_workers = 0,            // 0 = hardware_concurrency()
});
```

All tuning gates are pure functions of deterministic inputs (entity counts and
system counts) on purpose. A duration-based gate used to exist and was removed:
measured timings flip between runs, and a system that runs serial in one run
and staged-parallel in another merges its deferred ops in a different order,
which reorders flecs table rows and silently breaks run-vs-run determinism.

Task-only batches choose `ParallelAcrossSystems` when enough eligible systems
are present. Native Flecs systems choose `ParallelWithinNativeSystem` when the
matched entity count clears `min_native_entities`. Mixed native/task batches run
in deterministic system order in V1; native systems may still choose Flecs
workers, while task systems in that mixed batch are reported as serial.

Eligibility gotchas (see `parallel_eligibility_reasons` / the schedule report
for per-system diagnostics):

- An explicit `before`/`after` list disqualifies a system from parallel
  eligibility. Sequencing usually survives without it: `order` plus the
  conflict edges derived from declared reads/writes already order systems that
  touch the same components.
- The default `min_native_entities` (1024) silently serializes everything in
  games with fewer matched entities per query — check the `exec.*` rows in the
  F1 debug overlay (or the schedule report) to confirm systems actually run
  parallel rather than assuming they do.
- For native systems the access metadata is derived from the template
  signature: `const T` parameters become reads, mutable `T` parameters become
  writes. The `reads`/`writes` string lists only need entries for accesses the
  callback performs outside its parameters (e.g. tags toggled via
  `entity.add<T>()`). Declaring a const template component as a write is
  rejected. Registration failures are logged at ERROR level.

### Structural safety: never cache component pointers

Raw `T*`/`T&` obtained from a query or `get_mut()` dangle as soon as any entity
in the same table changes archetype (tag add/remove, destroy): flecs
swap-removes rows and may reallocate columns, so the stale pointer reads
relocated memory. Single-threaded this often "works" deterministically and then
becomes a heisenbug the moment allocation timing varies. When collecting
entities in one pass and mutating in a second, store `kin::EcsEntity` and
re-resolve with `get_mut()` per iteration, or hold a `kin::EcsRef<T>` (a
flecs-ref-backed handle that re-resolves the component's current location on
every access).

Use `ctx.running_parallel` to detect when a task or native system is running
inside a parallel batch. Direct `ctx.world->raw()` mutation is a serial-only
escape hatch; parallel-eligible systems should use `ctx.commands` for Kin-owned
structural changes in task systems or Flecs staged commands from native
callbacks. Field mutation is only safe for components declared in `writes`;
unrestricted world mutation is not supported for threaded batches.

Use `mutation_notes` to document known structural mutations or raw world access.
Systems with mutation notes remain serial-only until they are migrated to the
command buffer or a future Flecs staging path is implemented for that mutation
pattern.

## Example

See `games/ecs_systems_demo` for a small game that registers movement, bounds,
and color systems through `kin::ecs` and runs them with `world.run_frame(ctx.dt)`.
See `games/ecs_graph_demo` for a scheduler-focused sample that runs
`ParallelBatches`, uses `ctx.commands` from task systems, and reports graph
batches, dependency edges, and parallel diagnostics in its headless run report.
See `games/ecs_parallel_bench` for a headless timing comparison between
`SerialGraph` and `ParallelBatches` across costly task systems and native
Flecs-backed systems. It reports both speedups separately because task systems
parallelize across independent systems, while native Flecs systems currently
parallelize within each system's matched entity set.
