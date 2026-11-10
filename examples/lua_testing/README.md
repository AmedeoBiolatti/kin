# Lua authoring and tests

Build `kin_lua`, then run this project without a window:

```sh
build/bin/kin_lua test --root examples/lua_testing/content \
  --json build/lua-tests.json --junit build/lua-tests.xml \
  examples/lua_testing/tests/project.lua
```

Each test gets a fresh VM, ECS world, input and writable copy of the content.
The example covers pure modules, assertions, input, deterministic time, tasks,
entity and scene reload, rejected edits, and seeded random values.

Generate definitions for entity scripts:

```sh
python3 tools/kin_lua.py --native build/bin/kin_lua --host entity \
  generate --output build/lua-api/entity
python3 tools/kin_lua.py --native build/bin/kin_lua --host entity \
  check examples/lua_testing/content/actor.lua
```

See [Lua development](../../docs/lua_development.md) for editor configuration,
runtime limits, profiling and the task lifecycle.
