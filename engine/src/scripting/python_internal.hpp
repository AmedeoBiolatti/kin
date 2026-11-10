#pragma once

// What python_host.cpp, python_api.cpp and python_values.cpp share: the
// host of a PythonScripts and its interpreter (kin/scripting/python.hpp).

#include <kin/ecs/component.hpp>
#include <kin/ecs/world.hpp>
#include <kin/prefab/prefab_asset.hpp>
#include <kin/scripting/python.hpp>

#include <pocketpy.h>

#include "pocketpy_sandbox.h"

#include <flecs.h>

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace kin::python_detail {

using FileTime = std::optional<std::filesystem::file_time_type>;

// A component as scripts reach it: its fields by name, and the type of its views.
struct ComponentInfo {
    ComponentId id = 0;
    std::string name;
    bool data = false;     // a data component: its fields in the registry's payload
    bool scripted = false; // defined by a module (@component)
    std::string module;    // that module
    u64 generation = 0;    // the read of it that defined it
    std::vector<py_Name> names;
    std::vector<ComponentFieldKind> kinds;
    std::vector<bool> readonly;
    std::vector<std::optional<ComponentFieldAccess>> access; // a native component's fields
    py_Type type = 0;           // its views' (kin.Transform2D, or the class @component made)
    std::vector<py_Name> copied; // what its class gave the type (methods), taken away on reload
    py_TValue start{};          // its class's start(self) and stop(self), or nil
    py_TValue stop{};
    flecs::entity added;        // the observers calling them
    flecs::entity removed;
    flecs::entity added_event;  // those emitting added:Name, removed:Name, and watching for destroyed
    flecs::entity removed_event;
    flecs::entity destroy_watch;
    std::vector<flecs::entity_t> to_start;
    std::unordered_set<flecs::entity_t> started;
    // Its views, by entity index (the id's low 32 bits): each entity's one,
    // made once (an index another entity has now gets a new one).
    std::vector<std::pair<flecs::entity_t, py_TValue>> views;
};

// What a view holds: the entity and its component. Made by kin only.
struct View {
    flecs::entity_t entity = 0;
    ComponentInfo* info = nullptr;
};

// What an Entity holds.
struct EntityData {
    flecs::entity_t entity = 0;
};

struct System {
    std::string module;
    u64 generation = 0;
    std::string name;  // the function's, for problems
    py_TValue fn{};
    std::vector<ComponentInfo*> with;
    std::vector<ComponentId> without;
    bool method = false; // a component's update(self, dt): called (view, dt)
    flecs::query<> query;
    std::string failing; // the problem of its last run, told once
};

struct Handler {
    std::string module;
    u64 generation = 0;
    std::string event;
    std::string name;
    py_TValue fn{};
    std::vector<ComponentId> with;
};

struct QueuedEvent {
    std::string name;
    flecs::entity_t entity = 0; // 0: the world's
    py_TValue data{};
    ComponentId via = 0; // "destroyed": the component it was watched for
};

struct Module {
    std::string name;  // enemies, or levels.forest
    std::string file;  // enemies.py, under the folder
    FileTime time;
    py_GlobalRef object = nullptr;
    std::string internal; // the path PocketPy knows its object by (one per read)
    std::vector<std::string> imports; // the modules of the folder it imported
    u64 generation = 0;
};

struct ViewKey {
    flecs::entity_t entity = 0;
    ComponentId component = 0;
    bool operator==(const ViewKey&) const = default;
};

struct ViewKeyHash {
    std::size_t operator()(const ViewKey& key) const noexcept {
        return std::hash<u64>{}(key.entity * 0x9E3779B97F4A7C15ull ^ key.component);
    }
};

struct Host {
    EcsWorld& world;
    PythonScriptsConfig config;
    int vm = 0;
    Input* input = nullptr;
    // What the game gives its scripts (null: none).
    PhysicsWorld* physics = nullptr;
    SceneFlow* flow = nullptr;
    SceneSaves* saves = nullptr;

    py_GlobalRef kin = nullptr;
    py_GlobalRef time = nullptr;
    py_GlobalRef input_module = nullptr;
    py_GlobalRef world_module = nullptr;
    py_GlobalRef physics_module = nullptr;
    py_GlobalRef scene_module = nullptr;
    py_GlobalRef save_module = nullptr;
    py_Type entity_type = 0;
    // The prelude's (python_kin.py), kept to call.
    py_TValue spawn{}, due{}, step{}, fail{}, wake{}, cancel_module{}, running{}, tween{};

    std::unordered_map<ComponentId, std::unique_ptr<ComponentInfo>> components;
    std::unordered_map<py_Type, ComponentInfo*> by_type;
    // Entities' objects, by entity index, as ComponentInfo::views.
    std::vector<std::pair<flecs::entity_t, py_TValue>> entities;
    flecs::id_t data_payload = 0; // the component data components keep their fields in
    std::vector<System> systems;
    std::vector<Handler> handlers;
    std::vector<QueuedEvent> events;
    std::vector<QueuedEvent> delivering; // those being delivered now (kept marked)
    // Entities that lost a component watched for "destroyed": told once they are gone.
    std::vector<std::pair<flecs::entity_t, ComponentId>> maybe_destroyed;

    std::map<std::string, Module> modules;
    std::vector<std::string> order;           // load order
    std::vector<std::string> executing;       // the modules running their top level now, innermost last
    std::map<std::string, std::string> problems;
    std::unordered_map<std::string, std::shared_ptr<const PrefabAsset>> prefabs;
    std::size_t descriptors_seen = 0;

    f64 now = 0.0;
    u64 frame = 0;
    f32 dt = 0.0f;
    i32 frames_since_check = 0;
    u64 generations = 0;
    int budget_depth = 0;
    bool closing = false;

    Host(EcsWorld& world, PythonScriptsConfig config);
    ~Host();
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    // python_values.cpp
    void entity_object(py_OutRef out, flecs::entity_t entity);
    bool view_object(py_OutRef out, flecs::entity_t entity, ComponentInfo& info);
    ComponentInfo* info_for(ComponentId id);
    ComponentInfo* info_for(std::string_view name);
    void refresh_fields(ComponentInfo& info);
    void expose_components();
    // A data component's field values on `entity`, or null without it.
    // (Its own, or a prefab instance's prefab's; writable: made its own first.)
    const std::vector<ComponentFieldValue>* data_values(flecs::entity_t entity, ComponentId id) const;
    std::vector<ComponentFieldValue>* writable_data_values(flecs::entity_t entity, ComponentId id);

    // python_host.cpp
    bool call(py_Ref fn, int argc, py_Ref argv, std::string_view module, std::string_view what);
    void problem(std::string_view module, std::string message, std::string traceback = {});
    void queue(std::string name, flecs::entity_t entity, py_Ref data);
    void watch_start_stop(ComponentInfo& info);
    // The observers behind @on("added:Name"), @on("removed:Name") and @on("destroyed", Name).
    void watch_event(ComponentInfo& info, std::string_view event);
    void collect_contacts();
};

// The host whose interpreter is the current one; null when none (or no host
// is running scripts now).
Host* current();

// Raised when there is no current host: false.
bool no_host();

// The module a script function was defined in, by name ("" when it is not a module of the folder).
std::string module_of_function(py_Ref function);
// The component an event's name is about ("added:Health": Health), or empty.
std::string_view component_of_event(std::string_view event);
// The module of the code running now.
std::string module_running();

c11_sv view_of(std::string_view text);
std::string_view view_of(py_Ref value);
std::string text_of_name(py_Name name);

// A field's value as Python holds it, and back (nullopt: not that kind).
void to_python(Host& host, py_OutRef out, const ComponentFieldValue& value);
std::optional<ComponentFieldValue> from_python(ComponentFieldKind kind, py_Ref value);
std::optional<f64> number_of(py_Ref value);
std::optional<Vec2f> pair_of(py_Ref value);
void new_vec2(py_OutRef out, Vec2f value);

// The component an argument names: a view type (kin.Transform2D, a
// @component class) or a name. Null, TypeError raised, when it names none.
ComponentInfo* component_arg(Host& host, py_Ref value);
// The entity of an Entity argument, alive: 0, an exception raised, when it is not.
flecs::entity_t live_entity_arg(Host& host, py_Ref value);
flecs::entity_t entity_arg(Host& host, py_Ref value); // alive or not

// The view types' attribute hooks and magic methods (python_values.cpp).
void set_up_view_type(Host& host, ComponentInfo& info);
void set_up_entity_type(Host& host);
// kin's native functions and modules (python_api.cpp).
void bind_kin(Host& host);
// kin.physics, kin.scene and kin.save (python_services.cpp).
void bind_services(Host& host);
// A @component class made a component (python_api.cpp).
bool define_component(Host& host, py_Ref cls);

// Calls a binding, turning what C++ throws into a Python exception (none may
// cross PocketPy's C frames).
template <bool (*Function)(int, py_StackRef)> bool guarded(int argc, py_StackRef argv) {
    try {
        return Function(argc, argv);
    } catch (const std::exception& error) {
        return py_exception(tp_RuntimeError, "%s", error.what());
    } catch (...) {
        return py_exception(tp_RuntimeError, "%s", "kin failed");
    }
}

} // namespace kin::python_detail
