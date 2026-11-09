# Working in Kin

Keep changes, validation and generated output proportional to the task. Read the
relevant subsystem documentation before changing its contracts.

## Scope and structure

- Before implementation, identify the closest existing code/example, what can be
  reused, the smallest change that meets the request, and how to verify it. State
  the outcome and completion conditions briefly in the task update; small tasks
  do not need a planning document. Keep discovery targeted to relevant code and
  existing guidance; avoid repeating a whole-repository audit for a local task.
- Follow the current project's layout and dependencies. For a new game, use
  `games/starter` and `docs/project_workflow.md`. Start with one executable and
  `src/main.cpp`; create content, test and feature directories as they gain real
  contents. Larger projects grow from the same structure.
- Use direct functions and concrete types first. Add an abstraction, dependency,
  executable, configuration switch or architectural layer to solve a present
  problem; name that problem in the update or PR description. Extract shared
  behavior when actual callers benefit. Keep game-specific logic in the game.
- Split files around responsibilities that have become hard to understand or
  change. Avoid speculative managers, plugin systems, public-header trees and
  generic frameworks for imagined future requirements.
- Keep unrelated refactors, extra platforms and additional samples out of the
  branch unless required by the task. Once the requested behavior and relevant
  checks are complete, finish. Report unrelated findings without expanding scope.
- When branches overlap, record ownership in the existing task/PR description
  and coordinate shared interfaces before implementing competing helpers. Check
  existing work before starting a second implementation.
- Use the project guide's current references for implementation. Historical plans
  and measurements explain decisions but do not override current APIs or guides.

## Builds and output

- Start work without creating a build directory. Documentation and content-only
  tasks often need no compilation; reuse existing tools when suitable.
- Inspect existing build directories and their configuration before configuring.
  Reuse one compatible directory per worktree. For a new build of this repository,
  use `cmake --preset dev` (or the appropriate existing development preset); these use
  `out/build/<preset>/`. Do not duplicate a build for each sample or test.
- Request explicit targets: `cmake --build --preset dev --target hex_demo`.
  Development presets build tools, samples and tests on demand. Enable examples
  in the same build with `-DKIN_BUILD_EXAMPLES=ON` when needed.
- Create another build directory only for a configuration that must coexist,
  such as another compiler, platform or sanitizer. State the reason in the task
  update. Reconfigure compatible option changes in the existing directory.
- Keep generated binaries in the build tree, task captures/reports in
  `out/runs/<task>/`, and explicit package exports in `out/packages/` (packaging
  tools may stage packages inside the build tree). Use self-cleaning temporary
  directories for disposable experiments. Do not commit generated output.
- Share compiler caches across worktrees, not active CMake build directories.
  Keep the latest useful task evidence; retain older runs only for an active
  comparison. Check for valuable files and active processes before cleanup.

## Validation

- Choose checks from the changed behavior and its callers. Documentation-only
  edits need relevant document checks, not an engine build. Script/content edits
  usually need relevant script checks and a bounded sample smoke run.
- For C++ edits, build affected targets and run the relevant existing suites.
  Build test executables before invoking CTest; CTest filtering does not build
  them. Use anchored names, e.g. `ctest --preset dev -R '^kin-scene$'`.
- Broaden coverage for shared headers/contracts, build configuration, cross-module
  changes, failures or unresolved uncertainty. CI owns the full platform matrix;
  do not repeat it in every worktree. Full local validation remains appropriate
  for release/integration work or an explicit request.
- Extend existing test suites for meaningful behavior changes and regressions.
  Add a new executable only when dependencies, isolation or configuration require
  it. Avoid tests that merely mirror implementation or check incidental wording.
- Once relevant checks pass, rerun them only after changes that can affect the
  result or when a new failure/concern warrants it. Report what was checked and
  material gaps; do not describe focused validation as a full-suite pass.

See `docs/testing.md` for commands and `docs/build_times.md` for build settings.
