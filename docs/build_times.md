# Faster development builds

The earlier measurement rounds, through dependency scheduling, used the original
benchmarking branch before its build changes reached `main`. Their commit IDs
refer to that history. The workflow callback follow-up starts from merged `main`
at `66972da`. Each round records its scope and base; remeasure absolute times
and ratios for your checkout and machine.

Use a persistent build directory and build the game you are editing:

```sh
cmake --preset dev
cmake --build --preset dev --target hex_demo
```

`dev-3d` enables the experimental 3D engine. Both presets use Release
optimization, four build jobs, stable precompiled headers (PCH), and optional
compiler-cache discovery. They omit examples and benchmarks; tests remain
available as explicit targets. Choose the game target for the edit/build/run
loop instead of rebuilding all games and tests. Use the shipping presets for
packages and the existing CI presets for full validation.

PCHs occupy additional disk space and take time to create on the first build.
They help subsequent source edits by avoiding repeated header processing.
Engine PCHs exclude Kin's own headers, so engine API edits do not invalidate
them. Game PCHs include the common engine runtime API, which stays stable
while editing game sources; they exclude game-owned headers. Each target gets
its own PCH with its own compiler settings; a game's product/content
definitions do not leak into another game. Small engine adapters and third-party libraries keep ordinary
compilation. Sources with individual compiler options also keep ordinary
compilation, as their settings may not match their target's PCH.

