# Broader PCH measurements

Native Linux Clang/Ninja probes used for the [broader PCH report](../../docs/build_times.md#broader-standard-library-and-game-pchs).
The recorded toolchain is Clang 21.1.8, libstdc++ 15, CMake 4.2.3 and Ninja
1.13.2. These scripts are measurement tools outside the normal build.

Use a clean, disposable worktree and an exclusive build directory on a native
Linux filesystem. Run no other editor, configure or build against those inputs
while measuring. Python 3, `taskset` and `/usr/bin/time` are required.

From the checkout used as `--source`:

```sh
cmake --preset dev-clang -DKIN_COMPILER_CACHE=off \
    -DCMAKE_C_COMPILER_LAUNCHER= -DCMAKE_CXX_COMPILER_LAUNCHER=
cmake --build build-dev-clang --parallel 4 --target \
    charts_demo text_edit_demo shapes_demo lighting_demo \
    ecs_parallel_bench isometric_demo
python3 experimental/pch/bench_engine.py --source . --build build-dev-clang \
    --out /tmp/kin-pch-engine-results
python3 experimental/pch/bench_games.py --source . --build build-dev-clang \
    --out /tmp/kin-pch-game-results
```

Use new, empty output directories. The scripts pin work to the first four
available CPUs, disable ccache and report wall time, CPU time and maximum
individual-process RSS. They do not control other host workloads or CPU
frequency. Keep normal Release optimization, LTO off and the project-pinned
dependencies. A new run will have different absolute times.

`bench_engine.py` copies the generated Clang PCH inputs and compiler arguments
for the engine target into its output directory. It prepares
three PCH variants there: the previous standard list, that list plus
`filesystem`, and that list plus `filesystem` and `chrono`. Explicit instances
of those two headers are removed before constructing the control, so the
script also works after the production PCH has gained `filesystem`. Source
compiles use these private PCHs and write private objects; the source checkout
and configured build outputs stay untouched. All six variant orderings are
rotated, producing six samples per source/case. PCH creation is recorded
separately once per target/case. These are source-to-object measurements, with
no final executable link or full clean engine build.

`bench_games.py` measures six real game targets. A deferred CMake hook sets
either the previous common game PCH or that PCH plus the game's included engine
APIs. The control overrides the final target additions as well. It alternates
case order and reverses game order on alternating rounds, producing six warm
edit/link and no-op samples, and three cold samples per game/case. Cold samples
remove only that game's PCH, objects and executable; engine libraries,
dependencies and compiled shader inputs remain built. The script runs five
existing game checks plus Isometric's `--smoke` after each case visit.

The game script **temporarily edits six main sources** by appending comments,
reconfigures the supplied build, and removes that game's generated build
artifacts for cold samples. Its `finally` restores source bytes/timestamps,
clears its CMake hook and settles the normal game targets. It refuses a dirty
tracked source tree or an existing top-level CMake hook. Its process guard is
not a cross-process lock: the disposable worktree/build must remain exclusive.
After a hard kill, restore the six main sources in that disposable worktree and
reconfigure with `-DCMAKE_PROJECT_TOP_LEVEL_INCLUDES= -U KIN_BROAD_PCH_BENCH`.

The measured base is `ac7b57e858a6807289143b960a8078f02235e10b`. Final game
configuration omits headers already covered transitively by the common PCH;
the experiment includes the game's explicit engine headers, including those
redundant includes. Lighting and Isometric are measured negative/control
cases: their broader API PCHs were not retained. Public engine headers remain
outside engine PCHs, and game-owned headers remain outside game PCHs.


## Private widget PCH

`bench_widgets.py` compares `KIN_ENABLE_WIDGET_PCH=OFF` and `ON` with the common
PCH enabled. Use the same native Clang/Ninja setup above, a clean disposable
worktree, an exclusive build and a new empty output directory:

```sh
python3 experimental/pch/bench_widgets.py --source . --build build-dev-clang \
    --out /tmp/kin-widget-pch-results
```

The script temporarily appends comments to three widget implementations, the
private widget preamble and the public widgets header. It alternates option
order across four rounds and measures three source edits, no-op, cold widget
cache and private-header invalidation four times per case, plus public-header
invalidation three times per case. Each build ends at Hex's executable link.
A cold widget sample removes only the six widget objects and, when enabled,
the widget PCH. Other engine objects, the common engine PCH, component libraries,
shaders and dependencies remain built. Public-header comments invalidate a
wider set of consumers; declarations remain unchanged.

The `finally` restores source/header contents and timestamps, restores the
starting option, removes the widget PCH and objects to avoid stale input
metadata, and settles the normal Hex target. Do not run an editor or another
configure/build against these inputs. After a hard interruption, restore the
three widget `.cpp` files and two headers in the disposable worktree; remove
the generated widget PCH and reconfigure/build. The script is specific to native
Clang `.pch`/`.o` outputs. It is not a Windows or GCC performance benchmark.
