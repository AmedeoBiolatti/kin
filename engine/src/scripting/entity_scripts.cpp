#include "entity_scripts_internal.hpp"

#include "lua_api_surface.hpp"
#include "lua_state_copy.hpp"
#include "lua_vm.hpp"

#include <kin/ecs/dialogue.hpp>
#include <kin/ecs/render.hpp>
#include <kin/ecs/saving.hpp>
#include <kin/ecs/script.hpp>
#include <kin/platform/input.hpp>
#include <kin/platform/log.hpp>
#include <kin/prefab/prefab_asset.hpp>
#include <kin/scripting/lua_values.hpp>
#include <kin/scripting/workflows2d.hpp>
#if KIN_ENABLE_PHYSICS
#include <kin/ecs/physics.hpp>
#endif

#include <algorithm>
#include <system_error>
#include <utility>

namespace kin {
namespace {

FileTime time_of(const std::filesystem::path& file) {
    std::error_code error;
    const auto time = std::filesystem::last_write_time(file, error);
    return error ? std::nullopt : std::optional(time);
}

} // namespace

void EntityScripts::Impl::bind(sol::state& lua) {
    bind_entity(lua);
    bind_services(lua);
}

std::shared_ptr<const PrefabAsset> EntityScripts::Impl::prefab_of(const std::string& path) {
    if (const auto found = prefabs.find(path); found != prefabs.end()) {
        return found->second;
    }
    const PrefabLoadResult loaded = load_prefab_asset(config.content / path, world->components());
    std::shared_ptr<const PrefabAsset> asset = loaded.prefab ? std::make_shared<const PrefabAsset>(*loaded.prefab) : nullptr;
    prefabs.emplace(path, asset);
    return asset;
}

std::unique_ptr<LuaScript> EntityScripts::Impl::make_script(const std::string& path) {
    return std::make_unique<LuaScript>(LuaScriptOptions{
        .setup = [this](sol::state& lua) { bind(lua); },
        .module_root = config.content,
        .limits = config.runtime_limits,
        .on_error =
            [this, path](std::string_view message) {
                KIN_LOG_WARN_F("script", "script error",
                               (LogFields{{.name = "script", .value = path},
                                          {.name = "error", .value = std::string(message)}}));
                problems[path] = std::string(message);
            },
    });
}

void EntityScripts::Impl::remember_times(const std::string& path, File& made) {
    made.times.clear();
    for (const auto& dependency : made.script->dependencies()) {
        made.times.emplace_back(dependency, time_of(dependency));
    }
    const auto main = config.content / path;
    if (std::ranges::none_of(made.times, [&](const auto& entry) { return entry.first == main; })) {
        made.times.emplace_back(main, time_of(main));
    }
}

EntityScripts::Impl::File& EntityScripts::Impl::file(const std::string& path) {
    auto [found, fresh] = files.try_emplace(path);
    File& made = found->second;
    if (fresh) {
        made.script = make_script(path);
        if (made.script->load_file(config.content / path)) {
            problems.erase(path);
            ++made.generation;
        }
        remember_times(path, made);
    }
    return made;
}

EntityScripts::Impl::WorldScope EntityScripts::Impl::bind_world() {
    EcsWorld* previous = world.get();
    world = bound_world;
    return {world, previous};
}

bool EntityScripts::Impl::reload(const std::string& path, File& made) {
    if (!made.script->loaded()) {
        const bool loaded = made.script->load_file(config.content / path);
        if (loaded) {
            problems.erase(path);
            ++made.generation;
        }
        remember_times(path, made);
        return loaded;
    }
    const WorldScope scope = bind_world();
    auto next = make_script(path);
    const scripting_detail::ReloadStaging staging;
    std::map<u64, sol::table> selves; // destroyed before the candidate on rejection
    try {
        {
            const LuaInstructionBudget budget{lua_maintenance_budget};
            if (!next->load_file(config.content / path)) {
                throw std::runtime_error(next->error());
            }
        }
        std::vector<u64> ids;
        for (const auto& [id, run] : running) {
            if (run.path == path) {
                ids.push_back(id);
            }
        }
        std::ranges::sort(ids);
        for (u64 id : ids) {
            // Each instance migrates under its own budget: copy its data,
            // make its self in the candidate, then on_reload(self).
            const LuaInstructionBudget budget{lua_maintenance_budget};
            auto& run = running.at(id);
            const auto copy_entity = [&](const sol::object& value) -> std::optional<sol::object> {
                if (!value.is<ScriptEntity>()) {
                    return {};
                }
                auto entity = value.as<ScriptEntity>();
                if (!entity.alive()) {
                    throw std::runtime_error("reload state contains a stale entity");
                }
                return sol::make_object(next->lua(), entity);
            };
            const auto copied = scripting_detail::copy_lua_data(sol::make_object(run.self.lua_state(), run.self),
                                                                next->lua(), copy_entity, true)
                                    .as<sol::table>();
            auto result =
                next->call_for<sol::table>("__kin_self", ScriptEntity{world.get(), world.get()->raw().entity(id)});
            if (!result) {
                throw std::runtime_error(next->error().empty() ? "script self factory is missing" : next->error());
            }
            sol::table self = *result;
            for (const auto& [key, value] : copied) {
                self[key] = value;
            }
            if (next->runtime().memory_exceeded()) {
                throw std::runtime_error("entity reload exceeds Lua memory limit");
            }
            selves.emplace(id, self);
            next->set_task_owner(id);
            if (next->has("on_reload") && !next->call("on_reload", self)) {
                throw std::runtime_error(next->error());
            }
        }
        // No fallible Lua code after this point. Old selves and tasks are
        // released with the old VM, after every migration has succeeded.
        ++made.generation;
        for (auto& [id, self] : selves) {
            auto& run = running.at(id);
            run.self = std::move(self);
            run.generation = made.generation;
        }
        made.script.swap(next);
        problems.erase(path);
        remember_times(path, made);
        return true;
    } catch (const std::exception& e) {
        problems[path] = "reload: " + std::string(e.what());
        remember_times(path, made); // suppress unchanged bad edits
        return false;
    }
}

void EntityScripts::Impl::stop(const Running& run, std::string_view reason) {
    const WorldScope scope = bind_world();
    auto found = files.find(run.path);
    if (found == files.end()) {
        return;
    }
    auto& script = *found->second.script;
    script.set_task_owner(run.entity_id);
    if (run.started && script.has("on_stop")) {
        script.call("on_stop", run.self, std::string(reason));
    }
    script.cancel_tasks(run.entity_id);
}

void EntityScripts::Impl::reload_changed() {
    for (auto& [path, made] : files) {
        if (std::ranges::any_of(made.times, [](const auto& file) { return time_of(file.first) != file.second; })) {
            reload(path, made);
        }
    }
    prefabs.clear();
}

bool EntityScripts::Impl::task_owner_alive(EcsWorld& world, u64 id) const {
    const auto found = running.find(id);
    if (found == running.end()) {
        return false;
    }
    const EcsEntity entity{world.raw().entity(id)};
    const ScriptComponent* component = entity ? entity.get<ScriptComponent>() : nullptr;
    return entity && entity.raw().enabled() && component && component->enabled &&
           component->asset.id == found->second.path && same_lifetime(entity, found->second.lifetime);
}

EntityScripts::EntityScripts(EntityScriptsConfig config) : _impl(std::make_unique<Impl>()) {
    _impl->config = std::move(config);
}

EntityScripts::~EntityScripts() {
    if (_impl->saves) {
        _impl->saves->set_scripts({}); // its hooks go with it
    }
    if (_impl->workflows) {
        _impl->workflows->leave(this);
    }
    _impl->running.clear(); // the selves before the states they live in
}

const std::map<std::string, std::string>& EntityScripts::problems() const {
    return _impl->problems;
}

std::size_t EntityScripts::running() const {
    return _impl->running.size();
}

void EntityScripts::restart() {
    for (auto& [id, run] : _impl->running) {
        _impl->stop(run, "restart");
    }
    _impl->running.clear();
}

bool EntityScripts::reload(std::string_view asset) {
    const std::string path(asset);
    auto found = _impl->files.find(path);
    if (found == _impl->files.end()) {
        return false;
    }
    return _impl->reload(path, found->second);
}

LuaApiSurface EntityScripts::api_surface() const {
    auto script = _impl->make_script("<api>");
    LuaApiSurface surface = script->api_surface();
    // self's own methods (the self prelude's), beside its entity's.
    auto& entity = surface.tables["Entity"];
    const sol::table own = script->lua()["__kin_self_methods"];
    for (const auto& [key, value] : own) {
        entity.push_back(key.as<std::string>());
    }
    std::ranges::sort(entity);
    entity.erase(std::unique(entity.begin(), entity.end()), entity.end());
    for (const char* callback : {"on_start", "on_load", "on_reload"}) {
        surface.callbacks[callback] = {"Entity"};
    }
    surface.callbacks["on_update"] = {"Entity", "number"};
    surface.callbacks["on_stop"] = {"Entity", "string"};
    surface.callbacks["on_contact"] = {"Entity", "Entity"};
    surface.callbacks["on_contact_end"] = {"Entity", "Entity"};
    surface.callbacks["on_dialogue_event"] = {"Entity", "string", "string"};
    return surface;
}

std::map<std::string, LuaRuntimeStats> EntityScripts::runtime_stats() const {
    std::map<std::string, LuaRuntimeStats> stats;
    for (const auto& [path, file] : _impl->files) {
        stats.emplace(path, file.script->runtime().stats());
    }
    return stats;
}

void EntityScripts::set_screens(SceneScreens* screens) {
    _impl->screens = screens;
}

void EntityScripts::set_dialogue(SceneDialogue* dialogue) {
    _impl->dialogue = dialogue;
}

void EntityScripts::set_flow(SceneFlow* flow) {
    _impl->flow = flow;
}

void EntityScripts::set_physics(PhysicsWorld* physics) {
    _impl->physics = physics;
}

void EntityScripts::set_saves(SceneSaves* saves) {
    Impl& impl = *_impl;
    impl.saves = saves;
    impl.restored.clear();
    if (!saves) {
        return;
    }
    saves->set_scripts({
        .write = [&impl](EcsEntity entity) -> std::optional<JsonValue> {
            const auto found = impl.running.find(entity.id());
            if (found == impl.running.end() || !found->second.self.valid()) {
                return std::nullopt;
            }
            JsonValue::Object values;
            found->second.self.for_each([&](const sol::object& key, const sol::object& value) {
                if (key.get_type() != sol::type::string) {
                    return;
                }
                std::string name = key.as<std::string>();
                if (name == "entity") {
                    return; // its entity (methods), not a value
                }
                if (std::optional<JsonValue> kept = lua_to_json(value)) {
                    values.emplace(std::move(name), std::move(*kept));
                }
            });
            return JsonValue{std::move(values)};
        },
        .read = [&impl](EcsEntity entity, const JsonValue& values) {
            if (values.is_object()) {
                impl.restored[entity.id()] = values;
            }
        },
    });
}

void EntityScripts::update(EcsWorld& world, Input& input, f32 dt) {
    Impl& impl = *_impl;
    impl.bound_world = &world;
    impl.world = &world;
    impl.input = &input;
    if (!impl.workflows) {
        impl.workflows = ScriptWorkflows2D::shared(world, impl.config.content);
    }
    if (&impl.workflows->world() != &world) {
        throw std::invalid_argument("workflow services belong to another world");
    }
    impl.workflows->step(this, input, dt);
    if (impl.config.check_frames > 0 && ++impl.frames >= impl.config.check_frames) {
        impl.frames = 0;
        impl.reload_changed();
    }
    // The scripted entities now (a script may make or destroy some as it runs).
    std::vector<std::pair<flecs::entity, std::string>> scripted;
    world.each([&](flecs::entity entity, const ScriptComponent& script) {
        if (script.enabled && !script.asset.id.empty() && entity.enabled()) {
            scripted.emplace_back(entity, script.asset.id);
        }
    });
    // Values given back to entities gone since (or running no script): let go.
    std::erase_if(impl.restored, [&](const auto& entry) { return !world.raw().is_alive(entry.first); });
    // Those that no longer run theirs: gone.
    std::erase_if(impl.running, [&](const auto& entry) {
        const auto found = std::ranges::find_if(scripted, [&](const auto& item) { return item.first.id() == entry.first; });
        const bool remove = found == scripted.end() || found->second != entry.second.path;
        if (remove) {
            impl.stop(entry.second, !world.raw().is_alive(entry.first) ? "destroyed" : "disabled_or_removed");
        }
        return remove;
    });
    std::ranges::sort(scripted, [](const auto& a, const auto& b) { return a.first.id() < b.first.id(); });
    for (const auto& [entity, path] : scripted) {
        if (!entity.is_alive()) {
            continue;
        }
        const ScriptComponent* component = entity.get<ScriptComponent>();
        impl.player = component && component->player > 0 ? component->player - 1 : -1;
        Impl::File& file = impl.file(path);
        if (!file.script->loaded()) {
            continue; // it does not read: its problem is told
        }
        auto [found, fresh] = impl.running.try_emplace(entity.id());
        Impl::Running& run = found->second;
        const u64 lifetime = entity_lifetime(EcsEntity{entity});
        if (fresh || run.generation != file.generation || run.lifetime != lifetime) {
            if (!fresh && run.self.valid()) {
                impl.stop(run, "replaced");
            }
            // Its self, in the file's state as read now.
            run = {.path = path, .generation = file.generation, .lifetime = lifetime, .entity_id = entity.id()};
            auto made = file.script->call_for<sol::table>("__kin_self", ScriptEntity{&world, entity});
            if (!made) {
                impl.problems[path] =
                    file.script->error().empty() ? "script self factory is missing" : file.script->error();
                continue;
            }
            run.self = *made;
        }
        file.script->set_task_owner(entity.id());
        if (!run.started) {
            run.started = true;
            file.script->call("on_start", run.self);
        }
        // What a save gave back: its self's again, then on_load.
        if (const auto given = impl.restored.find(entity.id()); given != impl.restored.end()) {
            const JsonValue values = std::move(given->second);
            impl.restored.erase(given);
            if (entity.is_alive() && run.self.valid()) {
                sol::state_view lua(run.self.lua_state());
                for (const auto& [name, value] : values.members()) {
                    if (name != "entity") {
                        run.self[name] = json_to_lua(lua, value);
                    }
                }
                if (file.script->has("on_load")) {
                    file.script->call("on_load", run.self);
                }
            }
        }
        if (entity.is_alive()) {
            file.script->call("on_update", run.self, dt);
        }
        // What the dialogue told this step (the game clears them once the step has seen them).
        if (impl.dialogue && entity.is_alive() && file.script->has("on_dialogue_event")) {
            const std::vector<DialogueEvent> told = impl.dialogue->events();
            for (const DialogueEvent& event : told) {
                if (!entity.is_alive()) break;
                file.script->call("on_dialogue_event", run.self, event.id, event.node_id);
            }
        }
#if KIN_ENABLE_PHYSICS
        // What its body began touching this step, and stopped touching.
        for (const auto& [type, callback] : {std::pair{PhysicsContactEventType::Begin, "on_contact"},
                                             std::pair{PhysicsContactEventType::End, "on_contact_end"}}) {
            if (!entity.is_alive() || !file.script->has(callback)) {
                continue;
            }
            const PhysicsContactQueue* queue = entity.get<PhysicsContactQueue>();
            if (!queue || queue->events.empty()) {
                continue;
            }
            const PhysicsBodyComponent* mine = entity.get<PhysicsBodyComponent>();
            std::vector<PhysicsBody> touched;
            for (const PhysicsContactEvent& event : queue->events) {
                if (event.type == type) {
                    touched.push_back(mine && event.body_a == mine->body ? event.body_b : event.body_a);
                }
            }
            for (const PhysicsBody body : touched) {
                flecs::entity other;
                world.each([&](flecs::entity candidate, const PhysicsBodyComponent& component) {
                    if (!other && component.body == body) {
                        other = candidate;
                    }
                });
                if (!other) { // a tilemap's solid cells
                    world.each([&](flecs::entity candidate, const TilemapCollider& component) {
                        if (!other && component.body == body) {
                            other = candidate;
                        }
                    });
                }
                if (other && entity.is_alive()) {
                    file.script->call(callback, run.self, ScriptEntity{&world, other});
                }
            }
        }
#endif
    }
    for (auto& [path, file] : impl.files) {
        file.script->advance_tasks(dt, [&](u64 owner) { return impl.task_owner_alive(world, owner); });
    }
    impl.world = nullptr;
    impl.input = nullptr;
    impl.player = -1;
}

} // namespace kin
