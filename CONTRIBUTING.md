# Contributing to Kin

Thanks for your interest in kin. Bug reports, fixes, docs, and focused features
are all welcome.

## Before you start

- For anything beyond a small fix, open an issue first so we can agree on the
  approach. Kin grows incrementally and deliberately; [the manifest](docs/manifest.md)
  describes its scope and non-goals.
- Check the [docs](docs/) for the subsystem you're touching; they describe the
  intended APIs and workflows.

## Building and testing

See [Building](README.md#building) in the README. Before sending a pull request,
run the full suite:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure -j 8
```

CI builds and tests on Linux (GCC and Clang) and Windows (MSVC); a pull request
needs all three green. [docs/testing.md](docs/testing.md) covers the test layout
and how to keep tests fast and deterministic.

## Code guidelines

- C++23, four-space indentation, and the style of the surrounding code:
  `PascalCase` types, `snake_case` functions and variables, `_prefixed` private
  members, and everything in the `kin` namespace.
- Public headers live in `engine/include/kin/<module>/`, sources in
  `engine/src/<module>/`. Keep modules independent: a subsystem should be usable
  and testable on its own.
- Add or update a test for every behaviour change. Prefer pure engine tests;
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
