# Saving

What a game keeps is marked on its entities, in scene files like any component:
`Saved` (`kin/ecs/saving.hpp`). `SceneSaves` keeps it while the player is in
another scene, and writes it to save slots and reads it back (kin's `SaveStore`).

```json
{"id": "coin_3", "name": "Coin", "components": {
  "Transform2D": {"pos": {"x": 420, "y": 300}},
  "Saved": {"keep": ""}}}
```

**`Saved`** has two fields:

- **`keep`:** the components kept, by name, commas between (`"Transform2D, Health"`).
  It defaults to `"Transform2D"`. Empty keeps nothing but this: once destroyed,
  the entity stays destroyed (a coin taken, a door opened for good).
- **`script`:** its script's own values are kept too, those it put in its `self`.
  It defaults to true.

All of a kept component's fields are kept, except those the game works out
(read-only fields, such as a body's handle). A kept component the entity no
longer has is taken away again when it comes back.

Scene entities are known again by their ids in the scene file. An entity of a
prefab instance is known as `instance/entity`. Renaming entities is safe;
changing their ids loses what was kept of them. Prefab instances made while the
game runs are kept with the prefab they were made from (`SpawnedPrefab`, which
a script's `world.spawn` gives them), and made again.

## In the game

```cpp
kin::SceneSaves saves{{.game_id = info.id, .game_version = info.version}, content};
scripts.set_saves(&saves); // the scripts' save table, and their kept values

// A scene read (the first, one travelled to, or one read again):
saves.enter(world, "scenes/cave.kinscene");
// Before the scene goes for another:
saves.keep(world);
// Each step:
saves.played(ctx.dt);

// After each step: a read a script asked for.
if (std::optional<std::string> slot = saves.take_read()) {
    std::string error;
    if (std::optional<std::string> scene = saves.read(*slot, error)) {
        load_scene(*scene);
        saves.enter(world, *scene);
    } else {
        kin::log_error(error);
    }
}
```

- **`enter`** puts back what was kept of the scene: its entities as they were,
  those destroyed destroyed again, those made made again. It notes the scene's
  entities marked `Saved`, so that `keep` knows which have gone since.
  `problems()` lists what could not be put back (a component or field renamed
  since, a prefab gone).
- **`keep`** keeps what the scene's marked entities are now.
- **`write(world, slot, title[, summary])`** keeps the scene, then writes a slot:
  the scene the player is in, the game's own values, and every scene kept.
- **`read(slot, error)`** puts a slot's scenes and values in place of these, and
  returns the scene to read. The game reads it, then enters it.
- **`value(name)` and `set_value(name, value)`** are the game's own values (a
  score, the keys found). They go from scene to scene and into the slots.
- **`played(seconds)`** adds to the time played, which each slot keeps
  (`play_time_seconds`) and `read` gives back.
- **`clear()`** forgets the scenes kept, the values and the time played, for a new game.
- **`store()`** is the `SaveStore`: list, delete or check slots.

Moving an entity with a physics body puts its body there too, at the next
physics step (`PhysicsTeleport`).

The slot's payload is JSON (schema `kin.scene_saves/1`):

```json
{"schema": "kin.scene_saves/1", "scene": "scenes/cave.kinscene",
 "values": {"keys": 2},
 "scenes": {"scenes/cave.kinscene": {
   "entities": {"coin_3": {"gone": true},
                "player": {"Transform2D": {"pos": {"x": 80, "y": 412}, "rotation": 0, "scale": {"x": 1, "y": 1}},
                           "script": {"coins": 7}}},
   "spawned": [{"prefab": "prefabs/crate.kinprefab", "Transform2D": {"pos": {"x": 300, "y": 400}}}]}}}
```

## In scripts

Given the saves (`EntityScripts::set_saves`), scripts have a `save` table:

```lua
function on_contact(self, other)
    if other:name() == "Checkpoint" then
        save.set("checkpoint", other:name())
        local ok, why = save.write("auto", "At the checkpoint")
    end
end

function on_update(self, dt)
    if input.pressed("load") and save.exists("auto") then
        save.read("auto") -- the game reads it after this step
    end
end

function on_load(self)
    -- self.coins and the rest are the saved ones again
end
```

- `save.write(slot[, title[, summary]])` returns true, or nil and why.
- `save.read(slot)` asks the game to read the slot after the step. It returns
  true, or false and why (no such slot).
- `save.exists(slot)`, `save.delete(slot)`.
- `save.slots()` returns `{id, title, summary, updated}` tables, newest first.
- `save.get(name[, default])` and `save.set(name, value)` are the game's own
  values: true/false, numbers, text and tables of them. nil takes one away.

An entity marked `Saved` with `script` keeps the values in its `self`: true/false,
numbers, text and tables of them. Functions and entities are not kept. When it
comes back, its script starts as usual. The kept values replace those `on_start`
set, and then `on_load(self)` runs, if the script has one.
