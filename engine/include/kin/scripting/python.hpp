#pragma once

// Experimental (KIN_ENABLE_PYTHON, docs/python_scripting.md): gameplay in
// Python from components, tasks and events, run by PocketPy (a Python 3
// interpreter in C) in a sandbox. Lua scripting does not change.
//
// A PythonScripts loads every module of its folder (scripts/, of the content)
// in name order; modules import each other (`import enemies`) from that folder
// only. Each update it runs, in this order:
//   - kin's tweens (when update_tweens), and the start(self) of each scripted
//     component an entity gained;
//   - the events queued since the last update, to their @on handlers;
//   - the @system functions and the update(self, dt) methods of components, in
//     the order their modules loaded and defined them;
//   - the events those emitted;
//   - the tasks due (their wait is over), each to its next yield;
//   - the events the tasks emitted.
// A module whose file changed is read again (with those that import it):
// its tasks are cancelled, its systems, handlers and methods replaced, its
// components migrated (their fields' values kept), and on_reload() called.
// A module that fails to read keeps the last good one running.
// Nothing a script does stops the game: what it raises is logged, kept in
// problems(), and that call skipped.
//
//     kin::PythonScripts scripts{world, {.content = kin::content_root()}};
//     // before loading scenes that use the scripts' components; then each frame, after physics:
//     scripts.update(ctx.input, ctx.dt);
//
// Each PythonScripts has an interpreter of its own (PocketPy has 15 to give);
// all of it runs on the thread that made it. Destroy it before its world.

#include <kin/core/types.hpp>

#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace kin {

class EcsWorld;
class EcsEntity;
class Input;
class PhysicsWorld;
class SceneFlow;
class SceneSaves;

struct PythonScriptsConfig {
    std::filesystem::path content;    // the content root: prefabs spawn from here
    std::string folder = "scripts";   // the modules, a folder of the content
    i32 check_frames = 30;            // changed files are read again every so many updates (0: never)
    // Bytecode instructions one call may run (a system's run over its
    // entities, a handler, a task's step to its next yield, a module's load):
    // each counted, the same on every run and machine. 0: no limit.
    u64 instruction_limit = 50'000'000;
    // Bytes the interpreter may hold (garbage not yet collected counts too);
    // 0: no limit. A call that goes past fails at its next instruction.
    std::size_t memory_limit = 64u * 1024u * 1024u;
    // Rounds of handlers emitting events that are delivered in the same
    // update; what is left waits for the next (and is told, as a loop).
    i32 event_rounds = 16;
    // Steps kin's tweens (update_tweens) at the start of each update: off
    // when something else of the game already does.
    bool update_tweens = true;
};

// Whether `code` is Python that reads (it is not run): false, and `error` says
// where (name:line) and why, when it does not.
bool check_python_syntax(std::string_view code, std::string_view name, std::string& error);

class PythonScripts {
public:
    // Loads the folder's modules: their problems are in problems().
    PythonScripts(EcsWorld& world, PythonScriptsConfig config);
    ~PythonScripts();
    PythonScripts(const PythonScripts&) = delete;
    PythonScripts& operator=(const PythonScripts&) = delete;

    void update(Input& input, f32 dt);

    // An event for the scripts, delivered at the next update: from the world
    // (no entity), or from `entity`. Its data is None.
    void emit(std::string_view name);
    void emit(std::string_view name, EcsEntity entity);

    // Reads every module again, as if each had changed.
    void reload();

    // What the game gives its scripts (null: none, and the scripts using it
    // raise): its physics, for kin.physics's queries; its scene flow
    // (kin/ecs/scene_flow.hpp), for kin.scene.go; its saves
    // (kin/ecs/saving.hpp), for kin.save. They must outlive their use here.
    void set_physics(PhysicsWorld* physics);
    void set_flow(SceneFlow* flow);
    void set_saves(SceneSaves* saves);

    // Problems by module (enemies), with why (enemies.py:12: what). A module's
    // goes once it reads and runs again.
    const std::map<std::string, std::string>& problems() const;
    // The modules loaded, in load order.
    std::vector<std::string> modules() const;
    // Tasks not yet done.
    std::size_t tasks() const;

    // Writes kin.pyi, the module kin's types for editors (VS Code completes and
    // checks scripts with it): kin's API, and a class for each component the
    // world describes now (the scripts' own come from their modules). False,
    // with why in `error`, when it could not be written.
    bool write_stubs(const std::filesystem::path& file, std::string* error = nullptr) const;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace kin
