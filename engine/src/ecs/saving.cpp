#include <kin/ecs/saving.hpp>

#include <kin/core/json.hpp>
#include <kin/ecs/component.hpp>
#include <kin/ecs/render.hpp>
#include <kin/prefab/prefab_asset.hpp>
#include <kin/prefab/prefab_registry.hpp>
#if KIN_ENABLE_PHYSICS
#include <kin/ecs/physics.hpp>
#endif

#include <algorithm>
#include <sstream>
#include <utility>

namespace kin {

namespace {

constexpr std::string_view payload_schema = "kin.scene_saves/1";

std::string trimmed(std::string_view text) {
    const std::size_t begin = text.find_first_not_of(" \t");
    if (begin == std::string_view::npos) {
        return {};
    }
    const std::size_t end = text.find_last_not_of(" \t");
    return std::string(text.substr(begin, end - begin + 1));
}

// A field's value as a save holds it: as scene files write it.
JsonValue field_json(const ComponentFieldValue& value) {
    std::ostringstream text;
    JsonWriter json(text, false);
    write_component_field_value_json(json, value);
    JsonParseResult parsed = parse_json(text.str());
    return parsed.value ? std::move(*parsed.value) : JsonValue{};
}

std::optional<std::string> authored_id_of(EcsWorld& world, EcsEntity entity) {
    std::optional<std::string> id = world.entities().authored_id(entity);
    if (id && id->empty()) {
        id.reset();
    }
    return id;
}

} // namespace

std::vector<std::string> saved_components(const Saved& saved) {
    std::vector<std::string> names;
    std::string_view rest = saved.keep;
    while (!rest.empty()) {
        const std::size_t comma = rest.find(',');
        std::string name = trimmed(rest.substr(0, comma));
        if (!name.empty() && std::ranges::find(names, name) == names.end()) {
            names.push_back(std::move(name));
        }
        rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
    }
    return names;
}

SceneSaves::SceneSaves(SaveStoreConfig config, std::filesystem::path content)
    : _store(std::move(config)),
      _content(std::move(content)) {
}

JsonValue SceneSaves::entity_state(EcsWorld& world, EcsEntity entity, const Saved& saved) const {
    JsonValue::Object state;
    EcsComponentRegistry& components = world.components();
    for (const std::string& name : saved_components(saved)) {
        const ComponentDescriptor* descriptor = components.find(name);
        if (!descriptor) {
            continue; // no such component: nothing to keep (the studio says so)
        }
        if (!components.has(entity, name)) {
            state[name] = JsonValue{}; // kept as gone
            continue;
        }
        JsonValue::Object fields;
        components.visit_fields(entity, name, [&](std::string_view field, ComponentFieldKind, const ComponentFieldValue& value) {
            const auto described = std::ranges::find(descriptor->fields, field, &ComponentFieldDescriptor::name);
            if (described != descriptor->fields.end() && described->readonly) {
                return; // what the game works out (a body's handle): not the game's to set
            }
            fields[std::string(field)] = field_json(value);
        });
        state[name] = JsonValue{std::move(fields)};
    }
    if (saved.script && _scripts.write) {
        if (std::optional<JsonValue> values = _scripts.write(entity)) {
            state["script"] = std::move(*values);
        }
    }
    return JsonValue{std::move(state)};
}

void SceneSaves::put_back(EcsWorld& world, EcsEntity entity, const JsonValue& state) {
    if (!state.is_object()) {
        return;
    }
    EcsComponentRegistry& components = world.components();
    const std::string who = std::string(entity.raw().name().c_str());
    bool moved = false;
    for (const auto& [name, fields] : state.members()) {
        if (name == "script" || name == "gone" || name == "prefab") {
            continue;
        }
        const ComponentDescriptor* descriptor = components.find(name);
        if (!descriptor) {
            _problems.push_back(who + ": no component is called " + name + " now");
            continue;
        }
        if (fields.is_null()) {
            if (components.has(entity, name)) {
                components.remove(entity, name);
            }
            continue;
        }
        if (!components.has(entity, name) && !components.add(entity, name)) {
            _problems.push_back(who + ": " + name + " could not be added (" + components.last_error() + ")");
            continue;
        }
        for (const auto& [field, json] : fields.members()) {
            const auto described = std::ranges::find(descriptor->fields, field, &ComponentFieldDescriptor::name);
            if (described == descriptor->fields.end()) {
                _problems.push_back(who + ": " + name + " has no field " + field + " now");
                continue;
            }
            std::string error;
            const std::optional<ComponentFieldValue> value = component_field_value_from_json(described->kind, json, error);
            if (!value || !components.patch_field(entity, name, field, *value)) {
                _problems.push_back(who + ": " + name + "." + field + " could not be set" +
                                    (error.empty() ? std::string{} : " (" + error + ")"));
            }
        }
        moved = moved || name == "Transform2D";
    }
#if KIN_ENABLE_PHYSICS
    if (moved && entity.raw().has<PhysicsBodyComponent>()) {
        entity.raw().add<PhysicsTeleport>(); // its body with it, at the next step
    }
#else
    (void)moved;
#endif
    if (const JsonValue* script = state.find("script"); script && _scripts.read) {
        _scripts.read(entity, *script);
    }
}

void SceneSaves::enter(EcsWorld& world, std::string scene) {
    _scene = std::move(scene);
    _problems.clear();
    _noted.clear();
    std::vector<std::pair<std::string, EcsEntity>> marked;
    world.each([&](flecs::entity raw, const Saved&) {
        const EcsEntity entity{raw};
        if (std::optional<std::string> id = authored_id_of(world, entity)) {
            marked.emplace_back(std::move(*id), entity);
        }
    });
    for (auto& [id, entity] : marked) {
        _noted.insert(std::move(id));
    }
    const auto found = _scenes.find(_scene);
    if (found == _scenes.end()) {
        return;
    }
    const JsonValue state = found->second; // a copy: putting back may run scripts' hooks
    if (const JsonValue* entities = state.find("entities"); entities && entities->is_object()) {
        for (const auto& [id, kept] : entities->members()) {
            const EcsEntity entity = world.entities().find_by_authored_id(id);
            if (!entity || !entity.raw().is_alive()) {
                continue; // no longer in the scene's file
            }
            if (kept.bool_at("gone")) {
                world.entities().destroy(entity);
                continue;
            }
            put_back(world, entity, kept);
        }
    }
    if (const JsonValue* spawned = state.find("spawned"); spawned && spawned->is_array()) {
        std::map<std::string, std::optional<PrefabAsset>, std::less<>> prefabs;
        for (const JsonValue& kept : spawned->items()) {
            const std::string path = kept.string_at("prefab");
            auto [loaded, fresh] = prefabs.try_emplace(path);
            if (fresh) {
                PrefabLoadResult read = load_prefab_asset(_content / path, world.components());
                if (!read.prefab) {
                    _problems.push_back(path + " could not be read to make again");
                }
                loaded->second = std::move(read.prefab);
            }
            if (!loaded->second) {
                continue;
            }
            const PrefabInstance made = instantiate_prefab(world, *loaded->second, PrefabSpawn{}, world.components());
            if (!made.root) {
                _problems.push_back(path + " could not be made again");
                continue;
            }
            made.root.raw().set<SpawnedPrefab>({.prefab = path});
            put_back(world, made.root, kept);
        }
    }
}

void SceneSaves::keep(EcsWorld& world) {
    if (_scene.empty()) {
        return;
    }
    JsonValue::Object entities;
    JsonValue::Array spawned;
    std::vector<std::pair<EcsEntity, Saved>> marked;
    world.each([&](flecs::entity raw, const Saved& saved) {
        marked.emplace_back(EcsEntity{raw}, saved);
    });
    for (const auto& [entity, saved] : marked) {
        if (!entity.raw().is_alive()) {
            continue;
        }
        if (std::optional<std::string> id = authored_id_of(world, entity)) {
            entities[*id] = entity_state(world, entity, saved);
        } else if (const SpawnedPrefab* from = entity.raw().get<SpawnedPrefab>(); from && !from->prefab.empty()) {
            JsonValue state = entity_state(world, entity, saved);
            JsonValue::Object members = state.members();
            members["prefab"] = JsonValue{from->prefab};
            spawned.emplace_back(std::move(members));
        }
    }
    // Those the scene had that are gone now.
    for (const std::string& id : _noted) {
        if (!entities.contains(id)) {
            const EcsEntity entity = world.entities().find_by_authored_id(id);
            if (!entity || !entity.raw().is_alive()) {
                entities[id] = JsonValue{JsonValue::Object{{"gone", JsonValue{true}}}};
            }
        }
    }
    JsonValue::Object state;
    state["entities"] = JsonValue{std::move(entities)};
    if (!spawned.empty()) {
        state["spawned"] = JsonValue{std::move(spawned)};
    }
    _scenes[_scene] = JsonValue{std::move(state)};
}

const JsonValue* SceneSaves::value(std::string_view name) const {
    const auto found = _values.find(std::string(name));
    return found == _values.end() ? nullptr : &found->second;
}

void SceneSaves::set_value(const std::string& name, JsonValue value) {
    if (value.is_null()) {
        _values.erase(name);
    } else {
        _values[name] = std::move(value);
    }
}

void SceneSaves::clear() {
    _scenes.clear();
    _values.clear();
    _played = 0.0;
}

const JsonValue* SceneSaves::kept(std::string_view scene) const {
    const auto found = _scenes.find(scene);
    return found == _scenes.end() ? nullptr : &found->second;
}

SaveResult SceneSaves::write(EcsWorld& world, std::string_view slot, std::string title, std::string summary) {
    keep(world);
    SaveSlotInfo info;
    if (const SaveLoadResult earlier = _store.read_slot(slot); earlier.result.ok) {
        info = earlier.info; // its creation time kept
    }
    info.slot_id = std::string(slot);
    info.title = std::move(title);
    info.summary = std::move(summary);
    info.play_time_seconds = static_cast<i64>(_played);
    return _store.write_slot(slot, info, [&](JsonWriter& json) {
        json.begin_object();
        json.field("schema", payload_schema);
        json.field("scene", std::string_view{_scene});
        json.key("values");
        write_json(json, JsonValue{_values});
        json.key("scenes").begin_object();
        for (const auto& [scene, state] : _scenes) {
            json.key(scene);
            write_json(json, state);
        }
        json.end_object();
        json.end_object();
    });
}

std::optional<std::string> SceneSaves::read(std::string_view slot, std::string& error) {
    SaveLoadResult read = _store.read_slot(slot);
    if (!read.result.ok) {
        error = read.result.message.empty() ? "the slot could not be read" : read.result.message;
        return std::nullopt;
    }
    const JsonValue& payload = read.payload;
    if (!payload.is_object() || payload.string_at("schema") != payload_schema) {
        error = "the slot is not a scene save (" + std::string(payload_schema) + ")";
        return std::nullopt;
    }
    std::string scene = payload.string_at("scene");
    if (scene.empty()) {
        error = "the slot names no scene";
        return std::nullopt;
    }
    _scenes.clear();
    if (const JsonValue* scenes = payload.find("scenes"); scenes && scenes->is_object()) {
        for (const auto& [name, state] : scenes->members()) {
            _scenes.emplace(name, state);
        }
    }
    _values.clear();
    if (const JsonValue* values = payload.find("values"); values && values->is_object()) {
        _values = values->members();
    }
    _played = static_cast<f64>(read.info.play_time_seconds);
    _scene.clear(); // what is in the world now is not kept over what was read
    return scene;
}

std::optional<std::string> SceneSaves::take_read() {
    std::optional<std::string> asked = std::move(_asked);
    _asked.reset();
    return asked;
}

} // namespace kin
