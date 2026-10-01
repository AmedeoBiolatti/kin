# Scripting

Kin runs Lua 5.4 (through sol2) in two hosts:

- **`ScriptScene`** (`kin/scripting/script_scene.hpp`): a scene written in Lua.
  The script gets the scene's ECS world, input and rendering, and can register
  ECS systems ([systems](ecs_systems.md#registering-lua-systems)).
- **`LuaScript`** (`kin/scripting/lua_script.hpp`): a script for code that does
  not live in the ECS, such as a game's rules, AI, balance formulas or level
  logic, run by plain C++. It knows nothing of scenes or entities: the host
  binds what a script may ask and do, and calls the functions it defines. This
  page is about `LuaScript`.

## A rules script

```cpp
#include <kin/scripting/lua_script.hpp>

Base* current = nullptr; // the base being told, during a call
kin::LuaScript rules{{
    .setup = [&](sol::state& lua) {
        sol::table game = lua.create_named_table("game");
        game["give"] = [&](const std::string& item, int count) {
            if (current) current->give(item, count);
        };
        game["flag"] = [&](const std::string& name) { return current ? current->flag(name) : 0; };
    },
    .module_root = root / "scripts",
}};
rules.load_file(root / "scripts/rules.lua");

// When something happens: run the handler if the script defines one.
current = &base;
rules.call("on_built", "pump", q, r);
current = nullptr;

// A formula the designers tune in Lua.
const int cost = rules.call_for<int>("upgrade_cost", level).value_or(100);
```

```lua
-- scripts/rules.lua
local tables = require("tables") -- scripts/tables.lua

function on_built(item, q, r)
    if item == "pump" and game.flag("first_pump") == 0 then
        game.give("water", 10)
    end
end

function upgrade_cost(level)
    return tables.base_cost * level * level
end
```

- `setup` binds the host's API. It runs on every fresh state, before the
  script, so bindings survive reloads. Functions that act on game state usually
  reach it through a pointer the host sets around each call, as above.
- `call(name, args...)` runs a global function if the script defines one and
  returns whether it ran without error; `call_for<R>(...)` also returns its
  first result as an `R`. A handler the script does not define is not an error.
- `lua()` is the live `sol::state`, for anything this API does not cover.

## Errors

Nothing a script does stops the game. A load or call that fails returns false,
and `error()` holds the message with the script's name and line
(`rules.lua:12: attempt to index a nil value`). Each failure goes to
`LuaScriptOptions::on_error`, or is logged as a warning under `script`.

A load runs the script in a **fresh** state, which replaces the loaded one
only if it parses and runs; otherwise the previous script stays, handlers and
all. So a broken edit never leaves a game without its rules.

## The sandbox

Scripts get Lua's base, math, table, string and utf8 libraries, and nothing
that reaches outside the script or makes up numbers:

- no `io`, `os`, `debug`, `package`, `load`, `loadfile`, `dofile` or
  `collectgarbage`, and no `math.random` / `math.randomseed`;
- `require("a.b")` loads `a/b.lua` from `module_root` only, once per state;
  without a `module_root` there is no `require`;
- `print` writes to kin's log (category `script`).

With the same script and the same calls, a run comes out the same, so scripted
rules can run inside a deterministic simulation. One trap remains: `pairs`
walks a table in no fixed order, so use `ipairs`, or sort keys, where order
matters. A host that wants randomness should bind its own, seeded from the
run's `RngKey`.

Every load and call has an **instruction budget**
(`LuaScriptOptions::instruction_limit`, 50 million by default, checked every
1,000 instructions; 0 turns it off). A script stuck in a loop fails that call
with "instruction limit reached" instead of freezing the game; the next call
gets a fresh budget.

## Hot reload

```cpp
kin::FileWatcher files;
rules.watch(files);           // the script and every module it required
config.file_watcher = &files; // run_scene_app polls between frames
```

An edit to the script or any module it required reloads it, keeping the last
good version if the new one fails; fixing the file reloads it. The watcher and
the script may be destroyed in either order. See
[hot-reloading game data](assets.md#hot-reloading-game-data).
