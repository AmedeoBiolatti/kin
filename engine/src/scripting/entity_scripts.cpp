#include <kin/scripting/entity_scripts.hpp>

#include <kin/ecs/animator.hpp>
#include <kin/ecs/render.hpp>
#include <kin/ecs/script.hpp>
#include <kin/ecs/dialogue.hpp>
#include <kin/ecs/scene_flow.hpp>
#include <kin/ecs/ui_screens.hpp>
#include <kin/ecs/world.hpp>
#include <kin/l10n/localization.hpp>
#include <kin/platform/input.hpp>
#include <kin/platform/log.hpp>
#include <kin/prefab/prefab_asset.hpp>
#include <kin/scripting/lua_script.hpp>
#if KIN_ENABLE_PHYSICS
#include <kin/ecs/physics.hpp>
#endif

#include <algorithm>
#include <cstdio>
#include <optional>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kin {
namespace {

using FileTime = std::optional<std::filesystem::file_time_type>;

FileTime time_of(const std::filesystem::path& file) {
    std::error_code error;
    const auto time = std::filesystem::last_write_time(file, error);
    return error ? std::nullopt : std::optional(time);
}

// What a script holds of an entity: the world it is in, and it (gone: every
// method but alive() and id() fails).
struct ScriptEntity {
    EcsWorld* world = nullptr;
    flecs::entity entity;

    bool alive() const { return entity && entity.is_alive(); }
    flecs::entity live() const {
        if (!alive()) {
            throw sol::error("the entity is gone");
        }
        return entity;
    }
};

// A field's value as Lua holds it, and back.
sol::object to_lua(sol::state_view lua, const ComponentFieldValue& value) {
    return std::visit(
        [&](const auto& v) -> sol::object {
            using V = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<V, bool> || std::is_same_v<V, std::string>) {
                return sol::make_object(lua, v);
            } else if constexpr (std::is_arithmetic_v<V>) {
                return sol::make_object(lua, static_cast<f64>(v));
            } else if constexpr (std::is_same_v<V, Vec2f> || std::is_same_v<V, Vec2i>) {
                return sol::make_object(lua, lua.create_table_with("x", v.x, "y", v.y));
            } else if constexpr (std::is_same_v<V, Color>) {
                return sol::make_object(lua, lua.create_table_with("r", v.r, "g", v.g, "b", v.b, "a", v.a));
            } else if constexpr (std::is_same_v<V, ComponentAssetRef>) {
                return sol::make_object(lua, v.id);
            } else if constexpr (std::is_same_v<V, ComponentEntityRef>) {
                return sol::make_object(lua, static_cast<f64>(v.id));
            } else {
                return sol::lua_nil; // 3D values: not for 2D scripts
            }
        },
        value);
}

