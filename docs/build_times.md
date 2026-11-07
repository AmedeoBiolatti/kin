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
