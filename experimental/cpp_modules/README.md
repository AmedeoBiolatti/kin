# C++ modules pilot

This standalone project compares the real Hex Demo game body using ordinary
headers, a broader PCH, and `import kin.runtime`. It is deliberately outside
Kin's normal build. The measured facade did not beat a PCH covering the same
API; see [results](../../docs/build_times.md#cpp-modules-pilot).

The tested toolchain is native Linux Clang 21.1.8, CMake 4.2.3 and Ninja 1.13.2.
The project requires Clang 21+, CMake 3.28+ and Ninja 1.11+; it does not establish
support for Windows, AppleClang, GCC or Emscripten. Kin's normal compiler and
CMake requirements remain unchanged.

From the repository root:

```sh
cmake -S experimental/cpp_modules -B build-cpp-modules -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build-cpp-modules --parallel 4 \
    --target hex_cpp_headers hex_cpp_matched hex_cpp_modules
SDL_VIDEO_DRIVER=dummy SDL_AUDIO_DRIVER=dummy \
    ctest --test-dir build-cpp-modules --output-on-failure
```

The three clients use copies under `build-cpp-modules/clients`. CMake generates
these at configure time; edit those copies to measure a game-source change
without editing the checkout, and build just that client's target. The matched
client precompiles the eleven engine headers included by the facade. The module
client has a standard-library PCH plus the facade. All clients link the same
ordinary engine libraries and run the same deterministic headless game check.
Compiler caching and LTO default to off. The ordinary-header client uses Kin's
common game PCH; the normal Hex target's five extra PCH headers are not applied
to it.

`kin_runtime.cppm` includes the existing headers in its global module fragment
and exports selected names through using-declarations. The native ABI stays in
the existing libraries. This measures a gradual migration at the game API; it
is not a modularized engine implementation or a claim about the maximum benefit
of a fully modular design. The export list only covers this game's API. Macros
are not exported, and new game APIs need explicit exports. If Hex's include
layout changes, review the generated clients as well as their test output.
`-DKIN_MODULE_EXPERIMENT_REDUCED_BMI=ON` enables a Clang-only follow-up variant;
it did not improve the measured warm-edit result.

## Reproduce the recorded comparison

The published timings were collected before moving the pilot into this
standalone project. They use checkpoint
`baa5621032100adeb2fd48d84eb3170456ae5640`, based on
`5eb0c06ce78481002b655902ece812f3910f3672`. The checkpoint temporarily adds module
plumbing to Hex Demo; subsequent commits remove it from the normal build.
The standalone project's build-directory layout differs, so do not label its
new measurements as the recorded run.

Use a clean, disposable native Linux worktree for the checkpoint. Run the script
from this final checkout, supplying the checkpoint worktree and its own build
and output directories:

```sh
git worktree add --detach /tmp/kin-modules-snapshot \
    baa5621032100adeb2fd48d84eb3170456ae5640
cmake --preset dev-clang -S /tmp/kin-modules-snapshot \
    -B /tmp/kin-modules-snapshot-build -DKIN_COMPILER_CACHE=off \
    -DCMAKE_EXE_LINKER_FLAGS= -DCMAKE_SHARED_LINKER_FLAGS= \
    -DCMAKE_MODULE_LINKER_FLAGS=
cmake --build /tmp/kin-modules-snapshot-build --parallel 4 --target hex_demo
python3 experimental/cpp_modules/bench_snapshot.py \
    --source /tmp/kin-modules-snapshot \
    --build /tmp/kin-modules-snapshot-build \
    --out /tmp/kin-modules-snapshot-results
```

The script requires Python 3, Linux `/proc`, `taskset` and `/usr/bin/time`.
It changes the disposable worktree's main source and pathfinding source/header,
removes only that game's PCH/BMI/object/link outputs for cold samples, and
restores source contents and timestamps in `finally`. Run no other configure,
build or editor against those inputs while it runs. The script checks for owned
compiler/build processes before mutations, but this is not a cross-process lock.
It rotates all six orderings, collects twelve warm-edit/no-op samples and three
cold/native/header samples per case, and runs a headless check after each visit.
An API-header sample adds a comment: declarations remain unchanged. Cold samples
retain built engine libraries and dependencies. Final cleanup explicitly
invalidates the game PCH/BMI after restoring header timestamps.

For the smaller production PCH change, `bench_pch.py` runs twelve samples per
case against a clean checkout containing the final five-header addition. Its
control reconstructs the original common game PCH. Configure and build that
checkout with `dev-clang` first, then pass the same `--source`, `--build` and
`--out` arguments. It temporarily edits only the main source and restores it.
Neither script measures a full clean engine build or runtime performance.
