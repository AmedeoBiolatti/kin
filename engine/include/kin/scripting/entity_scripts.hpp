#pragma once

// Runs the scripts on a world's entities (ScriptComponent, kin/ecs/script.hpp):
// each script file in a sandboxed Lua state of its own (LuaScript's: no files,
// no clock, an instruction budget), shared by the entities running it, each
// with a `self` table of its own. Each update an entity whose script has read
// gets on_start(self) once, then on_update(self, dt), and on_contact(self,
// other) for each body its body began touching, on_contact_end(self, other)
// for each it stopped touching (with a PhysicsContactQueue).
// A changed script is prepared in a fresh VM; entity data carries over and
// optional on_reload(self) migrates it. Rejected edits retain the live VM.
// Nothing a script does stops
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
//   position() -> x, y; set_position(x, y); move(dx, dy) (its body goes there too);
//   rotation(); set_rotation(degrees)
//   tween(property, to, seconds[, easing][, options]) -> id: one of its fields
//   ("Transform2D.pos") moved to `to` (kin/ecs/tween.hpp; the game runs
//   update_tweens); options: delay, repeats, yoyo, relative, easing, done =
//   function(self) (once it gets to its end); tweening([property]),
//   stop_tween(id), stop_tweens([property])
//   after(seconds, fn) -> id, every(seconds, fn) -> id: fn(self) then (every:
//   each time), until cancel(id) or it stops running its script
//   velocity() -> vx, vy; set_velocity(vx, vy): its CharacterController2D's,
//   else its body's (PhysicsVelocity); on_floor(), on_wall(), on_ceiling():
//   what its CharacterController2D found in the last step
//   destroy()
//   play(animation), set_param(name, true|false|number), trigger(name), state()  (its Animator's)
// and the script has:
//   input.held(action), input.pressed(action), input.released(action), input.axis(name),
//   input.mouse() -> x, y
//   world.find(name) -> an entity or nil; world.spawn(prefab, x, y) -> its root, or nil
//   camera.shake(trauma) shakes the current cameras (0 to 1, added; it wears
//   off: kin/ecs/camera.hpp); camera.trauma() -> what is left
// and, given the scene's physics (set_physics), what is where (positions in
// the world; `options`, a table: mask = the kinds of collider to find (bits),
// sensors = true to find sensors too, ignore = an entity, self say):
//   physics.raycast(x1, y1, x2, y2[, options]) -> the nearest hit, or nil:
//     {entity = , x = , y = , normal = {x = , y = }, fraction = }
//   physics.cast_circle(x1, y1, x2, y2, radius[, options]) and
//   physics.cast_box(x1, y1, x2, y2, width, height[, options]): the same, for
//     a circle or a box (centred on the point, not turned) going that way
//   physics.overlap_point(x, y[, options]), physics.overlap_circle(x, y,
//     radius[, options]), physics.overlap_box(x, y, width, height[, options])
//     -> a list of the entities there (a box centred on x, y)
// and, given the scene's screens (set_screens), its UI's elements by id:
//   ui.clicked(id), ui.changed(id), ui.value(id), ui.checked(id), ui.text(id),
//   ui.set_text(id, text), ui.set_value(id, v), ui.set_checked(id, b), ui.show(id[, b]),
//   ui.hide(id), ui.set_image(id, path), ui.set_enabled(id, b)
// and, given the scene's dialogue (set_dialogue): dialogue.start(path[, node]),
// dialogue.stop(), dialogue.active(), dialogue.advance(), dialogue.choose(i or id),
// dialogue.node(), dialogue.get(name), dialogue.set(name, value); and each
// entity's on_dialogue_event(self, id, node) for what the dialogue told
// (its events, "dialogue.started", "dialogue.ended") this step.
// And, given the scene's flow (set_flow): scene.go(path[, arrive[,
// transition[, seconds]]]) asks to go to a scene file of the content (empty:
// a place of this one), arriving at the entity named `arrive` (transition:
// "fade", "crossfade", "dissolve", "pixelate", "wipe", "iris" or "none");
// scene.going() while one is asked for and not yet begun.
// And, given the game's saves (set_saves, kin/ecs/saving.hpp): save.write(slot[,
// title[, summary]]) -> true, or nil and why; save.read(slot) asks the game to
// read it (it does, after the step) -> true, or false and why; save.exists(slot),
// save.delete(slot), save.slots() -> {{id, title, summary, updated}, ...} (the
// newest first); save.get(name[, default]) and save.set(name, value) for the
// game's own values (true/false, numbers, text and tables of them; nil takes
// one away), kept with the saves and from scene to scene. An entity marked
// Saved (with script) keeps its self's values (the same kinds; its entity and
// functions not): given back once it starts, after on_start, and then
// on_load(self) runs.
// Fields read and set as scene files have them: numbers, true/false, text,
// {x = , y = } pairs and {r = , g = , b = , a = } colours.

#include <filesystem>
#include <kin/core/types.hpp>
#include <kin/scripting/lua_runtime.hpp>
#include <map>
#include <memory>
#include <string>

namespace kin {

class EcsWorld;
class Input;
class SceneScreens;
class SceneDialogue;
class SceneFlow;
class SceneSaves;
class PhysicsWorld;
class ScriptWorkflows2D;

struct EntityScriptsConfig {
    std::filesystem::path content;  // the scripts (and the prefabs they spawn) are read from here
    i32 check_frames = 30;          // script files changed are read again every so many updates (0: never)
    LuaRuntimeLimits runtime_limits;
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
    // Prepare all affected instances in a fresh VM; preserve self data and
    // keep the active VM on any failure. on_reload(self) is optional.
    bool reload(std::string_view asset);
    std::map<std::string, LuaRuntimeStats> runtime_stats() const;
    std::map<std::string, std::vector<std::string>> api_surface() const;
    // The scene's UI screens, for the scripts' `ui` (null: none).
    void set_screens(SceneScreens* screens);
    // The scene's dialogue, for the scripts' `dialogue` and on_dialogue_event (null: none).
    void set_dialogue(SceneDialogue* dialogue);
    // The scene's flow, for the scripts' `scene` (null: none).
    void set_flow(SceneFlow* flow);
    // The scene's physics, for the scripts' `physics` and their entities'
    // bodies' velocities (null: none).
    void set_physics(PhysicsWorld* physics);
    // The game's saves, for the scripts' `save` and their entities' kept
    // values (null: none). They are given the scripts' hooks (set_scripts).
    void set_saves(SceneSaves* saves);
    // Optional shared renderer/job/controller services; otherwise a native
    // workflow service is made automatically on the first update.
    void set_workflows(std::shared_ptr<ScriptWorkflows2D> workflows);
    std::shared_ptr<ScriptWorkflows2D> workflows() const;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace kin
