# Faster development builds

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