std::optional<ComponentFieldValue> from_lua(ComponentFieldKind kind, const sol::object& value) {
    const auto number = [&]() -> std::optional<f64> {
        return value.get_type() == sol::type::number ? std::optional(value.as<f64>()) : std::nullopt;
    };
    const auto field = [&](const char* name, f64 fallback) {
        if (value.get_type() != sol::type::table) {
            return fallback;
        }
        const sol::object part = value.as<sol::table>()[name];
        return part.get_type() == sol::type::number ? part.as<f64>() : fallback;
    };
    switch (kind) {
    case ComponentFieldKind::Bool:
        if (value.get_type() == sol::type::boolean) return value.as<bool>();
        break;
    case ComponentFieldKind::I32:
        if (const auto n = number()) return static_cast<i32>(*n);
        break;
    case ComponentFieldKind::I64:
        if (const auto n = number()) return static_cast<i64>(*n);
        break;
    case ComponentFieldKind::U32:
        if (const auto n = number(); n && *n >= 0) return static_cast<u32>(*n);
        break;
    case ComponentFieldKind::U64:
        if (const auto n = number(); n && *n >= 0) return static_cast<u64>(*n);
        break;
    case ComponentFieldKind::F32:
        if (const auto n = number()) return static_cast<f32>(*n);
        break;
    case ComponentFieldKind::F64:
        if (const auto n = number()) return *n;
        break;
    case ComponentFieldKind::String:
        if (value.get_type() == sol::type::string) return value.as<std::string>();
        break;
    case ComponentFieldKind::AssetRef:
        if (value.get_type() == sol::type::string) return ComponentAssetRef{value.as<std::string>()};
        break;
    case ComponentFieldKind::Vec2f:
        if (value.get_type() == sol::type::table) return Vec2f{static_cast<f32>(field("x", 0.0)), static_cast<f32>(field("y", 0.0))};
        break;
    case ComponentFieldKind::Vec2i:
        if (value.get_type() == sol::type::table) return Vec2i{static_cast<i32>(field("x", 0.0)), static_cast<i32>(field("y", 0.0))};
        break;
    case ComponentFieldKind::Color:
        if (value.get_type() == sol::type::table) {
            const auto channel = [&](const char* name) { return static_cast<u8>(std::clamp(field(name, 255.0), 0.0, 255.0)); };
            return Color{channel("r"), channel("g"), channel("b"), channel("a")};
        }
        break;
    default: break;
    }
    return std::nullopt;
}

// Makes a script's `self`: a table of its own, with the entity's methods.
constexpr const char* self_prelude = R"lua(
function __kin_self(entity)
    return setmetatable({entity = entity}, {__index = function(_, key)
        local method = entity[key]
        if type(method) == "function" then
            return function(_, ...) return method(entity, ...) end
        end
        return nil
    end})
end
)lua";

} // namespace

namespace {

// A number as text: 12, not 12.000000.
std::string format_number(double value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%g", value);
    return text;
}

} // namespace

struct EntityScripts::Impl {
    EntityScriptsConfig config;

    // A script file read, and the times of the files it was read from.
    struct File {
        std::unique_ptr<LuaScript> script;
        std::vector<std::pair<std::filesystem::path, FileTime>> times;
        u64 generation = 0; // bumped by each read: its selves belong to one
    };
    // An entity running a script: its self, made in the file's state then.
    struct Running {
        std::string path;
        u64 generation = 0;
        sol::table self;
        bool started = false;
    };
    std::unordered_map<std::string, File> files;
    std::unordered_map<u64, Running> running;
    std::map<std::string, std::string> problems;
    std::unordered_map<std::string, std::shared_ptr<const PrefabAsset>> prefabs;
    i32 frames = 0;

    // What the bindings reach, during an update.
    EcsWorld* world = nullptr;
    Input* input = nullptr;
    SceneScreens* screens = nullptr;
    SceneDialogue* dialogue = nullptr;
    SceneFlow* flow = nullptr;
    std::string calling; // the script being called, for its problems

