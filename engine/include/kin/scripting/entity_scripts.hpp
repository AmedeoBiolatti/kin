#pragma once

// Runs the scripts on a world's entities (ScriptComponent, kin/ecs/script.hpp):
// each script file in a sandboxed Lua state of its own (LuaScript's: no files,
// no clock, an instruction budget), shared by the entities running it, each
// with a `self` table of its own. Each update an entity whose script has read
// gets on_start(self) once, then on_update(self, dt), and on_contact(self,
// other) for each body its body began touching (with a PhysicsContactQueue).
// A script whose file changed is read again; its entities start over (a
// broken edit keeps the last good one running). Nothing a script does stops
// the game: an error is logged, kept in problems(), and that call skipped.
//
//     kin::EntityScripts scripts{{.content = kin::content_root()}};
//     // each frame, after physics
//     scripts.update(world, ctx.input, ctx.dt);
//
// In a script, `self` has the entity's methods and keeps what the script puts
// in it (self.speed = 160). An entity (self, or one found) has:
//   id(), name(), alive(), parent(), child(name)
//   get(component) -> a table of its fields, or nil; set(component, {field = value, ...})
//     (adds it when missing); has(component), add(component), remove(component)
//   position() -> x, y; set_position(x, y); move(dx, dy); rotation(); set_rotation(degrees)
//   destroy()
//   play(animation), set_param(name, true|false|number), trigger(name), state()  (its Animator's)
// and the script has:
//   input.held(action), input.pressed(action), input.released(action), input.axis(name),
//   input.mouse() -> x, y
//   world.find(name) -> an entity or nil; world.spawn(prefab, x, y) -> its root, or nil
// and, given the scene's screens (set_screens), its UI's elements by id:
//   ui.clicked(id), ui.changed(id), ui.value(id), ui.checked(id), ui.text(id),
//   ui.set_text(id, text), ui.set_value(id, v), ui.set_checked(id, b), ui.show(id[, b]),
//   ui.hide(id), ui.set_image(id, path), ui.set_enabled(id, b)
// and, given the scene's dialogue (set_dialogue): dialogue.start(path[, node]),
// dialogue.stop(), dialogue.active(), dialogue.advance(), dialogue.choose(i or id),
// dialogue.node(), dialogue.get(name), dialogue.set(name, value); and each
// entity's on_dialogue_event(self, id, node) for what the dialogue told
// (its events, "dialogue.started", "dialogue.ended") this step.
// Fields read and set as scene files have them: numbers, true/false, text,
// {x = , y = } pairs and {r = , g = , b = , a = } colours.

#include <kin/core/types.hpp>

#include <filesystem>
#include <map>
#include <memory>
#include <string>

namespace kin {

class EcsWorld;
class Input;
class SceneScreens;
class SceneDialogue;

struct EntityScriptsConfig {
    std::filesystem::path content;  // the scripts (and the prefabs they spawn) are read from here
    i32 check_frames = 30;          // script files changed are read again every so many updates (0: never)
};

class EntityScripts {
public:
    explicit EntityScripts(EntityScriptsConfig config);
    ~EntityScripts();
    EntityScripts(const EntityScripts&) = delete;
    EntityScripts& operator=(const EntityScripts&) = delete;

    void update(EcsWorld& world, Input& input, f32 dt);
    // Scripts that could not be read or failed a call, by path, with why
    // (script:line). Gone once one reads and runs again.
    const std::map<std::string, std::string>& problems() const;
    // Entities running a script now.
    std::size_t running() const;
    // Forgets every entity's self (all start over at the next update).
    void restart();
    // The scene's UI screens, for the scripts' `ui` (null: none).
    void set_screens(SceneScreens* screens);
    // The scene's dialogue, for the scripts' `dialogue` and on_dialogue_event (null: none).
    void set_dialogue(SceneDialogue* dialogue);

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace kin