For a game that adds Kin with `add_subdirectory` or `FetchContent`, configure
its existing build directory with:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DKIN_ENABLE_PCH=ON -DKIN_COMPILER_CACHE=auto
cmake --build build --parallel 4 --target my_game
```

`kin_game(my_game ...)` applies the game settings. A target that does not call
`kin_game` can call `kin_configure_game_build(my_game)` after linking
`kin::engine`. An existing target PCH is preserved. To diagnose a compiler/PCH
problem, reconfigure with `-DKIN_ENABLE_PCH=OFF`; headers are still included
normally by the sources. CMake's `DISABLE_PRECOMPILE_HEADERS` and
`SKIP_PRECOMPILE_HEADERS` properties also work for individual targets/sources.

## Compiler caches

Install `ccache` or `sccache` separately, then set `KIN_COMPILER_CACHE`:

Use Ninja (including on Windows) or a Makefile generator for CMake's compiler
launcher support.

| Value | Behavior |
|---|---|
| `off` (default outside the development presets) | Leave compiler launchers alone |
| `auto` | Use ccache, then sccache, if found |
| `ccache` / `sccache` | Require that tool |

Explicit CMake compiler launchers take precedence. Kin's CI already configures
ccache through the launcher environment variables. Auto-discovery is not saved
as a launcher cache entry, so switching `KIN_COMPILER_CACHE` back to `off`
removes launchers it selected.

A compiler cache helps when returning to previously compiled code, including
branch switches and rebuilds. It cannot avoid compiling genuinely new code,
and it does not cache the final link. Keep a persistent cache on a fast disk
and check its hit/miss statistics rather than assuming it is working.

For **ccache with GCC/Clang PCH**, ccache requires the following configuration:

```sh
export CCACHE_SLOPPINESS=pch_defines,time_macros
cmake --preset dev
cmake --build --preset dev --target hex_demo
ccache --show-stats
```

This relaxes ccache's checks of macros inside the precompiled header, including
time macros. Kin does not set it globally. Avoid time-dependent macros and
source-local configuration macros for PCH headers. If this tradeoff is not
suitable, use `KIN_ENABLE_PCH=OFF` with the cache, or PCH without caching.
See the [ccache PCH requirements](https://ccache.dev/manual/latest.html#_precompiled_headers).

## Linux linking

After installing [mold](https://github.com/rui314/mold), a Linux development
build can select it through CMake's normal executable linker flags:

```sh
cmake --preset dev -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=mold
cmake --build --preset dev --target hex_demo
```

This does not change C++ optimization. Keep LTO off for the edit loop, as the
development presets do. The default linker remains available by configuring
with `-DCMAKE_EXE_LINKER_FLAGS=` (retain any other linker flags you need).

## Linux compiler choice

With Clang 18+ installed, `dev-clang` and `dev-3d-clang` select `clang` and
`clang++` in separate build directories. They retain Release optimization,
PCH and the same optional compiler-cache discovery:

```sh
cmake --preset dev-clang
cmake --build --preset dev-clang --target hex_demo
```

The optional mold linker setting above works with these presets too. For an
external game, select the compiler when configuring a fresh build directory:

```sh
CC=clang CXX=clang++ cmake -S . -B build-clang -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DKIN_ENABLE_PCH=ON -DKIN_COMPILER_CACHE=auto
```

Clang reduced compiler memory substantially in the local comparison below,
but was slightly slower for the large Lua-binding translation unit. Compiler
choice remains explicit; the original development/shipping presets retain
their compiler selection.

## Faster Lua binding builds (opt-in)

`dev-clang-fast` trades some Lua binding runtime speed for less compiler
optimization work:

```sh
cmake --preset dev-clang-fast
cmake --build --preset dev-clang-fast --target hex_demo
```

`dev-fast` uses the system compiler instead; both presets are available on
Linux. They use separate build directories from the normal development presets.
For an existing GCC/Clang build, keep its compiler and enable
`-DKIN_DEV_FAST_SCRIPTING=ON` at configuration time. Turn it off with `OFF`;
the normal development presets explicitly select `OFF`.

This option appends `-O1` only to `kin_scripting_scenes` in Release and
RelWithDebInfo, including its target-private PCH. Scene, entity and workflow
bindings use it; other engine targets, Lua itself, and game sources retain their
normal settings. Debug and MinSizeRel flags are unchanged. Applying the option
to a shipping build or an unsupported compiler produces a configuration error.
The default is `OFF`.

Changing the option rebuilds the binding target and its PCH once. The mode is
intended for iteration; use normal development or shipping presets to assess
optimized game performance. See the whole-target measurements below for the
observed compilation/runtime tradeoff and its limits.

## Include the interfaces you use

`scene.hpp` declares `SceneContext` with references to App, Input, Window and
Renderer2D; it forward-declares those classes. A source that calls their methods
includes the corresponding header directly. For example, calling
`ctx.input.pressed(...)` needs `<kin/platform/input.hpp>`, and drawing through
`ctx.renderer` needs `<kin/renderer/renderer2d.hpp>`.

`script_engine.hpp` supplies the scripting interface, runtime statistics and
Color. It forward-declares SceneContext and the borrowed service classes. Code
that accesses SceneContext fields includes `<kin/scene/scene.hpp>`; code that
uses App methods includes `<kin/platform/app.hpp>`.

This keeps edits to concrete service headers from rebuilding consumers that
only need scene or scripting declarations. Two separate translation units check
that the public headers compile on their own, with PCH and unity disabled.

Renderer data headers follow the same rule: `ecs/render.hpp`, render commands,
sprite catalogs, lighting, particles and tilemaps declare Renderer2D references
without importing its definition. Code that constructs a Renderer2D or calls its
methods includes `<kin/renderer/renderer2d.hpp>` directly. `Flip` and its helpers
live in `<kin/renderer/flip.hpp>`; backend/renderer headers still expose them.
Normal-lighting declarations include their DataBuffer and Flip value types
directly and forward-declare backend capabilities used by reference. Nine
standalone renderer-header translation units check these include contracts.

## Measure the edit loop

Compare the same compiler, flags, sources, build filesystem and job count.
Separate dependency downloads/configuration from compilation, and distinguish
a cold compiler cache from a warm cache. Useful cases are:

1. A no-change build (should run no compilers).
2. A changed game `.cpp`, including its final link.
3. A changed engine `.cpp`, including archive creation and the game's link.
4. A changed public header and a changed shader include.
5. An engine rebuild with third-party libraries already built.

Repeat incremental cases and compare medians; other processes, memory pressure
and filesystem caches can change elapsed time. More jobs are not always
faster. Start with four, then compare two/eight while watching memory use.

For header/template profiling, use Clang's `-ftime-trace` and
[ClangBuildAnalyzer](https://github.com/aras-p/ClangBuildAnalyzer), or GCC's
`-ftime-report`, in a separate build. PCH and unity builds address repeated
frontend work; a faster linker addresses a different part of the wait.

## Local measurements, 2026-11-06

GCC 15.2, Release, Ninja with four jobs, a Linux source worktree and NTFS/FUSE
build output. The compiler cache was disabled for source-edit trials. Medians
of three runs, with existing PCHs and dependencies already built:

| Edit/build | Before | PCH | PCH + mold |
|---|---:|---:|---:|
| `hex_demo` main `.cpp`, including link | 8.36 s | 7.61 s | 4.30 s |
| ECS `system.cpp`, including engine archive and game link | 27.13 s | 17.67 s | not measured |

The engine row uses the normal GNU linker in both columns. Background workloads
were not controlled, so treat these as exploratory local results. These are
incremental timings, not a like-for-like clean-build comparison. PCH creation
costs additional time and disk space on the first build.

In a shader-only fixture using the full 3D shader manifest, changing the GI
sampling include went from 154 shader compiles in 9.39 s to 12 in 0.79 s
(one run each; excludes C++ embedding and linking). Nested include changes and
paths with spaces are covered by regression tests. If SPIR-V bytes are
unchanged, the embedding generator preserves its C++ output's timestamp.

With ccache/PCH enabled, recompiling one unchanged game source took 3.56 s cold
and a median 0.036 s warm (three direct hits). This measures object compilation
only; a changed game's link still takes time.

The dependency setup also initializes the SDL codec/font libraries' PIC default
before creating earlier targets. Previously that default arrived late in the
cache and caused SDL/Lua/Box2D recompilation on the next configure. A fresh
configure/reconfigure check now leaves all 975 compile commands unchanged.

The [sample timings and configuration](build_times_2026-11-06.json) retain the
individual runs and measurement limits. Validation included eight selected
CTest tests, GCC/Clang parent-game fixtures, Ninja/Make shader dependency tests,
and representative GCC 3D renderer/Lua compile checks. Windows and a full 3D
runtime test suite were not run locally.

### Compiler comparison

GCC 15.2 and Clang 21.1.8, both with Release optimization and their own PCHs;
compiler cache disabled. These are compile-only medians of three runs, with
compiler order alternating between runs:

| Translation unit | GCC | Clang | GCC peak RSS | Clang peak RSS |
|---|---:|---:|---:|---:|
| Hex demo main | 2.28 s | 1.61 s | 311 MiB | 189 MiB |
| ECS system | 9.01 s | 7.29 s | 484 MiB | 272 MiB |
| Scene asset/registration | 61.64 s | 53.98 s | 2,185 MiB | 920 MiB |
| Lua bindings | 38.85 s | 40.70 s | 1,583 MiB | 882 MiB |

A separately paired game edit/build/link comparison with mold on both sides
gave 2.59 s for GCC and 1.91 s for Clang. These runs had different host load
from the earlier measurements; do not multiply improvements across phases.
The compiler memory savings were 39–58% in these four files. This can matter
on a machine where parallel compilations compete with games/editors for RAM.

These timings compare the installed toolchains: explicit Kin Release settings
match, while implicit compiler defaults differ. In particular, this machine's
GCC driver injects hardening and padding-initialization flags that Clang does
not inject. The data records that limitation; this is not a universal compiler
ranking or an isolated comparison of optimizer implementations.

The complete selected Clang build succeeded, with 11/11 CTest tests passing
(also 11/11 for GCC). Clang 3D checks compiled the renderer, GI and Lua scene
translation units with their corresponding PCHs. Runtime CPU spot checks used
three processes per compiler, 50 samples after three warmups, pinned to CPU 2:

| Runtime case | GCC median | Clang median |
|---|---:|---:|
| ECS query, 10k entities | 1.744 ms | 1.794 ms |
| Prefab spawn/destroy | 0.0604 ms | 0.0630 ms |
| CPU collection of 100k sprites | 6.702 ms | 3.956 ms |

Totals matched between compilers. These narrow checks do not establish
whole-game performance or full 3D runtime behavior. The original compiler
presets remain available. Individual samples and validation scope are in the
[compiler comparison data](build_compilers_2026-11-06.json).

### Scene registration split

`scene_asset.cpp` now contains parsing, serialization and scene instantiation.
`scene_components.cpp` dispatches the built-in registrations in their original
order to six smaller files: rendering/audio, physics, particles/tilemaps,
gameplay, lighting, and view/UI. Physics registration is compiled only when
enabled. Component IDs, schemas and registration callbacks retain their
existing behavior. Games using `kin::engine` pick up the change automatically.

The following local measurements use the same Release/PCH settings as above,
with compiler caching disabled. The original file was compiled once per
compiler; the split measurements are medians of three runs:

| Compilation workload | GCC before | GCC after | Clang before | Clang after |
|---|---:|---:|---:|---:|
| Parser source edit, compile only | 63.51 s | 12.80 s | 54.01 s | 9.24 s |
| All scene files, synthetic four-job batch | 63.51 s | 31.10 s | 54.01 s | 34.63 s |
| Largest per-process peak RSS across these files | 2,186 MiB | 841 MiB | 919 MiB | 438 MiB |

The first row excludes rebuilding the registrations after a parser-only edit.
The second rebuilds all eight resulting files with four workers, compared
with the original single file. It excludes PCH creation, archive/link time
and the rest of the engine; it is not a full clean-build measurement. Other
builds were active on this machine, and the baseline has only one fresh sample
per compiler, so treat the ratios as estimates.

Splitting trades total work for shorter individual compilation tasks. Summed
compiler CPU time for all scene files rose from 63.10 to 108.01 s with GCC,
and from 53.91 to 110.87 s with Clang. Common template work is repeated across
the smaller files. A build with little parallelism, or a clean engine build
already using every core, can therefore get slower. Per-process memory
reductions do not establish the total memory used by a parallel build.

GCC and Clang each passed the nine selected game/runtime CTest tests. A
before/after probe compared all 47 registered components' IDs, ordered field
metadata and default values exactly. The permanent scene test also checks
registration idempotence, ordering, default snapshots and preservation of
caller-defined schemas. All seven scene files compiled with GCC without PCH,
physics, scripting, audio, image I/O or TTF. All eight compiled with Clang's
experimental 3D configuration. These additional checks cover compilation;
Windows and a full 3D runtime suite were not tested.

The [scene split data](build_scene_split_2026-11-07.json) includes individual
samples, queue order, CPU totals and measurement limits.

### Full runtime rebuild

A subsequent comparison rebuilt all Kin libraries needed by `hex_demo`,
including Lua, audio, physics, PCHs and the game's final link, using Ninja
with four jobs. The before and after commits were `031dd64` and `fc41b58`,
in separate worktrees/build directories. Third-party libraries, generated
shaders/embedding and initial CMake configuration were already built.
All 213 common engine/runtime compile commands matched after normalizing
source/build paths. There was no third-party compilation in the timed runs.

| Toolchain | Before split | After split | Observed change |
|---|---:|---:|---:|
| GCC 15.2 | 240.80 s | 248.59 s | 3.2% slower |
| Clang 21.1.8 | 229.45 s | 221.92 s | 3.3% faster |

There is one paired observation per compiler: GCC before/after, then Clang
after/before, run sequentially. These small, mixed differences do not
demonstrate a reliable full-rebuild improvement at four jobs. Host load,
hybrid-core scheduling, filesystem/cache effects and thermals can affect
these timings. The split remains useful for source edits and reduced
per-process memory; the scene-only four-worker result above should not be
used to predict a full engine rebuild.

The largest child-process peak RSS fell from 2,184 to 1,583 MiB with GCC
and from 920 to 882 MiB with Clang. These are not total parallel-build RAM
figures. All four builds succeeded and each resulting game passed its
headless/render smoke test.

The task traces identify `script_engine.cpp` and `entity_scripts.cpp` among
the last compilations before the final link in all four builds. In the GCC
baseline, the scene file finished at 187.82 s, while the game linked at
240.80 s. These Lua compilation tasks and their scheduling were profiled
in the following experiment.

The [full rebuild data](build_full_rebuild_2026-11-07.json) retains the run
order, CPU/RSS measurements, smoke results and complete Ninja task timelines.
This phase adds measurement records without further engine behavior changes.

### Lua binding template work

The Lua binding files now share sol2 invocation adapters by callback
signature. Captured callbacks use `std::function`; noncapturing entity
callbacks use function pointers. Large scene type definitions register their
members individually, keeping heterogeneous member tuples out of sol2's
forwarding layers. Callback bodies, captures, member order and Lua API names
are preserved. Optimization and sol2 safety settings are unchanged; no new
build option is needed.

Clang's [time profiler](https://clang.llvm.org/docs/ClangCommandLineReference.html#cmdoption-clang-ftime-trace)
showed about 60–67% of the original files' compilation time in the backend,
with substantial optimizer/inliner work. PCHs were already enabled. Reducing
the number of adapter instantiations also reduces the generated wrapper code.

The final direct Clang comparison used one core (CPU 2) for both versions,
Release `-O2`, matching PCHs, no compiler cache, and no profiler instrumentation.
There is one fresh before/after observation per file, with order alternating
between files:

| Translation unit | Before | After | Before peak RSS | After peak RSS |
|---|---:|---:|---:|---:|
| `script_engine.cpp` | 44.94 s | 35.18 s | 877 MiB | 743 MiB |
| `entity_scripts.cpp` | 33.57 s | 26.10 s | 795 MiB | 707 MiB |

Both files compiled about 22% faster in this pair. Pinning avoids switching
between this machine's different core types, but does not isolate the SMT
sibling, CPU frequency or background I/O. Treat the ratios as local estimates.

A separate GCC full runtime rebuild pair used four jobs on four distinct
performance cores (CPUs 0, 2, 4, 6), with dependencies/generated assets already
built. The two binding tasks fell from 61.56 to 39.27 s and from 47.35 to
29.36 s. However, the complete `hex_demo` rebuild rose from **366.23 to
404.28 s**. Other projects were compiling concurrently, including on SMT
siblings, and the 168 unchanged engine compilation tasks had a median
19.8% slowdown in the after run. This pair does not establish a clean-build
speedup; the retained change targets binding compilation work. These totals
should not be compared directly with the preceding unpinned session.

All four Lua API reports (scene, entity, test, scenario) matched baseline
exactly on both compilers. GCC and Clang each passed nine selected scripting,
runtime, tooling and game tests. Added checks cover constructors, read-only
properties and forbidden entity construction. A mechanical reconstruction
also confirms the old callback bodies and ordered registration definitions.
Without PCH, both files passed Clang 3D syntax/template checks and GCC checks
with physics/audio disabled. These additional checks cover compilation;
Windows and a full 3D runtime suite were not run.

Runtime spot checks used three processes per configuration, each taking
30 samples after three warmups on CPU 2. Median scene/entity bridge-loop
changes ranged from 1.2% faster to 2.3% slower, with identical resulting
entity positions. These narrow cases do not establish whole-game performance.
All process results, including an outlier, are retained in the
[Lua binding measurement data](build_lua_bindings_2026-11-07.json), alongside
the profiles, paired compile samples, full rebuild timelines and validation.

### UI2 dirty observer compilation

`ecs/ui2.cpp` now uses one non-template helper and one shared entity-only
callback to register dirty observers. The old helper instantiated Flecs
builders and typed callback adapters for every widget type, although the
callback only needed the entity. Passing the component ID keeps the same
query term while sharing those adapters. Layout, widget synchronization,
rendering and public APIs are unchanged; no build option is needed.

The original Clang profile spent 20.32 of 27.77 seconds in the backend,
including substantial optimizer work. Reducing repeated adapters cuts both
template work and generated code. Final measurements used the original
`ae92ca1` implementation and this change, matching Release `-O2` commands,
PCHs enabled, no compiler cache and no profiler flags. Each compilation ran
serially on CPU 2, with three samples per version/compiler and alternating
before/after order. The table reports medians:

| Toolchain | Before | After | Compile time reduction | Before peak RSS | After peak RSS |
|---|---:|---:|---:|---:|---:|
| GCC 15.2 | 36.98 s | 29.85 s | 19.3% | 1,002 MiB | 876 MiB |
| Clang 21.1.8 | 31.18 s | 23.84 s | 23.5% | 503 MiB | 440 MiB |

These are translation-unit results, not a measured clean engine/game build
speedup. CPU pinning does not isolate SMT siblings, thermals or background
I/O. RSS is per compiler process, not total parallel-build memory.

All 111 observer descriptors matched the original exactly, including names,
IDs, events, query terms and access flags. GCC and Clang each passed nine
selected UI2, scripting and game tests. New lifecycle assertions cover dirty
version counts, deferred changes, destruction, OnSet behavior and observer
re-registration; they also pass when linked against the original UI2 object.
Without PCH, the file passed Clang 3D and GCC physics/audio-disabled syntax
and template checks. Windows and full 3D runtime tests were not run.

A warmed event stress test added/removed a label 100,000 times per sample,
with three warmups and 30 measured samples per process. Across three
processes per version on CPU 2, the median rose from 109.977 to 111.653 ms
(1.5% slower); every process verified the exact final dirty version. This
narrow test does not establish whole-game performance or a runtime speedup.
The [UI2 measurement data](build_ui2_2026-11-07.json) retains all compile and
runtime samples, descriptor data, validation details and reproduction scripts.

### End-to-end scheduling and worker count

The engine now enables CMake's
[static-library dependency pruning](https://cmake.org/cmake/help/v4.2/prop_tgt/OPTIMIZE_DEPENDENCIES.html)
when the caller has not explicitly set `CMAKE_OPTIMIZE_DEPENDENCIES`.
Generated GPU shader bytes compile in a small object library included in the
engine archive. This lets CMake retain that small prerequisite instead of the
whole engine archive when building independent runtime libraries. Scripting
targets are declared early, with their source lists and settings attached in
their original location.

This addresses the local Ninja 1.13
[priority heuristic](https://github.com/ninja-build/ninja/blob/v1.13.2/src/build.cc#L432-L436),
which counts remaining actions rather than measured compiler durations.
The old graph prioritized engine objects ahead of expensive Lua compilation.
With eight workers, moving that Lua work earlier shortened the final link wait:

| Eight-worker runtime rebuild | Original graph | New graph |
|---|---:|---:|
| Median wall time, two runs each | 145.22 s | 121.01 s |
| Median aggregate compiler/build CPU time | 933.84 s | 898.01 s |

The observed wall reduction is **16.7%**. Runs followed original/changed/
changed/original order, pinned to CPUs 0, 2, 4, 6, 8, 10, 12, 14 (one thread
per performance core). Other user builds affected the session; the first pair
had nearly equal aggregate CPU time, while the final control consumed more CPU time.
These are local estimates, not an isolated or cross-platform guarantee.

All runs rebuilt the 221 Kin/runtime/game objects and PCHs needed by `hex_demo`,
then archived and linked the game. Dependencies, generated shaders and initial
CMake configuration were prebuilt. Compiler cache and LTO were off; Release
`-O2` remained enabled. Common compiler commands match except for the private
shader-data include directory. The generated data object uses ordinary headers
instead of the engine PCH; its generated C++ bytes are identical.

A separate capacity comparison used the original graph, three runs per count:

| Worker count | Median runtime rebuild |
|---|---:|
| Four performance cores/workers | 215.07 s |
| Eight performance cores/workers | 149.01 s |

On this machine, eight workers reduced that observed median by 30.7%. The
shared development presets retain four jobs. To try eight for a local build:

```sh
cmake --build --preset dev-clang --parallel 8 --target hex_demo
```

The comparison used explicit performance-core affinity; ordinary OS scheduling
can select different cores. Tune worker count for the machine and available
memory. For the scheduling comparison, maximum sampled process-tree RSS rose
from 2,446 to 2,876 MiB as heavy compilations overlapped earlier. Summed RSS
double-counts shared pages, including PCHs; host memory/activity observations
are also retained in the data.

An initial four-worker scheduling trial did not establish a gain: most of the
original build already kept all four workers busy, and other builds slowed the
changed trial. No four-worker speedup is claimed. Repeated no-change and source
edit checks are recorded separately from the complete rebuild comparisons.
After the change, their medians were 0.22 s for no changes, 2.00 s for a game
source edit and 22.14 s for a UI2 source edit, including archive/link work.
Each edit rebuilt exactly one C++ source. These checks verify rebuild scope;
no incremental speedup is attributed to the scheduling change.

GCC and Clang each passed nine selected UI2, scripting and game tests. A real
shader edit rebuilt one shader, one data object, the engine archive and the game
link; restoration reproduced the original generated bytes, and the next build
did no compilation or linking. Additional checks generated/compiled the 3D
shader data, compiled the new object with GCC LTO, configured without optional
subsystems/GPU/PCH, and verified the explicit dependency-optimization override.
Those checks do not cover a full 3D runtime or Windows/MSVC build.

The [scheduling and capacity data](build_scheduling_2026-11-07.json) retains
all runs, Ninja timelines, activity/memory samples, edit-loop results, validation
and reproduction scripts.

### Workflow callback follow-up

On merged `main` at `66972da`, a fresh Clang profile of
`lua_workflows2d.cpp` took 15.19 seconds: 5.87 in the frontend and 9.27 in the
backend. Twelve captured callbacks now use `std::function` with four repeated
signatures, allowing sol2 to share their invocation adapters. Unique signatures
retain their lambdas. Callback bodies, captures, safety settings and optimization
flags are unchanged.

Three uninstrumented pairs per compiler produced these local medians:

| Workflow source compile, `-O2` | Original callbacks | Shared adapters |
|---|---:|---:|
| Clang 21.1.8 | 13.18 s | 12.30 s |
| GCC 15.2.0 | 10.88 s | 10.89 s |

Clang was faster in every pair; the reduction between medians is **6.7%**.
GCC was effectively unchanged. An exploratory version that wrapped all 33
captured callbacks, including unique signatures, barely helped Clang and slowed
GCC, so it was discarded.

Each timed command compiled only this source, pinned to performance-core CPU 2,
with a matching prebuilt PCH, no compiler cache and no LTO. Run order reversed
in the middle pair. Before/after source copies used the same NTFS/FUSE volume
and private-header include path. Other user jobs were active during parts of the
session; these are local estimates. PCH creation, dependencies, shaders, archives
and links are excluded. **This is not a whole-build speedup measurement.**

Lua probes measured binding 50 fresh VMs, 400,000 input queries and 40,000
controller-table calls. Each compiler ran three processes per variant, with
three warmups and 30 timed samples per workload. The retained `-O2` adapters
showed no slowdown in these sampled medians, and all probe assertions passed.
This does not establish whole-game performance or cover every workflow path.

A separate trial compared optimization levels on the changed source, using
separately built, matching PCHs:

| Workflow source compile, shared adapters | `-O2` | `-O1` |
|---|---:|---:|
| Clang 21.1.8 | 11.79 s | 9.99 s |
| GCC 15.2.0 | 11.14 s | 8.99 s |

The observed reductions were **15.3%** and **19.3%**. These are separate runs
from the callback comparison above; compare within each table. With only this
translation unit built at `-O1`, sampled input-query medians rose by 3.8% with
Clang and 3.4% with GCC relative to the changed `-O2` version. Other engine
objects and Lua itself retained their normal optimization. `-O1` remains an
experiment in this single-file round; the whole-target follow-up below evaluates
the opt-in mode separately.

GCC and Clang each passed 11 selected scripting, scene and game tests, and all
four Lua API reports matched the original `main` reports. A mechanical check
also verified that removing the twelve wrappers reproduces the original
callback bodies and captures. Windows/MSVC and a full 3D runtime were not
validated in this round.

The [workflow measurement data](build_workflows_2026-11-07.json) contains the
compile samples and per-process runtime medians, rejected prototype results,
source hashes, validation results, original commands and reproduction scripts.

### Opt-in optimization for the whole binding target

Starting from `7749e3b`, `KIN_DEV_FAST_SCRIPTING` applies `-O1` to all nine
sources in `kin_scripting_scenes` and its PCH. This follows the single-file
experiment above. The setting is private to this target and off by default.

Three pairs per compiler rebuilt that entire target and relinked `hex_demo`:

| Binding target rebuild + game link | Normal `-O2` | Opt-in `-O1` | Reduction |
|---|---:|---:|---:|
| Clang 21.1.8, median | 26.72 s | 22.94 s | 14.1% |
| GCC 15.2.0, median | 27.55 s | 23.70 s | 14.0% |

Every pair was faster with `-O1`. Each build used four workers pinned to CPUs
0, 2, 4 and 6, one thread per performance core, with compiler cache and LTO off.
Each run rebuilt the PCH and nine C++ objects, archived the target and linked
the game: twelve compile/archive/link actions. CMake configuration was untimed;
the normal Ninja glob check remained in the timed build. Dependencies, shaders,
other engine targets and game objects were already built. These percentages
describe a **binding-target rebuild**, not a complete Kin/game build or a game
source edit. Existing includes and source files were identical between modes.

Comparing all 999 compile database entries per compiler found exactly ten
changes: the target's nine sources and its PCH each gained one trailing
optimization flag, `-O1`. The other 989 commands were identical. Both modes used
fresh, matching PCHs. Run order reversed in the middle pair. Other user jobs
were active during parts of the session, and wall times varied; the data
retains all pairs and CPU-time measurements.

Runtime probes linked the same optimized engine/Lua libraries against either
the complete `-O2` or complete `-O1` binding archive. Both compilers ran three
processes per mode and workload group, pinned to CPU 2, with three warmups and
30 timed samples per workload. The table shows the median of those per-process
medians, in milliseconds:

| Workload | Clang `-O2` | Clang `-O1` | GCC `-O2` | GCC `-O1` |
|---|---:|---:|---:|---:|
| Create one scene VM | 0.456 | 0.485 | 0.497 | 0.487 |
| 20,000 scene calls | 4.31 | 4.27 | 4.35 | 4.20 |
| 50,000 entity calls | 24.50 | 24.48 | 25.84 | 24.16 |
| Bind/destroy 50 workflow VMs | 5.35 | 5.20 | 5.57 | 5.32 |
| 400,000 input queries | 23.85 | 24.68 | 23.30 | 23.47 |
| 40,000 controller-table calls | 17.90 | 16.89 | 16.03 | 17.03 |

Sampled medians moved in both directions, with slowdowns up to 6.3% and gains
up to 6.5%. Results vary by workload and overlap run-to-run variation; they do
not establish a general runtime speedup or a game frame-time/FPS result. The
scene/entity probes assert final positions; workflow probes assert input states,
table contents and iteration counts. The mode stays opt-in for development.

Both compilers passed twelve selected tests with the option enabled, including
the scripting, workflow, scene and headless game checks. All four Lua API reports
matched `-O2`. Configuration tests cover private flags, matching PCH compilation,
turning the option off, Release/RelWithDebInfo versus Debug/MinSizeRel, and
shipping/unsupported-compiler rejection. Both new Linux presets configured
successfully in fresh directories using cached dependency sources. The existing
benchmark builds were restored to `OFF` and their optimized game smoke tests
passed. This round does not validate Windows, a full 3D runtime, LTO/PGO or
representative real-game frame times.

The [whole-target data](build_scripting_o1_2026-11-07.json) includes all build
samples and raw runtime timings, command comparisons, validation, source hashes
and reproduction scripts.

### Scene and scripting header dependencies

Starting from `71255e5`, `scene.hpp` forward-declares the services that
SceneContext borrows instead of including App, Input, Window and Renderer2D.
`script_engine.hpp` includes the small Color/types interfaces and forward-declares
SceneContext and its borrowed services. Its consumers no longer inherit the
scene implementation's dependencies. The two implementation files that call App
methods include App directly, and a Lua runtime test now includes Input directly.
Consumers outside this repository may need the same explicit service includes.

The fields, method signatures, method bodies and test assertions are unchanged;
a mechanical comparison verifies that the five existing C++ files differ only
in includes and forward declarations. These measurements use normal Release
optimization, with `KIN_DEV_FAST_SCRIPTING=OFF`, PCH on, cache off and LTO off.

Ninja rebuild plans for `hex_demo` changed as follows. GCC and Clang agree:

| Edited header | Before: C++ steps | After: C++ steps |
|---|---:|---:|
| `platform/app.hpp` | 27 | 18 |
| `platform/input.hpp` | 55 | 51 |
| `platform/window.hpp` | 116 | 115 |
| `renderer/renderer2d.hpp` | 102 | 99 |

Each row includes one game PCH compilation. `shapes_demo` has the same counts.
For `kin_lua`, App edits dropped from 26 to 16 source compilations, with no game
PCH. Plans were obtained from copies of the generated Ninja manifests, avoiding
dry-run termination at CMake's always-checked glob rule. Header edits were
temporary trailing comments; original bytes and timestamps were restored.

App edits no longer rebuild nine engine sources, including entity scripts,
workflow bindings, script reload/runtime, ECS scenes, scene flow/preload and menu
scenes. The App edit still rebuilds the game PCH and its main source, archives
two runtime libraries, and links the game. Three real App-edit builds per
compiler and version produced these local medians:

| App header edit through `hex_demo` link | Before | After |
|---|---:|---:|
| Clang 21.1.8 | 40.36 s | 20.36 s |
| GCC 15.2.0 | 51.34 s | 21.54 s |

The observed wall-time reductions are 49.6% and 58.0%. Each real run's object
set matched its rebuild plan. Builds ran serially with four workers pinned to
CPUs 0, 2, 4 and 6, with other objects, dependencies, shaders and engine PCHs
already built. Compiler order alternated between rounds and reversed for the
changed version. All original-version samples preceded the changed-version
samples; the versions were not interleaved.

Timing ratios are exploratory shared-host estimates. A separate workload drove
load above 140 during the final original GCC run, which took 118.38 seconds;
that sample is retained. The other two original GCC runs took 48.92 and 51.34
seconds. Host activity also varied across the measurement phases. The reduction
from 27 to 18 rebuild steps is the stronger structural result. These are App
header-edit measurements, not complete-build or ordinary game-source-edit gains.

GCC and Clang each passed 18 selected scripting, scene, UI2, development and game
tests; all four Lua API reports matched the baseline. A no-PCH syntax audit
passed for 337 Clang 2D translation units (including the two new header checks),
330 GCC 2D units and 436 Clang 3D units after fixing the test's indirect Input
include. The standalone header checks also compiled with both compilers and
the configured component definitions, with PCH and unity disabled.
This does not constitute Windows/MSVC or full 3D runtime validation.

After timing, the original App header was restored and each measured game's
PCH rebuilt to match its original input bytes/attributes. Game/Lua runtime
smoke tests and API reports passed again, and a subsequent game build performed
no compilation or linking.

The [header dependency data](build_headers_2026-11-07.json) retains the complete
rebuild plans, timing samples, skipped object sets, syntax source lists,
validation, mechanical comparison and reproduction scripts.

### Renderer data header dependencies

Starting from `38439f5`, six data/service headers stop including the full
Renderer2D definition: ECS rendering, render commands, sprite catalogs, lighting,
particle systems and tilemaps. They forward-declare the renderer used by
reference. Render commands include their ClipRegion/Flip types directly;
normal lighting includes DataBuffer/Flip and forward-declares backend
capabilities used by reference. ECS rendering includes draw-trace declarations
directly. Implementations and tests that construct or call Renderer2D include
its definition explicitly.

The existing Flip enum/operators/mirroring helpers moved byte-for-byte from
`backend.hpp` to `flip.hpp`; the backend still includes that header. A mechanical
comparison verifies unchanged fields, signatures, function bodies and assertions
in all 23 existing C++ files touched by the cleanup. Sizes/alignments of eleven
rendering data types and Flip values match before/after with both compilers and
render-probe mode on/off. RenderCommand remains 152 bytes in those checks.

GCC and Clang produce the same `hex_demo` rebuild plans:

| Edited header | Before: C++ steps | After: C++ steps |
|---|---:|---:|
| `renderer/renderer2d.hpp` | 99 | 62 |
| `renderer/backend.hpp` | 101 | 97 |
| `platform/window.hpp` | 115 | 83 |

Each row includes one game PCH. `shapes_demo` has identical counts. Renderer
edits drop from 95 to 60 source compilations for `kin_lua`, without a game PCH.
For the games, 37 fewer source compilations means **37.4% fewer C++ steps**;
runtime archive updates also drop from five to two. The skipped work includes
four Lua binding sources, animation, pathfinding, prefabs, particle simulation,
tilemaps and several component registrations. Entity-script compilation remains
in the renderer dependency set through its UI dependencies.

Plans use the same alternate-manifest dry-run method as the preceding round.
Each completed real renderer-header edit produced exactly its planned object
set. Both versions use normal Release `-O2`, PCH on, compiler cache/LTO off,
four workers pinned to CPUs 0, 2, 4 and 6, and the same prebuilt dependencies,
shaders and unaffected objects. One completed observation per compiler/version
measured the edit through the final game link:

| Renderer header edit through `hex_demo` link | Before | After |
|---|---:|---:|
| Clang 21.1.8 | 126.06 s | 63.10 s |
| GCC 15.2.0 | 284.37 s | 78.69 s |

These are exploratory individual observations, not medians or controlled
speedup ratios. External host load rose above 50 during the original GCC run;
the later runs had much lower contention. An additional original GCC repeat
was interrupted to avoid continuing repeated profiling under that load. Both
completed originals and the interrupted attempt are retained in the data.
Versions were measured in separate phases, not interleaved. The rebuild-count
reduction is the primary result; no percentage wall-time improvement, complete
build improvement or ordinary game-source-edit improvement is claimed here.

Both compilers passed 25 selected rendering, ECS, sprite animation, particle,
tilemap, scripting, scene, UI2, development and game tests. Four Lua API reports
matched the baseline. No-PCH syntax/template checks passed for 346 Clang 2D,
332 GCC 2D and 436 Clang 3D translation units. The initial audit identified 13
consumers needing direct includes; all passed their recheck. Nine standalone
renderer-header units also compiled with each compiler, component definitions,
and PCH/unity disabled. This is not full
Windows/MSVC or 3D/GPU runtime validation.

After timing, the renderer header's original bytes/attributes were restored,
each measured game PCH was rebuilt against that header, rendering/game/Lua
smoke tests and API checks passed again, and a no-change game build compiled
and linked nothing.

The [renderer header data](build_renderer_headers_2026-11-07.json) retains plans,
skipped object sets, individual timing samples, host load, the interrupted
repeat, layout results, source lists, validation and reproduction scripts.

### Render queue backend dependency

Follow-up measured on 2026-11-07, starting at `0447dc4`. `render_queue.hpp`
needed the full backend header only for the small `SpriteInstance` value.
That definition now lives in `renderer/sprite_instance.hpp`, with its original
fields, defaults and comments. The backend includes the new header, retaining
its existing export; the queue includes the lightweight header directly.
Consumers using unrelated backend types include `backend.hpp` themselves.
The tilemap test's fake backend needed that direct include; production sources
needed no repairs. A mechanical comparison confirms that existing fields,
signatures, function bodies and assertions are unchanged, and that the sprite
instance definition moved byte-for-byte.

Clang and GCC produce identical dependency counts:

| Backend header edit | Before: C++ steps | After: C++ steps |
|---|---:|---:|
| `hex_demo` | 97 | 64 |
| `shapes_demo` | 97 | 64 |
| `kin_lua` | 93 | 62 |

Each game count includes one game PCH. The games compile 33 fewer sources,
**34.0% fewer C++ steps**, and update two runtime archives instead of five.
Skipped work includes four Lua binding sources, animation, pathfinding,
particles, tilemaps and component registrations. `kin_lua` skips 31 sources.
Renderer2D-header and Window-header counts stay at 62 and 83 respectively for
each game; this change specifically reduces the backend dependency.

Plans use the alternate Ninja manifest method described above. Every completed
real backend-header edit rebuilt exactly its planned object set. Both versions
use normal Release `-O2`, `KIN_DEV_FAST_SCRIPTING=OFF`, PCH on, compiler cache
and LTO off, mold, and four workers pinned to CPUs 0, 2, 4 and 6. Dependencies,
shaders and unaffected objects were already built. One observation per version
and compiler measured the edit through the final `hex_demo` link:

| Backend header edit through game link | Before | After |
|---|---:|---:|
| Clang 21.1.8 | 137.49 s | 72.60 s |
| GCC 15.2.0 | 143.23 s | 85.58 s |

These are exploratory single observations, not medians or controlled speedup
ratios. Unrelated compilation and video rendering were active on the host;
load averages are retained with each sample. Versions were measured in
separate phases, without agent-owned validation compilations overlapping the
timed builds. The rebuild-count reduction is the primary result. No percentage
wall-time improvement, complete-build improvement or ordinary game-source-edit
improvement is claimed here.

Both compilers passed 25 selected rendering, ECS, sprite animation, particle,
tilemap, scripting, scene, UI2, development and game tests; all four Lua API
reports matched the baseline. Eleven standalone renderer-header units,
including the queue and sprite-instance headers, compiled with PCH and unity
disabled for each compiler. A transitive repository-include audit selected
consumers of the queue or sprite-instance headers for no-PCH syntax/template
checks: 176 Clang 2D, 172 GCC 2D and 204 Clang 3D units all passed. These are
affected-consumer checks, not a whole-repository or 3D/GPU runtime test run.

Sizes and alignments of 14 types, including `SpriteInstance`, `RenderQueue` and
`PreparedSprite`, and Flip numeric values match before/after with both compilers
and render-probe mode on/off. `SpriteInstance` remains 52 bytes. This is not a
cross-platform ABI certification or Windows/MSVC validation.

After timing, the backend header's original bytes and timestamps were restored,
each measured game PCH was rebuilt against the restored header, six smoke
tests and the Lua API checks passed again per compiler, and a no-change game
build compiled and linked nothing.

The [queue/backend data](build_queue_backend_2026-11-07.json) retains exact plans,
skipped object sets, timing samples and host load, layout comparisons, affected
source lists, validation results and reproduction scripts.

### Renderer interface dependencies

Follow-up measured on 2026-11-07 from `3f88125`. `renderer2d.hpp` no longer
includes the complete backend or Window definitions. It forward-declares the
borrowed/owned services and includes its rendering values directly.
`DistanceFieldStyle`, `RendererBackendCapabilities`, `GpuScopeTiming`,
`ComputeBindings` and `RendererBackendStats` moved unchanged into
`renderer/backend_types.hpp`; the backend still exports them through that header.
The four methods that accessed the backend inline (`renderer_3d`,
`set_overdraw_view`, `pipeline_record`, `prewarm_pipelines`) now have the same
bodies in `renderer2d.cpp`.

Merge validation on 2026-11-08 added complete `Texture` and `DataBuffer`
includes to the value header: the MSVC standard library instantiates span
iterators that need complete element types. It also added direct backend and
Window includes to optional browser, example, tool and compact-test consumers.
These are portability corrections to the header split; the measurements below
were collected before these integration fixes.

Custom backend implementations and callers owning a `make_render_backend()`
result include `renderer/backend.hpp` directly. Code constructing or calling
Window includes `platform/window.hpp`. Repository consumers now own those
includes, including optional 3D tests. The audit also found the renderer's shape
implementation accessing the private backend; its direct include was added.

Clang and GCC agree on the `hex_demo` and `shapes_demo` plans:

| Edited header | Before: C++ steps | After: C++ steps |
|---|---:|---:|
| `renderer/backend.hpp` | 64 | 7 |
| `platform/window.hpp` | 83 | 60 |
| `renderer/renderer2d.hpp` | 62 | 62 |

A backend edit now compiles only seven implementation files, avoiding the game
PCH and main-source compilation. That is **89.1% fewer C++ steps**: 56 fewer
source compilations and one fewer PCH. Runtime archive updates drop from two
to one. Window edits retain the game PCH and avoid 23 source compilations,
**27.7% fewer C++ steps**. For `kin_lua`, the backend plan drops from 62 to 7
compilations and the Window plan from 82 to 59; renderer-header edits remain
at 60. These counts describe these specific header edits, not clean-build or
ordinary game-source-edit improvements.

Plans use a copy of the generated Ninja manifest, avoiding the dry-run CMake
glob-regeneration stop. All four real Clang header edits rebuilt exactly the
planned object sets. Both versions use Release with normal `-O2`,
`KIN_DEV_FAST_SCRIPTING=OFF`, PCH on, compiler cache and LTO off, mold, and four
workers pinned to CPUs 0, 2, 4 and 6. Dependencies, shaders and unaffected
objects were already built. Clang 21.1.8 measured each edit through the final
`hex_demo` link:

| Edited header | Before | After |
|---|---:|---:|
| `renderer/backend.hpp` | 103.84 s | 14.71 s |
| `platform/window.hpp` | 116.94 s | 74.40 s |

These are exploratory single observations per header/version under varying
background activity. Versions were measured in separate phases, without
agent-owned validation compilations overlapping the timed builds. The data
retains per-sample host load. Counts are the primary result; these observations
are not controlled wall-time speedup ratios. GCC received dependency and
functional validation, with no new wall-time samples in this round.

Both compilers passed 25 selected rendering, ECS, sprite animation, particle,
tilemap, scripting, scene, UI2, development and game tests, and all four Lua API
reports matched the baseline. Thirteen standalone renderer-header units,
including Renderer2D and the new value header, compiled with PCH and unity
disabled. A transitive repository-include audit selected affected consumers for
no-PCH syntax/template checks: 111 Clang 2D, 107 GCC 2D and 140 Clang 3D units
have passing results. The shape source passed after its direct-include repair.
One optional 3D shipping-feature check reported invalid UTF-8 in the system
`cstddef` header followed by a Clang frontend crash. The file was valid UTF-8 on
inspection; the same check passed twice in isolation without source or flag
changes. The dependency probes had rewritten headers while that syntax audit
was still running; that input-file race is a plausible explanation, although
the cause is unproven. The incident and retries are retained. Subsequent
header-edit probes run after all compiler checks and reject active worktree
compiler processes.

Mechanical checks confirm unchanged fields, signatures and function bodies,
including byte-for-byte movement of the five value definitions and preservation
of the four forwarding bodies. Sizes/alignments of 21 types, including Renderer2D
and the moved values, and Flip numeric values match before/after with both
compilers and render-probe on/off. Renderer2D remains 832 bytes with the probe
and 776 without it. Four forwarding methods become out-of-line; without LTO,
callers lose their previous opportunity to inline them. Runtime performance at
that boundary was not benchmarked. This round does not provide full 3D/GPU
runtime or Windows/MSVC validation.

After timing, all benchmark headers were restored, the game PCHs were rebuilt
against the restored inputs, six smoke tests and four API comparisons passed
again per compiler, and no-change game builds compiled and linked nothing.

The [renderer interface data](build_renderer_interface_2026-11-07.json) retains
exact plans, skipped objects, timing samples/load, layout comparisons, source
lists, the compiler incident, validation and reproduction scripts.

### Window value dependencies

Follow-up measured on 2026-11-07 from `6bf787e`. The Window ID alias,
`WindowConfig`, `SystemCursor`, `WindowChrome` and `WindowArea` now live in
`platform/window_types.hpp`. Their original definitions, defaults, comments and
ordering were copied unchanged. The pure `window_area_at` declaration moves
with its argument/result types; its implementation stays in `window.cpp`.
`window.hpp` includes the new header, retaining its existing exports.

App, Input and UI2 include the values they need. App, UI2 and the windowed app
interface forward-declare Window; the GPU device header also forward-declares
its Window reference arguments. Sources constructing Window or calling its
methods include `platform/window.hpp` directly, including demo callbacks using
`ctx.window.close_requested()`. Existing fields, signatures, assertions,
function bodies and inline definitions are preserved. This pass adds no new
forwarding calls or allocations and changes no optimization flags.

Clang and GCC agree on the `hex_demo` and `shapes_demo` plans:

| Edited header | Before: C++ steps | After: C++ steps |
|---|---:|---:|
| `platform/window.hpp` | 60 | 14 |
| `platform/app.hpp` | 18 | 18 |
| `platform/input.hpp` | 51 | 51 |
| `ui2/context.hpp` | 29 | 29 |

A Window-class header edit avoids 45 source compilations and the game PCH:
**76.7% fewer C++ steps**. The game main still compiles because its callback
calls Window methods. Runtime archive updates drop from two to one. For
`kin_lua`, Window-header edits drop from 59 to 13 source compilations.
The newly introduced value header still triggers 57 C++ steps per game,
including its PCH, and 56 for `kin_lua`. Changes to shared configuration, ID,
cursor or chrome contracts still rebuild their consumers; the improvement
above specifically concerns edits to the Window class header.

Dependency plans use the alternate Ninja manifest method, with header bytes
and nanosecond timestamps restored after each probe. All compiler checks and
builds finish before header probes start. The probe scripts also reject active
compiler, CMake and Ninja processes using this worktree/build directories.
Both real Clang Window-header edits rebuilt exactly their planned object sets.

The two versions use normal Release `-O2`, `KIN_DEV_FAST_SCRIPTING=OFF`, PCH on,
compiler cache/LTO off, mold, and four workers pinned to CPUs 0, 2, 4 and 6.
Dependencies, shaders and unaffected objects were already built. One Clang
21.1.8 observation per version measured the Window edit through the final
`hex_demo` link: **87.72 s before, 20.45 s after**. These are exploratory single
observations under varying external activity, not controlled speedup ratios
or clean-build comparisons. Host load is retained in the data. GCC received
dependency and functional validation, with no new wall-time samples.

Both compilers passed 28 selected tests: the earlier rendering, scene,
scripting, UI2 and game set plus platform, window-point scaling and input
workflows. All four Lua API reports match the baseline. Six separate platform,
UI and windowed-app header units compile with component/Flecs definitions and
PCH/unity disabled; the existing renderer contracts also build. A transitive
include audit selected affected sources for no-PCH syntax/template checks:
113 Clang 2D, 109 GCC 2D and 136 Clang 3D units have passing results. Nine initial
Clang findings were direct-include repairs, and all passed rechecks. A later
coverage check also compiled the newly configured windowed-app header unit.

Sizes and alignments of 31 types, including App, Input, UI2 Context/BoxStyle,
Window and the moved values, match before/after with both compilers and
render-probe on/off. Cursor, WindowArea and Flip numeric values also match.
This does not provide cross-platform ABI certification or full native desktop,
3D/GPU runtime or Windows/MSVC validation.

The previous renderer-interface record now documents that its header probes
overlapped syntax checks: rewriting compiler inputs is a plausible explanation
for that round's transient Clang crash, although the cause remains unproven.
This round also caught stale GCC Ninja log records after a separate configure
overlapped its build. The clean-plan assertion rejected that state before any
changed-version timing. A serialized rebuild refreshed the logs and passed
all tests/API comparisons again. Configuration, builds, syntax checks and
header probes are now kept in separate phases; the data retains this incident.

After timing, benchmark headers were restored, game PCHs were rebuilt against
the restored inputs, nine smoke tests and four API comparisons passed again
per compiler, and no-change game builds compiled and linked nothing.

The [Window value data](build_window_values_2026-11-07.json) retains exact plans,
skipped objects, timing samples/load, layouts/enums, affected source lists,
validation, workflow corrections and reproduction scripts.

### Native component field callbacks

Follow-up measured on 2026-11-07 from `12475f9`. A fresh clean Kin + `hex_demo`
profile uses normal Release `-O2`, `KIN_DEV_FAST_SCRIPTING=OFF`, PCH on,
compiler cache/LTO off, mold, and four workers pinned to CPUs 0, 2, 4 and 6.
It recompiles 221 own C/C++/PCH/generated-data outputs through the final game
link; third-party compilation and shader generation are already built.
This is not a from-scratch dependency build.

Fresh serial Clang traces identify substantial optimizer/code-generation work:
`script_engine.cpp` spends 23.42 of 35.57 traced seconds in the backend;
ECS `ui2.cpp` spends 16.62 of 25.05 seconds there. These are diagnostic,
instrumented runs, separate from the ordinary timings below. Nested trace
categories overlap and must not be added together. The clean build keeps four
workers busy compiling; the final game link takes less than 0.2 seconds.

Native reflected `.field()` accessors previously became `std::function`
readers/writers, then another pair of `std::function` callbacks stored in the
registry. Direct and nested member projections now stay typed until that final
callback boundary. This removes an adapter layer and its template machinery.
Custom `.property()` retains its public `std::function` parameters. The original
validation order/error strings, descriptor construction, accessor/callback
bodies, metadata methods and builder fields are preserved. The custom callback
validity check is evaluated before moving the functions.

Uninstrumented serial Clang 21.1.8 compiles use matching target PCHs and normal
`-O2` on CPU 2. One ABBA then one BAAB block gives four observations per version
and source. Stable include overlays select the two component-header versions;
dependency files verify the selected header, with no worktree header rewrites.
The unchanged JSON source is a control:

| Source | Before median | After median | Reduction |
|---|---:|---:|---:|
| `scene_components_render.cpp` | 11.215 s | 9.945 s | 11.3% |
| `scene_components_physics2d.cpp` | 10.845 s | 10.390 s | 4.2% |
| `core/json.cpp` (unchanged control) | 0.895 s | 0.900 s | none |

The rendering and physics registration object files shrink by 17.6% and 7.5%
respectively. These are object sizes, not final executable-size reductions.

All four clean-build observations are retained in order:

| Run | Original callbacks | Typed field callbacks |
|---|---:|---:|
| Initial observation | 290.70 s | 207.78 s |
| Return-to-baseline / optimized repeat | 204.98 s | 198.78 s |

External GCC builds became active during the first baseline, making its large
gap unsuitable as a speedup claim. The later pair observes about 3% lower wall
time, but host activity remains uncontrolled and each row has one observation
per version. It does not establish a precise whole-build speedup or a cumulative
improvement since the optimization project began. No agent-owned validation
compilers overlap timing; configuration, header writes and Ninja-log changes
are also serialized with compilation.

The game-edit loop still compiles exactly `hex_demo`'s main source and links.
Three source-edit observations have medians of 2.15 s before and 1.88 s after;
no-op medians are 0.27 s and 0.17 s. These separate-phase observations also
include host/filesystem noise; no causal edit-loop improvement is claimed.

An `-O2` runtime probe registers 1,000 integer fields of one private component
and counts C++ `operator new` during field registration, excluding world/type
creation. Calls fall from 2,022 to 22, and allocated bytes from 1,406,832 to
1,342,832: two allocations and 64 bytes removed per field on this libstdc++
configuration. Remaining calls include registration-vector growth. Small-buffer
behavior may differ with other standard libraries. The probe visits a million
integer fields per sample, with three warmups and ten measured samples per
process, three processes/version. Process medians are 7.544–7.551 ms before
and 5.703–6.707 ms after; checksums and direct/nested/string patches match.
These synthetic callback results are not full-game FPS measurements.

Clang and GCC each pass 39 selected ECS, rendering, scene, scripting, UI2,
platform and game tests; all four Lua API reports match the baseline per compiler.
The existing standalone renderer/platform header contracts also build. New
regression coverage checks direct/nested/string/vector fields, custom callbacks,
metadata, errors, rejected patches and callbacks copied to a world that outlives
the original registry. Eleven optional 3D source/test units pass Clang syntax and
template checks without PCH. After restoring the edit-probe source, the canonical
game main is recompiled, game/component smoke tests and API comparisons pass
again, and no-change `hex_demo`, `shapes_demo` and `kin_lua` builds compile/link
nothing with either compiler. This does not provide Windows/MSVC or full native
desktop/3D/GPU runtime validation.

The [field callback data](build_field_callbacks_2026-11-07.json) retains every
clean-build timeline, paired compiler sample, trace summary, edit-loop sample,
allocation/runtime observation, header hash, validation result and reproduction
script. It also records recovery of the successful baseline after a C/C++ action
counting bug, and recovery of the completed first trace after adapting its
parser to Clang 21 async Source events; neither run was discarded or repeated.

### Shared Lua API bookkeeping

Follow-up measured on 2026-11-07 from `c002068`. The private Lua API header now
calls one non-template `record_api_type` function for the registry's type and
property metadata. API inspection and member enumeration also move into
`lua_api_surface.cpp`, compiled by the base `kin_scripting` library. The five
Lua consumer sources, binding lists, callback bodies, receiver/liveness checks,
constructors, properties and public headers are unchanged. The inspection
function bodies move unchanged; the recording statements receive the existing
metatable name and read the property-name vector through a const reference.

This reduces repeated template/optimizer work and gives the moved implementation
a narrower edit scope. It adds ordinary calls during VM registration and API
inspection; it changes no per-frame Lua callback body or invocation adapter.
Normal engine/game Release `-O2`, sol2 safety settings and
`KIN_DEV_FAST_SCRIPTING=OFF` are retained. PCH is on; compiler cache and LTO are
off, with mold linking. Source is on Linux; build/output/TMPDIR use NTFS/FUSE.

A focused uninstrumented Clang 21.1.8 comparison uses matching target PCHs and
stable source/header copies, with one ABBA then one BAAB block. Each of the
four observations/version compiles `script_engine.cpp` on CPU 2, bracketed by
an unchanged `core/json.cpp` compile on that same CPU:

| `script_engine.cpp` | Before | After |
|---|---:|---:|
| Median | 32.465 s | 31.515 s |
| Range | 31.76–35.07 s | 31.13–31.82 s |

That observes about **2.9% lower time for this file**, a modest local estimate.
All control brackets took 0.90–0.93 s except the first pre-compile control at
1.49 s; that sample and every measured binding compile are retained. Fresh
instrumented traces also retain frontend/backend, optimizer, template and
function-event totals separately. Nested trace categories overlap, and traced
runs are diagnostic rather than the source of the percentage above.

The newly added source takes 1.55 s in the direct pilot and is included in the
complete affected-set comparison. Eight alternating four-worker batches compile
all five Lua consumers, plus the shared source in the changed version and an
unchanged JSON control, then link replacement Lua objects into a cached
`hex_demo`. Per-source CPU affinity is fixed to CPUs 0, 2, 4 and 6. The median
batch times are 42.65 s before and 38.955 s after. However, the control's medians
also change from 1.45 to 1.135 s and individual observations vary substantially.
This parallel ratio is unsuitable as a causal speedup claim. Those runs prompted
the serial bracketed comparison above; none were discarded. Other engine/game
objects, third parties and shaders are prebuilt, and explicit replacement
objects force the Lua units into the link. These batches do not measure a full
clean Kin build or its ordinary dependency scheduling. No whole-project or
cumulative speedup is inferred.

The five directly compiled original Lua objects total 6,889,752 bytes; the six
changed objects, including the new shared file, total 6,740,976 bytes, about
2.2% less. These are object-file sizes, not a final executable-size comparison.

Edits to the moved metadata/inspection implementation previously changed the
private header consumed by five Lua sources. They now change one source file.
Three real trailing-comment edits each rebuilt exactly the one planned
`kin_scripting` object, its archive and the final game link, taking 1.95, 1.97
and 1.95 s. Source bytes and nanosecond timestamps are restored after each
sample. This result concerns edits to the moved implementation: binding-list
edits still compile their containing Lua source, and private declaration or
template-header edits reach all six consumers.

Clang and GCC each pass 39 selected ECS, rendering, scene, scripting, UI2,
platform and game tests, and all four Lua API reports match the baseline per
compiler. Twelve no-PCH syntax/template checks pass: the five consumers and new
shared source with Clang 3D enabled, and the same six with GCC physics/audio
disabled. Mechanical reconstruction checks the moved bodies, registration
wrappers, unchanged consumer sources and the single CMake source addition.

Runtime spot checks use three processes/version in alternating order, with
30 samples after three warmups on CPU 2. Fresh scene VM creation, 20,000 scene
bridge calls and 50,000 entity bridge calls complete with identical resulting
positions. Across the three process medians, scene/entity bridge-loop medians
change by less than 0.4%;
all raw observations are retained. These probes do not establish full-game FPS.
The canonical shared source is recompiled after restoring the comment probe;
four smoke tests and four API comparisons pass again per compiler. No-change
`hex_demo`, `shapes_demo` and `kin_lua` builds compile and link nothing. Optional
3D checks cover compilation, not full native desktop/3D/GPU runtime or MSVC.

The custom benchmark harness initially omitted Ninja's `builddir`, appending
60 unique benchmark-output records to the canonical build log. No configure or
other Ninja process overlapped, and canonical object records were retained.
The canonical clean-plan assertion passed before the implementation-edit probes.
Future manifests isolate logs per sample. Ninja compaction retained existing
benchmark outputs, so final cleanup filters exactly those 60 output records,
preserves every canonical record and passes no-change builds again. The original
harness and log snapshots are retained.

The [Lua API bookkeeping data](build_lua_surface_2026-11-07.json) records every
initial/pilot, parallel and serial sample, controls/load, fresh trace totals,
implementation edits, runtime samples, hashes, compiler flags, validation,
workflow correction and reproduction scripts.

### Split scene scripting translation unit

Follow-up measured on 2026-11-07 from `82043ec`. Scene scripting now has three
implementation files: `script_engine.cpp` retains runtime lifetime, file/module
loading and public callback scheduling; `script_scene_api.cpp` owns native API
methods, component/query helpers, typed invocation and reload migration;
`script_scene_bindings.cpp` owns the ordered registrations and receiver adapters.
Both added sources belong to `kin_scripting_scenes` and use its existing PCH and
normal Release `-O2`. Compiler cache/LTO are off, mold is used, and
`KIN_DEV_FAST_SCRIPTING=OFF`; sol2 safety settings are unchanged.

Forty-eight native method bodies move out of class definitions; 21 small methods,
including entity receiver/liveness checks, stay inline. Host/Impl definitions,
userdata fields/defaults/access and all 27 ScriptEngine member bodies are
preserved. Three explicit instantiations keep typed `call_optional` invocation
out of the runtime unit, without an extra Lua object wrapper/allocation per
callback. Outlining can remove previous inlining opportunities; runtime probes
below bound only the sampled operations. Guard/command TLS definitions stay
with native execution so direct accesses retain known static initialization.
Public C++ headers and Lua registration names are unchanged.

The userdata types require one shared C++ identity across translation units.
An initial `kin::scripting_detail` definition caused about a 5% scene-bridge
slowdown with both compilers. Sol2 strips anonymous-namespace text from the old
type names; the new explicit prefix made registry keys longer, crossing the
bundled Lua `LUAI_MAXSHORTLEN` limit of 40. Hidden visibility alone reduced but
did not remove the Clang regression. The final shared types use a compact hidden
`kin::s` namespace and readable aliases in `scripting_detail`. The existing
receiver/liveness bodies are retained. A regression test checks all ten primary
and reference keys are at most 40 bytes. Its negative control links the measured
long-name prototype and must fail the length assertion. Internal C++/sol registry
identities change; public Lua type names/reports remain the same.

A focused Clang rebuild/link comparison uses one ABBA block, two observations
per version, with four workers pinned to CPUs 0, 2, 4 and 6. The original combined
source and the new registration unit both run on CPU 2; native API/runtime work
can run on CPUs 0 and 6. An unchanged JSON compile uses CPU 4. Stable source and
private-header copies use matching target flags/PCH, with dependency files
checking header selection. Linker dependency output and Ninja logs are isolated.
All other objects/dependencies/shaders are cached, and explicit replacement Lua
objects are linked into `hex_demo`, whose headless report matches in every run.
This forces archive members an ordinary game might omit and does not model a
complete clean project or its normal scheduling.

The median scene compilation-set/game-link time falls from **30.865 to 18.240 s**,
about **40.9% less**. It uses more combined compiler CPU: **30.680 to 33.155 s**,
about **8.1% more**. Parallelism and smaller edit scopes provide the gain; a
single-worker rebuild of the whole scene set can be slower. Individual child
RSS does not measure simultaneous total memory, and several split units may run
at once. All per-source CPU/RSS observations and controls, including a 1.39 s
before control outlier, are retained.

Real implementation-only edits use the canonical Ninja graph and rebuild through
the final `hex_demo` link. Each rebuild compiles exactly its one planned object;
bytes and nanosecond source timestamps are restored after every sample:

| Implementation edit | Original combined file | Split file | After observations |
|---|---:|---:|---:|
| Runtime/file loading | 32.68 s median | 5.71 s median | 3 |
| Native API/reload/typed dispatch | 32.68 s median | 10.01 s | 1 |
| Registration list/adapters | 32.68 s median | 18.50 s | 1 |

The original median has three observations (31.66–33.07 s). Runtime-only edits
use **82.5% less wall time**, about **5.7 times faster**, in these local samples.
Native API and registration timings are single exploratory observations.

The original combined file contained all three roles. Shared private declaration
or inline-definition edits now compile three sources; ordinary game-main edits
still have their original source scope. After probes, all three canonical scene
objects are rebuilt from the restored source bytes.

A fresh clean-own-output pair recompiles 222 original or 224 split C/C++/PCH/
generated-data outputs through the game link, with dependencies and shader
generation prebuilt. Wall time is 232.81 s before and 202.17 s for the final split.
The 221 unchanged compile tasks have a median after/before wall-time ratio of
0.873: they also became substantially faster. Background activity and scheduling
confound this pair, so its 13.2% gap is not attributed to the refactor. A retained
long-name prototype clean run took 222.45 s. All timelines and loads are recorded;
none provide a precise clean-project or cumulative speedup.

Final runtime checks use both compilers, three processes/version/compiler in
alternating order, and 30 measured samples after three warmups on CPU 2. They
measure fresh VM creation, 20,000 scene bridge calls and 50,000 entity calls.
Scene/entity medians across process medians range from 3.4% faster to 1.6% slower;
VM creation is about 2.1–2.2% slower. Final entity positions match in every run.
These narrow, noisy probes do not establish whole-game FPS. The final probe's
code/read-only sections grow by about 0.48% on each compiler; unstripped file and
individual object sizes also include symbol/COMDAT overhead.

Clang and GCC each pass 39 selected ECS, rendering, scene, scripting, UI2,
platform and game tests, including the registry-key regression, and all four Lua
API reports match the baseline per compiler. Clang and GCC each pass 80 no-PCH
assertions comparing all ten original/new userdata sizes, alignments and six
type traits. Twelve further no-PCH cases compile the three sources and three
private headers with Clang 3D enabled and GCC physics/audio disabled. Mechanical
checks preserve the 48 moved/21 inline bodies and all 27 ScriptEngine member
bodies; the transformation regenerates all six implementation/header files and
CMake exactly. Four final smoke tests and API comparisons pass again per compiler;
no-change game/Lua builds compile and link nothing. This does not provide
Windows/MSVC or full native desktop/3D/GPU runtime validation.

The [scene split data](build_scene_bindings_split_2026-11-07.json) records complete
clean-build timelines and unchanged-task controls, isolated rebuild/link samples,
real edit plans/timings, runtime probes for both compilers, prototype regressions,
layout/trait and body-preservation checks, flags, hashes and reproduction scripts.
The initial missing direct includes for preloaded scenes and workflow binding
declarations are repaired; their failed pilot logs are retained. All source,
configuration and Ninja-log probes are serialized with compilation.

### Split entity scripting translation unit

Follow-up measured on 2026-11-07 from `2398342`. The former 1,252-line
`entity_scripts.cpp` now separates runtime/file loading/reload/scheduling from
`entity_script_bindings.cpp` (entity methods and pool/tween bridge) and
`entity_service_bindings.cpp` (game services, physics queries and the Lua self
prelude). A private header shares the original state fields and live-service
guard. Public C++ headers and Lua registration names are unchanged.

The 15 moved helper/runtime method bodies, all 14 public member definitions,
the concatenated binding body/registration order, userdata fields/constructor/
`alive`/`live` bodies, service guard and Lua self prelude are preserved. Public
member definitions remain byte-identical. The shared userdata uses the compact
hidden `kin::s::ScriptEntity` identity; a regression checks both sol2 primary
and reference registry keys stay within the bundled Lua 40-byte short-string
limit. Internal C++/sol identities change; public API reports match.

Measurements retain normal Release target `-O2`, the existing PCH, mold,
compiler cache/LTO off and `KIN_DEV_FAST_SCRIPTING=OFF`. Safety checks and Lua
limits are unchanged. A fresh diagnostic Clang trace spends 24.66 s overall,
10.34 s in the frontend and 14.25 s in the backend. The instrumented trace is
kept separate from timing samples.

A focused Clang comparison runs two ABBA blocks, four observations per version,
compiling stable original or final source/header snapshots with matching flags
and four fixed workers. The original combined file and final longest service
file use CPU 2; final entity/runtime units use CPUs 0/6. An unchanged JSON
compile uses CPU 4. No worktree compilers overlap the experiment. Host activity
remains uncontrolled, and every observation/load is retained.

| Entity compilation set | Before | After |
|---|---:|---:|
| Median elapsed time | 22.80 s | 10.91 s |
| Median combined compiler CPU | 22.76 s | 26.83 s |
| Median unchanged JSON control | 0.94 s | 0.94 s |

This set takes **52.1% less wall time**, about **2.1 times faster**, but uses
**17.9% more compiler CPU**. Before ranges from 22.63–23.28 s; after from
10.77–11.08 s. The before control's 1.39 s outlier is retained. This experiment
measures compilation only, without archive/link work or ordinary clean-project
scheduling. It does not establish a whole-project or cumulative speedup.

Real implementation-only edits use the canonical Ninja graph, four workers
pinned to CPUs 0/2/4/6, and rebuild through the final `hex_demo` archive/link.
Each plan and actual build compiles exactly its one expected C++ object:

| Implementation edit | Original combined file | Split file | After observations |
|---|---:|---:|---:|
| Runtime, reload and scheduling | 26.44 s median | 7.51 s median | 3 |
| Entity methods and bridge | 26.44 s median | 9.54 s | 1 |
| Game-service bindings and physics | 26.44 s median | 11.53 s | 1 |

The original three observations are 24.31, 26.53 and 26.44 s. Final runtime
observations are 7.47, 7.51 and 7.56 s: **71.6% less wall time**, about
**3.5 times faster**, for that edit scope. The other two final timings are
single exploratory observations. Source bytes and nanosecond timestamps are
restored after each probe; canonical objects are then rebuilt from original
source bytes. Shared private-header edits reach three production sources and
the entity regression test; game-main implementation scope remains unchanged.

Parallelism and smaller edit scopes provide the gain. Repeated frontend work
and templates increase aggregate CPU; a single-worker rebuild of the full set
can be slower. Several units may use memory simultaneously, and individual
max-child RSS does not measure the combined concurrent peak. No lower
optimization/safety settings or whole-build CPU/memory reduction are claimed.

Runtime probes use both compilers, three processes/version/compiler in
alternating order, with 30 measured samples after three warmups on CPU 2.
Entity bridge-loop medians across processes are 36.033 to 36.732 ms with Clang
and 36.593 to 36.990 ms with GCC, approximately **1.9% and 1.1% slower**.
Unchanged scene VM/control-loop medians vary by up to about 2%; all final entity
positions match. These narrow probes do not establish whole-game FPS or bound
every service/query operation. The probe's code/read-only sections grow about
0.07% with Clang and 0.22% with GCC; unstripped file size barely changes.

Clang and GCC each pass 39 selected tests and all four Lua API reports match
their baseline. Eight no-PCH assertions per compiler compare original/new
userdata size, alignment and six type traits. Eight further no-PCH cases compile
the three sources/private header with Clang 3D enabled and GCC physics/audio
disabled. Mechanical checks regenerate the four source/header files and CMake
exactly and verify body/field/order preservation. Final entity/runtime/game
smoke tests and API comparisons pass per compiler; no-change builds compile
and link nothing. Windows/MSVC and native desktop/3D/GPU runtime are untested.

The [entity split data](build_entity_bindings_split_2026-11-07.json) records all
samples, controls/load, runtime probes, binary sizes, source hashes, compiler
flags, validation and reproduction scripts. The initial baseline dry-run stopped
at a pending CMake regeneration after branch creation; the source was restored
and the build settled before the three recorded edit samples.

### Shared UI2 record collection

Follow-up measured over 2026-11-07–08 from `5b425e4`. Widget-record collection in
`ecs/ui2.cpp` now shares its query cache holder type and iteration adapter across
51 widget types. Each type retains a private key/query per Kin-owned world and
uses the original typed `query_builder<T&>()` factory; standalone Flecs worlds
still build an uncached query per call. Record assignment and drawing remain
typed. `field_at` handles sparse storage, with row zero for shared inherited
fields. Code outside this collection helper, including registrations, builders,
layout, sorting and clipping, is byte-identical. Public headers/CMake are unchanged.

This removes repeated compiler work without introducing more translation units.
The Clang object contains 8 cache holder vtables instead of 56, because the
other UI2 query paths retain their own typed caches. Runtime query instances
are still per widget/world. Collection now uses private keys instead of sharing
the typed `each()` cache with other callers: querying the same signature
elsewhere can retain another query, including the layout's prompt queries.

Fresh diagnostic Clang compilations take 23.41 s before and 18.77 s after. Instrumented
profiles are separate from the following normal Release `-O2` samples. Existing
PCH is on; compiler cache/LTO and `KIN_DEV_FAST_SCRIPTING` are off, with mold
linking and unchanged safety settings.

Five observations/version/compiler use stable original/final source/private
header copies and matching commands/PCHs without profiling. Before/after order
alternates, reversing on every second pair. UI2 compiles on CPU 2 while an
unchanged JSON control compiles on CPU 4. No owned validation compilers overlap
the benchmark. Medians are:

| Compiler | Compile before | Compile after | Compiler CPU before → after | Peak compiler RSS before → after |
|---|---:|---:|---:|---:|
| Clang 21.1.8 | 22.09 s | 18.99 s | 22.05 → 18.94 s | 503 → 426 MiB |
| GCC 15.2 | 29.08 s | 24.24 s | 28.76 → 23.90 s | 988 → 866 MiB |

Background activity and SMT siblings are uncontrolled. Load/control variation
prompted two additional pairs per compiler; all initial samples, including
33.89 s Clang and 42.33 s GCC before outliers, remain in the summary/data.
These observed ratios are local estimates, not precise causal percentages or
clean-project/cumulative speedups. RSS is per compiler, not total build memory.

Three real implementation-only comment edits per version rebuild through the
Clang `hex_demo` archive/link, with four workers pinned to CPUs 0/2/4/6.
The median is **22.44 → 19.07 s**, an observed **15.0% less wall time**.
Each plan and actual build compiles exactly one expected C++ object. Source
bytes and nanosecond timestamps are restored after every sample, then the
canonical object is recompiled from restored bytes. Rebuild fanout is unchanged.
Host conditions also limit the precision of this edit-loop comparison.

Renderer checks use three alternating processes/version/compiler/world mode,
each with 30 measured samples after three warmups on CPU 2. Each sample renders
20 frames of 500 panels, 500 empty labels and 500 spacers through an identity
backend, asserting all 1,500 records and representative pointers/IDs/bounds.
Across process medians, Clang's owned-world case is 2.8% faster; standalone
Flecs is 3.1% slower. GCC cases are 2.2–2.7% slower. Dynamic field lookup and
record assignment add per-row work. These narrow CPU probes have no GPU work,
layout trees or non-empty text and do not establish whole-game FPS.

Clang and GCC each pass 39 selected tests and all four Lua API reports match
their baseline. All 111 dirty-observer descriptors match, including IDs, names,
events and query/access flags. New regression coverage exercises sparse panels,
shared inherited labels, disabled entities/components, deferred additions/
removals, toggles and refreshed table matches in both owned and standalone
worlds. The full UI2 suite, including the new regression, also passes when
linked against the original object on both compilers.

Four no-PCH syntax/template cases compile the implementation and test file
with Clang 3D enabled and GCC physics/audio disabled. Seven final UI2/game smoke
tests and API comparisons pass per compiler; no-change builds compile and link
nothing. Windows/MSVC and native desktop/3D/GPU runtime are untested.

The [UI2 collection data](build_ui2_collection_2026-11-08.json) retains all
profiles, timing/control/load observations, real edit plans, runtime samples,
object/cache holder counts, observer descriptors, validation, hashes, flags and
reproduction scripts.

### Split native workflow stepping from Lua bindings

Follow-up measured on 2026-11-08 from `7eef736`. `workflows2d.cpp` now owns
`ScriptWorkflows2D` lifetime, world sharing, stepping, scene handoff, tween
settlement and native Impl helpers. `lua_workflows2d.cpp` keeps registration,
userdata, table/value conversion and reload cloning. A private header shares
the original native fields and tween state. Both files belong to the existing
scripting target with normal Release `-O2` and its PCH. Compiler cache/LTO and
`KIN_DEV_FAST_SCRIPTING` are off; safety settings and public headers are unchanged.

All ten public native member definitions, settlement/mutation guards and native
fields are preserved. All three userdata definitions remain local to the Lua
binding file, retaining their existing namespace identities. Native `follow()`
returns shared state; two Lua callbacks construct the original `LuaTween2D`
aggregate around it. This moves the existing construction across the boundary
without an extra allocation/Lua wrapper. Other callback bodies, captures,
registration order, value conversion and reload cloning remain byte-identical.

The original diagnostic trace takes 16.58 s (6.82 frontend, 9.61 backend);
instrumented time is excluded from performance comparisons. A first split put
the tween userdata in a shared hidden namespace. It passed tests but gave little
GCC compilation-set wall improvement while using substantially more CPU. That
prototype's complete sources, generator and measurements are retained. The final
boundary keeps userdata private to bindings and shares only native state; no
direct same-host prototype-vs-final causal speedup is claimed.

Three observations/version/compiler use original/final source and private-header
snapshots, matching target flags/PCHs and alternating before/after order with the
middle pair reversed. The original combined file and final binding file run on
CPU 2; the added native source runs on CPU 0, with an unchanged JSON control on
CPU 4. No owned validation compilers overlap timing. The medians are:

| Workflow compilation set | Wall before → after | Combined compiler CPU before → after |
|---|---:|---:|
| Clang 21.1.8 | 15.04 → 14.34 s | 15.01 → 18.06 s |
| GCC 15.2 | 14.42 → 12.91 s | 14.23 → 17.46 s |

Observed set wall time is about 4.7% lower with Clang and 10.5% lower with GCC,
while combined compiler CPU is **20.3% and 22.7% higher**. This measures
compilation only, with no archiving/linking or ordinary clean-project scheduling.
Host activity, frequency and SMT siblings are uncontrolled; all samples,
controls and loads are retained. These are local estimates, not precise
clean-project/cumulative speedups. Parallel units can use memory simultaneously;
per-child RSS does not measure total concurrent peak memory. A single-worker
rebuild of the complete set can be slower.

Real implementation-only edits use the canonical Clang Ninja graph through
`hex_demo` archive/link, four workers pinned to CPUs 0/2/4/6, three observations
per source/version:

| Implementation edit | Original combined file | Final source |
|---|---:|---:|
| Native lifetime/stepping/helpers | 12.31 s | 3.61 s |
| Lua registration/conversion/cloning | 12.31 s | 14.30 s |

Native-only edits take **70.7% less wall time**, about **3.4 times faster**,
in these samples. The binding-edit samples are slower than the original block;
host conditions also changed between blocks, so they do not establish a precise
causal effect for registration edits. Every plan/actual build compiles exactly
one expected C++ object. Bytes and nanosecond timestamps are restored after each
sample, then both canonical objects are refreshed from restored bytes. Shared
private-header edits reach two sources; game-main source scope is unchanged.

Five alternating processes/version/compiler use the same assertions-enabled
`-O2` probe object, each with 30 samples after three warmups on CPU 2. Initial
prototype timings showed roughly twofold host-wide changes; final probes use
five fresh processes/version and retain every prototype/final observation.
Medians across process medians, in milliseconds:

| Runtime workload | Clang before → after | GCC before → after |
|---|---:|---:|
| Bind 50 VMs | 5.856 → 6.003 | 6.466 → 6.286 |
| 400,000 input queries | 27.950 → 26.660 | 27.996 → 27.715 |
| 100,000 tween state calls | 7.342 → 7.132 | 8.357 → 8.185 |
| 1,000 native steps | 0.661 → 0.663 | 0.635 → 0.657 |
| 40,000 controller table calls | 20.713 → 21.908 | 19.455 → 20.395 |

Input/tween-state cases improve by up to about 4.6%; controller-table cases are
4.8–5.8% slower. VM creation and native stepping also move in both directions.
These narrow CPU probes assert states/counts, but do not establish whole-game
FPS or bound all asset, preload, pool and reload operations.

Clang and GCC each pass 39 selected tests and all four Lua API reports match
their baseline. The new regression checks all three userdata's primary/reference
registry keys stay within the bundled Lua 40-byte short-string limit. Sixteen
no-PCH assertions per compiler compare original/new state and tween-wrapper
sizes, alignments and six traits. Six more no-PCH cases compile both sources and
the private header with Clang 3D enabled and GCC physics/audio disabled. The full
workflow suite, including the new regression, also passes linked against the
original combined object per compiler. Final six scripting/game smoke tests,
API comparisons and no-change builds pass per compiler. Windows/MSVC and full
native desktop/3D/GPU runtime are untested.

The [workflow split data](build_workflow_split_2026-11-08.json) retains all
prototype/final compilation, real-edit and runtime observations, controls/load,
layout/body checks, executable sizes, hashes, flags, validation and reproduction.


## C++ modules pilot

A native Linux Clang 21 pilot compared Hex Demo's existing PCH with an
`import kin.runtime` facade and a PCH covering the same eleven engine headers.
The facade works with the existing native libraries and preserves their ABI.
It exports selected names from headers in the global module fragment; engine
implementation files still use headers. This is an incremental migration pilot,
not a fully modular engine.

The balanced run used Clang 21.1.8, CMake 4.2.3, Ninja 1.13.2, Release `-O2`,
four workers pinned to four CPUs, the default GNU linker, no LTO and no compiler
cache. Project-pinned dependencies and engine libraries were built before the
measurements. All six case orderings were rotated to reduce ordering bias.
Times include the final executable link:

| Rebuild scope | Existing PCH | Module + standard PCH | Same-coverage PCH |
|---|---:|---:|---:|
| Warm game source edit, median of 12 | 1.723 s | 1.567 s | **1.463 s** |
| Game/API preparation from cold, median of 3 | 3.766 s | 4.020 s | 3.789 s |
| Native pathfinding source edit, median of 3 | 1.932 s | 1.947 s | 1.925 s |
| Pathfinding header comment edit, median of 3 | 7.429 s | 8.423 s | 8.249 s |
| No-op, median of 12 | 0.059 s | 0.060 s | 0.058 s |

The module facade shortened warm game edits by about **9%**, while the
same-coverage PCH shortened them by **15%** and was about **7% faster than the
module client**. Native engine edits and no-ops did not improve. Cold module
preparation used 4.91 CPU seconds versus 3.75 for the original PCH, with wall
time about 7% higher. The header-comment rebuild was about 13% slower with the
module and 11% slower with the broader PCH. This is an invalidation test with
unchanged declarations, not a measurement of an ABI-changing API edit.

Cold means rebuilding the game PCH, module BMI/object when applicable, game
object and executable, with the engine and dependencies already built. It is
not a full project clean build. Maximum process RSS for a warm game edit fell
from about 188 MiB to 173 MiB with the broader PCH; the module used about
187 MiB. RSS is the largest single process, not total concurrent memory.
Background host load and CPU frequency varied; interpret these as one host,
one game and one compiler. Exploratory reduced-BMI and no-consumer-PCH module
variants did not beat the ordinary module client.

The normal Hex build therefore keeps headers and adds five stable engine
headers to its optional PCH: hex coordinates, JSON, pathfinding, window and UI2
text. Existing transitive PCH coverage supplies the other facade headers. This
helps game-body edits while those APIs remain stable; editing one of the added
headers also rebuilds the game PCH. Game-owned headers remain outside the PCH.
No other game's PCH changes.

A separate A/B of that final five-header change used the original, unchanged
source include order and normal build plumbing. Six alternating before/after
rounds (twelve samples each) reduced median edit-to-link time from **2.380 s to
1.981 s**, about **17%**; CPU time fell from 2.37 s to 1.96 s. Maximum process
RSS fell from about 189 MiB to 174 MiB. All twelve headless checks passed. Host
load changed between this batch and the module comparison, so compare cases
within each batch rather than mixing their absolute times.

The [standalone experiment](../experimental/cpp_modules/README.md) retains the
working module and three comparable clients without adding module scanning or
new compiler/CMake requirements to normal builds. The
[raw samples and toolchain metadata](build_cpp_modules_2026-11-08.json) identify
the measured checkpoint and distinguish exploratory, balanced and production
PCH comparisons. All 18 balanced-case headless checks passed with identical
path length, path cost and reachable-cell counts. The final native 2D suite
passed (118 executed tests, two GPU tests skipped with the dummy video driver).
GCC 15 also compiled the final PCH and real Hex source with invalid-PCH warnings
treated as errors; this is a compile smoke check, not a GCC module benchmark.
The standalone project also built all three clients and passed all three
headless tests.

A larger migration should earn its complexity with a new benchmark: partition
frequently consumed, stable engine interfaces and measure representative games,
clean preparation, implementation edits and interface invalidation against a
matched PCH. Modules do not remove template instantiation or optimizer work for
new bindings. CMake's [module documentation](https://cmake.org/cmake/help/v4.2/manual/cmake-cxxmodules.7.html)
and Clang's [standard module documentation](https://clang.llvm.org/docs/StandardCPlusPlusModules.html)
cover the build graph and compiler constraints; this pilot does not establish
support on other platforms.


## Broader standard-library and game PCHs

Starting from merged `main` at `ac7b57e`, Clang traces showed repeated
`filesystem` parsing in engine sources and UI/ECS API parsing in game sources.
The common standard-library PCH now includes `filesystem`. Engine PCHs still
exclude Kin headers, so this adds no engine-API invalidation dependencies.
Charts, Text Edit, Shapes and ECS Parallel also precompile their own stable
engine APIs. Game logic stays outside the PCH, and the normal header includes
remain in each source. All changes apply only with `KIN_ENABLE_PCH=ON`.

The native Linux measurements used Clang 21.1.8, libstdc++ 15, CMake 4.2.3,
Ninja 1.13.2, Release `-O2`, four available CPUs, four build workers, the default
GNU linker, no LTO and no compiler cache. The engine probe prepared matching
private PCHs outside the normal build and rotated all six case orderings.
Median **source-to-object** times, six samples per case:

| Engine source | Previous standard PCH | With filesystem | Reduction |
|---|---:|---:|---:|
| renderer2d.cpp | 3.079 s | 2.772 s | 10.0% |
| widgets_editor.cpp | 2.578 s | 2.093 s | 18.8% |
| services_catalog.cpp | 1.804 s | 1.269 s | 29.6% |

These exclude executable linking and do not measure a full clean engine build.
PCH generation was recorded separately, once per target/case, in the raw data.
Adding `chrono` as well gave little further benefit in these files and was not
retained. Clang traces locate the repeated filesystem parse at roughly 0.3 s
in each source; inclusive nested trace spans must not be summed.

The real game comparison includes the final executable link. It alternates
before/after order and reverses game order on alternating rounds, with six
warm edits and no-ops per game/case. Median **edit-to-link** times:

| Game | Common game PCH | Broader API PCH | Reduction | Retained |
|---|---:|---:|---:|---|
| Charts | 2.851 s | 2.039 s | 28.5% | Yes |
| Text Edit | 2.065 s | 1.431 s | 30.7% | Yes |
| Shapes | 2.609 s | 1.700 s | 34.8% | Yes |
| ECS Parallel | 5.779 s | 4.950 s | 14.4% | Yes |
| Lighting | 1.567 s | 1.497 s | 4.5% | No |
| Isometric | 1.187 s | 1.165 s | 1.9% | No |

The four retained cases improved in all six paired rounds. Paired median
reductions were 27.0%, 27.7%, 33.3% and 14.4%, respectively. Lighting and
Isometric had small, inconsistent gains, so their API coverage stays as before.
No-op builds stayed around 0.06 s. Engine and external libraries were already
built during the game timings; unchanged source flags and optimization apply
to both cases.

Broader PCHs also have a preparation and memory cost. Recreating the **game
PCH, objects and executable**, with engine/dependencies built, gave these
medians over three cold samples per case:

| Retained game | Common game PCH | Broader API PCH |
|---|---:|---:|
| Charts | 4.596 s | 4.675 s |
| Text Edit | 3.924 s | 4.597 s |
| Shapes | 5.298 s | 4.949 s |
| ECS Parallel | 7.875 s | 8.060 s |

Cold time did not improve consistently; Text Edit's preparation was about 17%
slower. PCH creation can also use more memory even when warm source compilation
uses less. Editing a precompiled engine API header invalidates the affected
game PCH. Choose stable APIs used during a game's edit loop rather than adding
all available headers. Existing custom PCHs and PCH-off builds keep their
configuration; no compiler or CMake requirement changed.

The [raw samples, profiles and metadata](build_broader_pch_2026-11-08.json)
and [reproduction scripts](../experimental/pch/README.md) distinguish these
scopes and the rejected variants. All 60 registered game checks and twelve
Isometric smoke runs passed during the comparison. Final configuration omits
engine headers already covered transitively by the common game PCH.
Background host load and CPU frequency varied, so these are one-host Clang
measurements rather than predicted gains on every compiler or game.

Final validation rebuilt the normal native Clang/PCH configuration and passed
its 120-test 2D suite (118 executed, two GPU checks skipped under SDL dummy).
GCC 15 compiled the seven real sampled sources using five final PCHs, with
PCH use confirmed and invalid-PCH warnings treated as errors. That is a compile
smoke check, not a GCC performance benchmark.


## Private widget implementation PCH

When editing UI2's widget implementations, enable the optional private cache:

```sh
cmake --preset dev-clang -DKIN_ENABLE_WIDGET_PCH=ON
cmake --build --preset dev-clang --target my_game
```

`KIN_ENABLE_WIDGET_PCH` defaults to **OFF** and also requires `KIN_ENABLE_PCH`.
It groups six implementation files that already include
`ui2/widget_chunk_preamble.hpp` into an object target with its own PCH. Their
objects still enter `libkin_engine`, with the engine's private options,
definitions and dependency usage requirements. The remaining engine sources
keep their existing PCH. No C++ implementation or public API changed.

The opt-in setting fits repeated widget `.cpp` edits. Leave it off when the
widget headers change frequently or when prioritizing cold cache preparation.
An isolated source-to-object pilot showed 32–62% shorter compiler times, but the
real edit loop also rebuilds the engine archive and links the game. Four
alternating rounds on native Linux Clang 21.1.8, Release `-O2`, four workers
pinned to four CPUs, no compiler cache or LTO and the default GNU linker gave
these median **edit-to-Hex-link** times:

| Implementation edit | Common engine PCH | Private widget PCH | Reduction |
|---|---:|---:|---:|
| widgets_editor.cpp | 3.967 s | 2.869 s | 27.7% |
| widgets_core.cpp | 4.508 s | 3.689 s | 18.2% |
| widgets_inputs.cpp | 2.831 s | 1.622 s | 42.7% |

All three improved in each of the four paired rounds. Warm compiler-process
RSS fell from roughly 212–226 MiB to 169–189 MiB. No-op builds stayed around
0.08 s. These gains do not apply to unrelated game or engine source edits.

The extra precompilation has a cost:

| Rebuild through Hex link | Common engine PCH | Private widget PCH | Samples per case |
|---|---:|---:|---:|
| Six widget objects plus their private PCH, if present | 8.373 s | 9.437 s | 4 |
| Private preamble comment edit | 7.663 s | 9.378 s | 4 |
| Public widgets header comment edit | 62.247 s | 60.818 s | 3 |

Cold widget preparation was about 13% slower and private-preamble invalidation
about 22% slower, despite lower aggregate CPU work. PCH creation raised maximum
individual-process RSS to about 356 MiB. Public-header rebuild wall and CPU
times varied without a clear improvement. Those samples changed comments,
not API declarations. The common engine PCH, unrelated objects, component
libraries and dependencies stayed built during these measurements.

A separate single cold-engine sanity pair recreated `kin_engine`'s objects,
PCH/archive and Hex link, with foundation/scripting/audio/physics libraries,
shaders and external dependencies retained. It took 219.60 s versus 226.66 s
(+3.2%). One pair is insufficient for a general cold-build claim; the repeated
local invalidation costs above are why the option stays off by default.

The [raw samples and scope metadata](build_widget_pch_2026-11-08.json) and
[reproducer](../experimental/pch/README.md#private-widget-pch) retain the source
pilot, integrated comparison and cold-engine check separately. All eight
comparison headless checks passed. The final native 2D/PCH build passed 119
tests with two GPU checks skipped under SDL dummy. GCC 15 also built the actual
private PCH and all six widget objects.

The build contract checks that each widget source keeps the normal engine's
compiler options, definitions and include classifications, and that widget
headers stay out of the common engine PCH. Clang and Windows MSVC CI enable the
real widget group as well. Clang's cached CI build uses the
[ccache 4.9 PCH settings](https://ccache.dev/manual/4.9.html#_precompiled_headers);
performance measurements disable compiler caching. Host load and CPU frequency
varied, so remeasure on the compiler and machine used for development.
