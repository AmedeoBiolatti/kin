#pragma once

#include "lua_reload_internal.hpp"

#include <kin/ecs/lifecycle.hpp>
#include <kin/ecs/world.hpp>
#include <kin/scripting/entity_scripts.hpp>
#include <kin/scripting/lua_script.hpp>
#if KIN_ENABLE_PHYSICS
#include <kin/physics/physics.hpp>
#endif

#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

// One private userdata identity for registration, pushing and reload migration.
// Compact names keep sol2's primary/reference keys within Lua's short strings.
#if defined(__GNUC__) && !defined(_WIN32)
#define KIN_ENTITY_SCRIPT_TYPE __attribute__((visibility("hidden")))
#else
#define KIN_ENTITY_SCRIPT_TYPE
#endif

namespace kin::s {

// What a script holds of an entity: the world it is in, and it (gone: every
// method but alive() and id() fails).
struct KIN_ENTITY_SCRIPT_TYPE ScriptEntity {
    EcsWorld* world = nullptr;
    flecs::entity entity;

    u64 lifetime = 0; // its incarnation (a pooled entity is used again)

    ScriptEntity(EcsWorld* w, flecs::entity e) : world(w), entity(e) {
        if (entity && entity.is_alive()) {
            lifetime = entity_lifetime(EcsEntity{entity});
        }
    }
    // same_lifetime(), less a second check that it is alive (every binding asks).
    bool alive() const { return entity && entity.is_alive() && entity_lifetime(EcsEntity{entity}) == lifetime; }
    flecs::entity live() const {
        if (scripting_detail::reload_staging)
            throw sol::error("live entity access is unavailable while preparing reload");
        if (!alive()) {
            throw sol::error("the entity is gone");
        }
        return entity;
    }
};

} // namespace kin::s

#undef KIN_ENTITY_SCRIPT_TYPE

namespace kin::entity_script_detail {

using FileTime = std::optional<std::filesystem::file_time_type>;

// A live host service the bindings reach. While a reload candidate is being
// prepared every use throws, so a rejected candidate cannot have touched the
// game; get() is for the host's own code.
template <typename T> class LiveService {
public:
    LiveService& operator=(T* service) {
        _service = service;
        return *this;
    }
    T* get() const { return _service; }
    operator T*() const { return checked(); }
    T* operator->() const { return checked(); }
    T& operator*() const { return *checked(); }

private:
    T* checked() const {
        if (scripting_detail::reload_staging) {
            throw sol::error("live services are unavailable while preparing reload");
        }
        return _service;
    }

    T* _service = nullptr;
};

} // namespace kin::entity_script_detail

namespace kin {

class ScriptWorkflows2D;
struct PrefabAsset;

using entity_script_detail::FileTime;
using entity_script_detail::LiveService;
using s::ScriptEntity;

struct EntityScripts::Impl {
    EntityScriptsConfig config;
    std::shared_ptr<ScriptWorkflows2D> workflows; // the world's (ScriptWorkflows2D::shared)

    // A script file read, and the times of the files it was read from.
    struct File {
        std::unique_ptr<LuaScript> script;
        std::vector<std::pair<std::filesystem::path, FileTime>> times;
        u64 generation = 0; // bumped by each read: its selves belong to one
        // Its callbacks as found in the update numbered `found_in`: looked up
        // once an update, not once an entity (a script that defines one
        // anew has it called from the next update). Declared after `script`,
        // they go first; forget() lets go of them before its state closes.
        u64 found_in = 0;
        sol::protected_function on_update, on_dialogue_event, on_contact, on_contact_end;
        void find_callbacks(u64 update) {
            if (found_in == update) {
                return;
            }
            found_in = update;
            on_update = script->function("on_update");
            on_dialogue_event = script->function("on_dialogue_event");
            on_contact = script->function("on_contact");
            on_contact_end = script->function("on_contact_end");
        }
        void forget_callbacks() {
            found_in = 0;
            on_update = {};
            on_dialogue_event = {};
            on_contact = {};
            on_contact_end = {};
        }
    };
    // An entity running a script: its self, made in the file's state then.
    struct Running {
        std::string path;
        u64 generation = 0;
        sol::table self;
        bool started = false;
        u64 lifetime = 0;  // its entity's incarnation
        u64 entity_id = 0; // its tasks' owner
    };
    std::map<std::string, File> files; // in order: reloads and tasks run reproducibly
    std::unordered_map<u64, Running> running;
    std::map<std::string, std::string> problems;
    std::unordered_map<std::string, std::shared_ptr<const PrefabAsset>> prefabs;
    i32 frames = 0;
    u64 updates = 0; // EntityScripts::update calls, numbering them for File::found_in

    // What the bindings reach, during an update.
    LiveService<EcsWorld> world;
    EcsWorld* bound_world = nullptr; // the world of the last update (for reloads and stops between them)
    LiveService<Input> input;
    LiveService<SceneScreens> screens;
    LiveService<SceneDialogue> dialogue;
    LiveService<SceneFlow> flow;
    LiveService<SceneSaves> saves;
    LiveService<PhysicsWorld> physics;
    // Values given back by the saves, for entities once they start (by id).
    std::unordered_map<u64, JsonValue> restored;
    std::string calling; // the script being called, for its problems
    i32 player = -1;     // whose input the entity being run reads (Input's numbers: -1 anyone)

    // A player as a script names it (1 the first, 0 anyone; none: its entity's), as Input numbers them.
    i32 player_of(const sol::optional<i32>& named) const {
        return named ? (*named > 0 ? *named - 1 : -1) : player;
    }

    struct WorldScope {
        LiveService<EcsWorld>& world;
        EcsWorld* previous;
        ~WorldScope() { world = previous; }
    };

    void bind(sol::state& lua);
    void bind_entity(sol::state& lua);
    void bind_services(sol::state& lua);
    std::shared_ptr<const PrefabAsset> prefab_of(const std::string& path);
#if KIN_ENABLE_PHYSICS
    flecs::entity entity_of(PhysicsBody body) const;
    static PhysicsFilter filter_of(const sol::optional<sol::table>& options);
    PhysicsWorld& physics_or_throw() const;
    sol::table hit_table(sol::state_view lua, PhysicsBody body, Vec2f point, Vec2f normal, f32 fraction) const;
    sol::table entities_table(sol::state_view lua, const std::vector<PhysicsFixture>& fixtures) const;
    void bind_physics(sol::state& lua);
#endif
    std::unique_ptr<LuaScript> make_script(const std::string& path);
    void remember_times(const std::string& path, File& made);
    File& file(const std::string& path);
    WorldScope bind_world();
    bool reload(const std::string& path, File& made);
    void stop(const Running& run, std::string_view reason);
    void reload_changed();
    bool task_owner_alive(EcsWorld& world, u64 id) const;
};

} // namespace kin
