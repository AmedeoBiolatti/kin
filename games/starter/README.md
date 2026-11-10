# Kin starter

One native 2D scene with a square moved by the arrow keys. Escape or closing the
window quits. It uses `run_scene_app` for the normal window, fixed-step loop and
headless report; no game assets are needed.

## Inside Kin

Reuse your compatible development build. From the Kin repository root:

```sh
cmake --preset dev
cmake --build --preset dev --target starter
./out/build/dev/bin/starter
ctest --preset dev -R '^starter-smoke$'
```

On Windows, use a Visual Studio developer prompt with Ninja installed, and run
`out/build/dev/bin/starter.exe`.

## As a standalone game

Copy this directory, including `.gitignore` and `AGENTS.md`, into your game's repository. The small
agent entry point refers to the shared Kin workflow so copied games keep the same
conventions. From that directory, configure
one build with a local Kin checkout:

```sh
cmake -S . -B out/build/dev -G Ninja -DCMAKE_BUILD_TYPE=Release -DKIN_DIR=/path/to/kin
cmake --build out/build/dev --target starter --parallel 4
./out/build/dev/starter
ctest --test-dir out/build/dev -R '^starter-smoke$' --output-on-failure
```

The standalone executable is at `out/build/dev/starter` (`starter.exe` on
Windows); Kin is added as a subproject, so its other games and engine tests stay
disabled by default. The included `.gitignore` keeps `out/` untracked. Reuse the same
build for edits; choose another directory only for a configuration that must
coexist. A multi-config generator also needs `--config Release` and `ctest -C
Release`, and puts the executable under `Release/`.

Rename the CMake project/target, window title and test as the game takes shape.
Add content when needed: remove `NO_CONTENT` from `kin_game` and use
`kin::find_content_root(KIN_GAME_CONTENT, argc, argv)` to locate it. Kin's
`docs/project_workflow.md` describes growth and current feature references.
Keep shared runtime/build behavior in Kin and game rules in this project.