    void bind(sol::state& lua) {
        sol::usertype<ScriptEntity> entity = lua.new_usertype<ScriptEntity>("Entity", sol::no_constructor);
        entity["id"] = [](const ScriptEntity& e) { return static_cast<f64>(e.entity.id()); };
        entity["alive"] = &ScriptEntity::alive;
        entity["name"] = [](const ScriptEntity& e) { return std::string(e.live().name().c_str()); };
        entity["parent"] = [](const ScriptEntity& e) -> std::optional<ScriptEntity> {
            const flecs::entity parent = e.live().parent();
            return parent ? std::optional(ScriptEntity{e.world, parent}) : std::nullopt;
        };
        entity["child"] = [](const ScriptEntity& e, const std::string& name) -> std::optional<ScriptEntity> {
            const flecs::entity child = e.live().lookup(name.c_str());
            return child ? std::optional(ScriptEntity{e.world, child}) : std::nullopt;
        };
        entity["has"] = [](const ScriptEntity& e, const std::string& component) {
            return e.world->components().has(EcsEntity{e.live()}, component);
        };
        entity["add"] = [](const ScriptEntity& e, const std::string& component) {
            return e.world->components().add(EcsEntity{e.live()}, component);
        };
        entity["remove"] = [](const ScriptEntity& e, const std::string& component) {
            return e.world->components().remove(EcsEntity{e.live()}, component);
        };
        entity["get"] = [](const ScriptEntity& e, const std::string& component, sol::this_state state) -> sol::object {
            sol::state_view lua(state);
            sol::table fields = lua.create_table();
            const bool found = e.world->components().visit_fields(
                EcsEntity{e.live()}, component,
                [&](std::string_view name, ComponentFieldKind, const ComponentFieldValue& value) { fields[std::string(name)] = to_lua(lua, value); });
            return found ? sol::object(fields) : sol::lua_nil;
        };
        entity["set"] = [](const ScriptEntity& e, const std::string& component, const sol::table& values) {
            EcsComponentRegistry& registry = e.world->components();
            const ComponentDescriptor* descriptor = registry.find(component);
            if (!descriptor) {
                throw sol::error("no component '" + component + "' is described");
            }
            EcsEntity target{e.live()};
            if (!registry.has(target, component)) {
                registry.add(target, component);
            }
            for (const auto& [key, value] : values) {
                const std::string name = key.as<std::string>();
                const auto field = std::ranges::find(descriptor->fields, name, &ComponentFieldDescriptor::name);
                if (field == descriptor->fields.end()) {
                    throw sol::error(component + " has no field '" + name + "'");
                }
                const std::optional<ComponentFieldValue> converted = from_lua(field->kind, value);
                if (!converted || !registry.patch_field(target, component, name, *converted)) {
                    throw sol::error(component + "." + name + " takes a " + component_field_kind_name(field->kind));
                }
            }
        };
        entity["position"] = [](const ScriptEntity& e) {
            const Transform2D* t = e.live().get<Transform2D>();
            return std::make_tuple(t ? t->pos.x : 0.0f, t ? t->pos.y : 0.0f);
        };
        entity["set_position"] = [](const ScriptEntity& e, f32 x, f32 y) {
            EcsEntity target{e.live()};
            Transform2D t = target.get<Transform2D>() ? *target.get<Transform2D>() : Transform2D{};
            t.pos = {x, y};
            target.set(t);
        };
        entity["move"] = [](const ScriptEntity& e, f32 dx, f32 dy) {
            EcsEntity target{e.live()};
            Transform2D t = target.get<Transform2D>() ? *target.get<Transform2D>() : Transform2D{};
            t.pos = {t.pos.x + dx, t.pos.y + dy};
            target.set(t);
        };
        entity["rotation"] = [](const ScriptEntity& e) {
            const Transform2D* t = e.live().get<Transform2D>();
            return t ? t->rotation : 0.0f;
        };
        entity["set_rotation"] = [](const ScriptEntity& e, f32 degrees) {
            EcsEntity target{e.live()};
            Transform2D t = target.get<Transform2D>() ? *target.get<Transform2D>() : Transform2D{};
            t.rotation = degrees;
            target.set(t);
        };
        entity["destroy"] = [](const ScriptEntity& e) { e.live().destruct(); };
        // Its Animator's.
        const auto animator = [](const ScriptEntity& e) {
            Animator* found = EcsEntity{e.live()}.get_mut<Animator>();
            if (!found) {
                throw sol::error("the entity has no Animator");
            }
            return found;
        };
        entity["play"] = [animator](const ScriptEntity& e, const std::string& animation) { animator(e)->animation = animation; };
        entity["set_param"] = [animator](const ScriptEntity& e, const std::string& name, const sol::object& value) {
            Animator* found = animator(e);
            if (value.get_type() == sol::type::boolean) {
                found->params.set_bool(name, value.as<bool>());
            } else if (value.get_type() == sol::type::number) {
                found->params.set_float(name, value.as<f32>());
            } else {
                throw sol::error("set_param takes true, false or a number");
            }
        };
        entity["trigger"] = [animator](const ScriptEntity& e, const std::string& name) { animator(e)->params.set_trigger(name); };
        entity["state"] = [animator](const ScriptEntity& e) { return animator(e)->state; };

        sol::table input_table = lua.create_named_table("input");
        input_table["held"] = [this](const std::string& action) { return input && input->held(action); };
        input_table["pressed"] = [this](const std::string& action) { return input && input->pressed(action); };
        input_table["released"] = [this](const std::string& action) { return input && input->released(action); };
        input_table["axis"] = [this](const std::string& name) { return input ? input->axis(name) : 0.0f; };
        input_table["mouse"] = [this]() {
            const Vec2f at = input ? input->mouse_pos() : Vec2f{};
            return std::make_tuple(at.x, at.y);
        };

        // The language, which game scripts may change (a language menu's).
        if (sol::optional<sol::table> l10n = lua["l10n"]) {
            (*l10n)["set_locale"] = [](const std::string& tag) -> std::string {
                Localization* active = active_localization();
                return active ? active->set_locale(tag) : std::string{};
            };
        }

        sol::table ui_table = lua.create_named_table("ui");
        ui_table["clicked"] = [this](const std::string& id) { return screens && screens->clicked(id); };
        ui_table["changed"] = [this](const std::string& id) { return screens && screens->changed(id); };
        ui_table["value"] = [this](const std::string& id) { return screens ? screens->value(id) : 0.0f; };
        ui_table["checked"] = [this](const std::string& id) { return screens && screens->checked(id); };
        ui_table["text"] = [this](const std::string& id) { return screens ? screens->text(id) : std::string(); };
        ui_table["set_text"] = [this](const std::string& id, const sol::object& text) {
            if (screens) {
                screens->set_text(id, text.is<std::string>() ? text.as<std::string>()
                                                            : text.is<double>() ? format_number(text.as<double>()) : std::string());
            }
        };
        ui_table["set_value"] = [this](const std::string& id, f32 value) {
            if (screens) screens->set_value(id, value);
        };
        ui_table["set_checked"] = [this](const std::string& id, bool checked) {
            if (screens) screens->set_checked(id, checked);
        };
        ui_table["show"] = [this](const std::string& id, sol::optional<bool> shown) {
            if (screens) screens->set_visible(id, shown.value_or(true));
        };
        ui_table["hide"] = [this](const std::string& id) {
            if (screens) screens->set_visible(id, false);
        };
        ui_table["set_image"] = [this](const std::string& id, const std::string& path) {
            if (screens) screens->set_image(id, path);
        };
        ui_table["set_enabled"] = [this](const std::string& id, bool enabled) {
            if (screens) screens->set_enabled(id, enabled);
        };

        sol::table dialogue_table = lua.create_named_table("dialogue");
        dialogue_table["start"] = [this](const std::string& path, sol::optional<std::string> node) {
            return dialogue && dialogue->start(path, node.value_or(""));
        };
        dialogue_table["stop"] = [this]() {
            if (dialogue) dialogue->stop();
        };
        dialogue_table["active"] = [this]() { return dialogue && dialogue->active(); };
        dialogue_table["advance"] = [this]() {
            if (dialogue) dialogue->advance();
        };
        dialogue_table["choose"] = [this](const sol::object& which) {
            if (!dialogue) return false;
            if (which.is<std::string>()) return dialogue->choose(std::string_view(which.as<std::string>()));
            return which.is<double>() && dialogue->choose(static_cast<i32>(which.as<double>()) - 1); // Lua counts from 1
        };
        dialogue_table["node"] = [this]() { return dialogue && dialogue->active() ? dialogue->view().node_id : std::string(); };
        dialogue_table["get"] = [this](sol::this_state state, const std::string& name) -> sol::object {
            if (!dialogue) return sol::make_object(state, sol::lua_nil);
            const DialogueVariable value = dialogue->variable(name);
            if (const auto* b = std::get_if<bool>(&value.value)) return sol::make_object(state, *b);
            if (const auto* n = std::get_if<f64>(&value.value)) return sol::make_object(state, *n);
            return sol::make_object(state, std::get<std::string>(value.value));
        };
        dialogue_table["set"] = [this](const std::string& name, const sol::object& value) {
            if (!dialogue) return;
            if (value.is<bool>()) dialogue->set_variable(name, {.value = value.as<bool>()});
            else if (value.is<double>()) dialogue->set_variable(name, {.value = value.as<double>()});
            else if (value.is<std::string>()) dialogue->set_variable(name, {.value = value.as<std::string>()});
        };

        sol::table scene_table = lua.create_named_table("scene");
        scene_table["go"] = [this](const std::string& path, sol::optional<std::string> arrive, sol::optional<std::string> transition,
                                   sol::optional<f32> duration) {
            if (!flow) {
                return false;
            }
            SceneTravel travel{.scene = path, .arrive = arrive.value_or("")};
            if (transition) {
                if (*transition == "none") {
                    travel.transition.reset();
                } else if (const std::optional<TransitionKind> kind = transition_from_name(*transition)) {
                    travel.transition = kind;
                } else {
                    throw sol::error("no transition is called '" + *transition + "'");
                }
            }
            travel.duration = std::max(duration.value_or(travel.duration), 0.0f);
            flow->go(std::move(travel));
            return true;
        };
        scene_table["going"] = [this]() { return flow && flow->pending(); };

        sol::table world_table = lua.create_named_table("world");
        world_table["find"] = [this](const std::string& name) -> std::optional<ScriptEntity> {
            const EcsEntity found = world ? world->entities().find_by_name(name) : EcsEntity{};
            return found.valid() ? std::optional(ScriptEntity{world, found.raw()}) : std::nullopt;
        };
        world_table["spawn"] = [this](const std::string& prefab, sol::optional<f32> x, sol::optional<f32> y) -> std::optional<ScriptEntity> {
            const std::shared_ptr<const PrefabAsset> asset = prefab_of(prefab);
            if (!asset || !world) {
                throw sol::error("the prefab '" + prefab + "' could not be read");
            }
            const PrefabInstance made = instantiate_prefab(*world, *asset, PrefabSpawn{.pos = {x.value_or(0.0f), y.value_or(0.0f)}},
                                                           world->components());
            return made.root.valid() ? std::optional(ScriptEntity{world, made.root.raw()}) : std::nullopt;
        };
        lua.safe_script(self_prelude);
    }

