# Contributing to Kin

Thanks for your interest in kin. Bug reports, fixes, docs, and focused features
are all welcome.

## Before you start

- For anything beyond a small fix, open an issue first so we can agree on the
  approach. Kin grows incrementally and deliberately; [the manifest](docs/manifest.md)
  describes its scope and non-goals.
- Check the [docs](docs/) for the subsystem you're touching; they describe the
  intended APIs and workflows.
- Follow the [project workflow](docs/project_workflow.md): reuse the closest
  implementation, keep branch scope explicit, and add structure for present needs.

## Building and testing

See [Building](README.md#building) in the README. For normal development, reuse
one build directory and build only the targets affected by your change:

```sh
cmake --preset dev
cmake --build --preset dev --target kin_scene_tests
ctest --preset dev -R '^kin-scene$'
```

Choose relevant existing suites and add focused regression coverage for behavior
changes. Documentation-only edits do not need compilation. Broaden validation
for shared contracts, build configuration, cross-module changes or uncertainty;
full local suites are useful for integration and release work. CI runs the full
platform matrix before merge. See [the agent workflow](AGENTS.md) for output and
validation conventions shared by automated contributors.

The required checks are `Linux GCC`, `Linux Clang`, `Windows MSVC` and
`Linux GPU (lavapipe)` (a job that passes when the Release and `render3d` GPU
jobs, which split that work, both pass). CI also checks formatting, the 2D-only build, Debug
lavapipe coverage and Linux/Windows shipping builds. The builds cache compiles
with ccache (a pull request reuses the cache `main` last saved, so the first
build after a toolchain or dependency change is slow), and are skipped when a
change only touches documentation.
[docs/testing.md](docs/testing.md) covers the test layout, the test profiles CI
uses and how to keep tests fast and deterministic.

## Code guidelines

- C++23, four-space indentation, and the style of the surrounding code:
  `PascalCase` types, `snake_case` functions and variables, `_prefixed` private
  members, and everything in the `kin` namespace.
- Public headers live in `engine/include/kin/<module>/`, sources in
  `engine/src/<module>/`. Keep modules independent: a subsystem should be usable
  and testable on its own.
- Extend existing suites for meaningful behaviour changes and regressions. Add
  another executable only when dependencies, isolation or configuration require
  it. Prefer pure engine tests;
  create SDL windows only when the behaviour depends on them.
- Headless runs must stay deterministic: fixed timestep, explicit seed, no wall
  clock or unseeded randomness in simulation code.
- Update the relevant `docs/` page and add a line under `[Unreleased]` in
  [docs/changelog.md](docs/changelog.md) for user-visible changes.

## Pull requests

- Keep each pull request to one logical change, with a description of what it
  does and why.
- Don't commit build output, local editor/agent settings, or assets you don't
  have the rights to publish under the MIT License.

By contributing, you agree that your contributions are licensed under the
[MIT License](LICENSE), the same license as kin.

## Code of conduct

Everyone taking part is expected to follow the
[Code of Conduct](.github/CODE_OF_CONDUCT.md).
