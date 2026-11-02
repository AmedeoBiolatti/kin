#pragma once

// What a game keeps, as data. An entity marked Saved keeps the components it
// names, and its script's own values (its `self`'s), while the player is in
// another scene and in the save slots the game writes; one destroyed stays
// destroyed. SceneSaves keeps them: what each scene the player left was, the
// game's own values (a script's save.set), and the slots (kin's SaveStore):
//
//     kin::SceneSaves saves{{.game_id = "my_game"}, content};
//     // a scene read (the first, or one travelled to, or read again):
//     saves.enter(world, "scenes/cave.kinscene");
//     // each step:
//     saves.played(ctx.dt);
//     // before it goes for another:
//     saves.keep(world);
//     // a save, and reading it back (a script's save.write and save.read):
//     saves.write(world, "slot_1", "In the cave");
//     if (std::optional<std::string> slot = saves.take_read()) {
//         std::string error;
//         if (std::optional<std::string> scene = saves.read(*slot, error)) {
//             // the scene read from *scene, then
//             saves.enter(world, *scene);
//         }
//     }
//
// Scene entities are known again by their ids in the scene file (an entity of
// a prefab instance: "instance/entity"), so renaming them is safe and changing
// their ids is not. Prefab instances made while the game runs (a script's
// world.spawn) are kept with the prefab they came from (SpawnedPrefab), and
// made again. What is kept of a component is all its fields; a component
// kept that the entity no longer has is taken away again.

#include <kin/core/json_value.hpp>
#include <kin/ecs/world.hpp>
#include <kin/save/save_store.hpp>

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace kin {

// What an entity keeps.
struct Saved {
    // The components kept, by name, commas between ("Transform2D, Health").
    // None: only that it is gone, once destroyed.
    std::string keep = "Transform2D";
    bool script = true; // its script's own values (those of its self)
};

// A prefab instance made while the game ran: the prefab (a content path) it
// is made again from. World.spawn gives it to the instances it makes.
struct SpawnedPrefab {
    std::string prefab;
};

// The components a Saved names, in its order (blanks trimmed, empty names dropped).
std::vector<std::string> saved_components(const Saved& saved);

// What a script keeps of its entity and is given back (EntityScripts gives
// them to the saves it is told of).
struct SavedScripts {
    // Its entity's values (none: nothing to keep).
    std::function<std::optional<JsonValue>(EcsEntity)> write;
    // Given back: they are its self's again once it starts (on_load says so).
    std::function<void(EcsEntity, const JsonValue&)> read;
};

class SceneSaves {
public:
    // Its slots in `config`'s store; `content` is what prefab paths are from.
    SceneSaves(SaveStoreConfig config, std::filesystem::path content);

    // The scene `scene` (as the content names it: "scenes/cave.kinscene") is
    // read: what was kept of it is put back (the entities destroyed then
    // destroyed now, those made made again), and its entities marked Saved noted.
    void enter(EcsWorld& world, std::string scene);
    // What its entities marked Saved are now, kept (it is going, or a save is written).
    void keep(EcsWorld& world);
    const std::string& scene() const { return _scene; }

    // The game's own values, kept with the scenes (a script's save.get and
    // save.set); null takes one away.
    const JsonValue* value(std::string_view name) const;
    void set_value(const std::string& name, JsonValue value);
    const JsonValue::Object& values() const { return _values; }

    // Time played (the game adds each step's), written with each slot and read back.
    void played(f64 seconds) { _played += seconds; }
    f64 play_time() const { return _played; }

    // A new game: the scenes kept, the values and the time played forgotten.
    void clear();

    // What was kept of a scene, if any:
    // {"entities": {"coin_1": {"gone": true}, "player": {"Transform2D": {...}, "script": {...}}},
    //  "spawned": [{"prefab": "prefabs/crate.kinprefab", "Transform2D": {...}}]}
    const JsonValue* kept(std::string_view scene) const;

    // Written to a slot (the scene kept first): its scene, values and the
    // scenes kept. `summary` is a line of the game's for listings.
    SaveResult write(EcsWorld& world, std::string_view slot, std::string title, std::string summary = {});
    // A slot read, its scenes and values in place of these: the scene it was
    // written in, to read (then enter); none, and `error` says why, when it can't be.
    std::optional<std::string> read(std::string_view slot, std::string& error);

    // A read asked for (by a script, mid-step): taken once by the game, which reads it.
    void ask_read(std::string slot) { _asked = std::move(slot); }
    std::optional<std::string> take_read();

    SaveStore& store() { return _store; }
    const SaveStore& store() const { return _store; }

    void set_scripts(SavedScripts scripts) { _scripts = std::move(scripts); }

    // What went wrong putting kept things back at the last enter (a component
    // or prefab no longer there, a field that doesn't read); empty: nothing.
    const std::vector<std::string>& problems() const { return _problems; }

private:
    JsonValue entity_state(EcsWorld& world, EcsEntity entity, const Saved& saved) const;
    void put_back(EcsWorld& world, EcsEntity entity, const JsonValue& state);

    SaveStore _store;
    std::filesystem::path _content;
    std::string _scene;
    std::set<std::string> _noted; // the scene's Saved entities as entered, by authored id
    std::map<std::string, JsonValue, std::less<>> _scenes;
    JsonValue::Object _values;
    std::optional<std::string> _asked;
    f64 _played = 0.0;
    SavedScripts _scripts;
    std::vector<std::string> _problems;
};

} // namespace kin