    std::shared_ptr<const PrefabAsset> prefab_of(const std::string& path) {
        if (const auto found = prefabs.find(path); found != prefabs.end()) {
            return found->second;
        }
        const PrefabLoadResult loaded = load_prefab_asset(config.content / path, world->components());
        std::shared_ptr<const PrefabAsset> asset = loaded.prefab ? std::make_shared<const PrefabAsset>(*loaded.prefab) : nullptr;
        prefabs.emplace(path, asset);
        return asset;
    }

    // A script file, read (or read again): false when it does not read.
    File& file(const std::string& path) {
        auto [found, fresh] = files.try_emplace(path);
        File& made = found->second;
        if (fresh) {
            made.script = std::make_unique<LuaScript>(LuaScriptOptions{
                .setup = [this](sol::state& lua) { bind(lua); },
                .module_root = config.content,
                .on_error = [this, path](std::string_view message) {
                    KIN_LOG_WARN_F("script", "script error", (LogFields{{.name = "script", .value = path}, {.name = "error", .value = std::string(message)}}));
                    problems[path] = std::string(message);
                },
            });
            read(path, made);
        }
        return made;
    }

    void read(const std::string& path, File& made) {
        if (made.script->load_file(config.content / path)) {
            problems.erase(path);
            ++made.generation;
        }
        made.times.clear();
        for (const std::filesystem::path& dependency : made.script->dependencies()) {
            made.times.emplace_back(dependency, time_of(dependency));
        }
        if (made.times.empty()) {
            made.times.emplace_back(config.content / path, time_of(config.content / path));
        }
    }

    void reload_changed() {
        for (auto& [path, made] : files) {
            const bool stale = std::ranges::any_of(made.times, [](const auto& file) { return time_of(file.first) != file.second; });
            if (stale) {
                // Its entities' selves let go first: they live in the state a read may replace.
                std::erase_if(running, [&](const auto& entry) { return entry.second.path == path; });
                read(path, made); // they start again at the next update
            }
        }
        prefabs.clear(); // read again when next spawned
    }
};

EntityScripts::EntityScripts(EntityScriptsConfig config) : _impl(std::make_unique<Impl>()) {
    _impl->config = std::move(config);
}

EntityScripts::~EntityScripts() {
    _impl->running.clear(); // the selves before the states they live in
}

const std::map<std::string, std::string>& EntityScripts::problems() const {
    return _impl->problems;
}

std::size_t EntityScripts::running() const {
    return _impl->running.size();
}

void EntityScripts::restart() {
    _impl->running.clear();
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

void EntityScripts::update(EcsWorld& world, Input& input, f32 dt) {
    Impl& impl = *_impl;
    impl.world = &world;
    impl.input = &input;
    if (impl.config.check_frames > 0 && ++impl.frames >= impl.config.check_frames) {
        impl.frames = 0;
        impl.reload_changed();
    }
    // The scripted entities now (a script may make or destroy some as it runs).
    std::vector<std::pair<flecs::entity, std::string>> scripted;
    world.raw().each([&](flecs::entity entity, const ScriptComponent& script) {
        if (script.enabled && !script.asset.id.empty() && entity.enabled()) {
            scripted.emplace_back(entity, script.asset.id);
        }
    });
    // Those that no longer run theirs: gone.
    std::erase_if(impl.running, [&](const auto& entry) {
        const auto found = std::ranges::find_if(scripted, [&](const auto& item) { return item.first.id() == entry.first; });
        return found == scripted.end() || found->second != entry.second.path;
    });
    for (const auto& [entity, path] : scripted) {
        if (!entity.is_alive()) {
            continue;
        }
        Impl::File& file = impl.file(path);
        if (!file.script->loaded()) {
            continue; // it does not read: its problem is told
        }
        auto [found, fresh] = impl.running.try_emplace(entity.id());
        Impl::Running& run = found->second;
        if (fresh || run.generation != file.generation) {
            // Its self, in the file's state as read now.
            run = {.path = path, .generation = file.generation, .self = {}, .started = false};
            const sol::protected_function make = file.script->lua()["__kin_self"];
            const sol::protected_function_result made = make(ScriptEntity{&world, entity});
            if (!made.valid()) {
                continue;
            }
            run.self = made.get<sol::table>();
        }
        if (!run.started) {
            run.started = true;
            file.script->call("on_start", run.self);
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
        // What its body began touching this step.
        if (entity.is_alive() && file.script->has("on_contact")) {
            if (const PhysicsContactQueue* queue = entity.get<PhysicsContactQueue>(); queue && !queue->events.empty()) {
                const PhysicsBodyComponent* mine = entity.get<PhysicsBodyComponent>();
                std::vector<PhysicsBody> touched;
                for (const PhysicsContactEvent& event : queue->events) {
                    if (event.type == PhysicsContactEventType::Begin) {
                        touched.push_back(mine && event.body_a == mine->body ? event.body_b : event.body_a);
                    }
                }
                for (const PhysicsBody body : touched) {
                    flecs::entity other;
                    world.raw().each([&](flecs::entity candidate, const PhysicsBodyComponent& component) {
                        if (!other && component.body == body) {
                            other = candidate;
                        }
                    });
                    if (other && entity.is_alive()) {
                        file.script->call("on_contact", run.self, ScriptEntity{&world, other});
                    }
                }
            }
        }
#endif
    }
    impl.world = nullptr;
    impl.input = nullptr;
}

} // namespace kin
