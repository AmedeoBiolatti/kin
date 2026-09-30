#include <kin/ecs/component.hpp>

#include <algorithm>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace kin {
namespace {

flecs::entity entity_from_id(const EcsWorld& world, EcsId id) {
    return flecs::entity{const_cast<flecs::world&>(world.raw()).c_ptr(), static_cast<flecs::entity_t>(id)};
}

std::string entity_name(flecs::entity entity) {
    const flecs::string_view value = entity.name();
    return {value.c_str(), value.length()};
}

// Inspection order: root entities first, then children grouped by parent id,
// each group by name, then id. Reading parent and name is a flecs lookup, so
// they are read once per entity into a key rather than once per comparison.
struct WorldEntityKey {
    u64 parent = 0; // 0 for roots, which sort first (entity ids are never 0)
    std::string_view name;
    u64 id = 0;
    flecs::entity entity;

    bool operator<(const WorldEntityKey& other) const {
        if (parent != other.parent) {
            return parent < other.parent;
        }
        if (name != other.name) {
            return name < other.name;
        }
        return id < other.id;
    }
};

void sort_world_entities(std::vector<flecs::entity>& entities) {
    std::vector<WorldEntityKey> keys;
    keys.reserve(entities.size());
    for (const flecs::entity entity : entities) {
        const flecs::entity parent = entity.parent();
        const flecs::string_view name = entity.name();
        keys.push_back({.parent = parent ? parent.id() : 0,
                        .name = {name.c_str(), name.length()},
                        .id = entity.id(),
                        .entity = entity});
    }
    std::sort(keys.begin(), keys.end());
    for (std::size_t i = 0; i < keys.size(); ++i) {
        entities[i] = keys[i].entity;
    }
}

bool is_editor_internal_entity(flecs::entity entity) {
    const flecs::string_view raw_name = entity.name();
    const std::string_view name{raw_name.c_str(), raw_name.length()};
    return entity.has<flecs::Component>() ||
           name.starts_with("kin::ecs::phase::") ||
           name.starts_with("kin::ecs::data::") ||
           name.starts_with("kin::prefab::");
}

detail::DataComponentPayload* ensure_data_payload(EcsEntity entity) {
    if (const detail::DataComponentPayload* inherited = entity.get<detail::DataComponentPayload>();
        inherited && !entity.raw().owns<detail::DataComponentPayload>()) {
        entity.set(*inherited);
    }
    detail::DataComponentPayload* payload = entity.get_mut<detail::DataComponentPayload>();
    if (!payload) {
        entity.set(detail::DataComponentPayload{});
        payload = entity.get_mut<detail::DataComponentPayload>();
    }
    return payload;
}

std::vector<ComponentFieldValue>* data_values_for(EcsEntity entity, ComponentId component_id) {
    detail::DataComponentPayload* payload = ensure_data_payload(entity);
    if (!payload) {
        return nullptr;
    }
    const auto found = payload->values.find(component_id);
    return found == payload->values.end() ? nullptr : &found->second;
}

const std::vector<ComponentFieldValue>* data_values_for_const(EcsEntity entity, ComponentId component_id) {
    const detail::DataComponentPayload* payload = entity.get<detail::DataComponentPayload>();
    if (!payload) {
        return nullptr;
    }
    const auto found = payload->values.find(component_id);
    return found == payload->values.end() ? nullptr : &found->second;
}

std::vector<flecs::entity> sorted_world_entities(const EcsWorld& world) {
    std::vector<flecs::entity> entities;
    const auto add_entity = [&](EcsEntity entity) {
        if (entity && !is_editor_internal_entity(entity.raw())) {
            entities.push_back(entity.raw());
        }
    };
    for (EcsEntity entity : world.entities().all()) {
        add_entity(entity);
    }
    for (EcsEntity entity : world.components().entities()) {
        add_entity(entity);
    }
    const_cast<flecs::world&>(world.raw()).each([&](flecs::entity entity) {
        add_entity(EcsEntity{entity});
    });
    sort_world_entities(entities);
    entities.erase(std::ranges::unique(entities, [](flecs::entity a, flecs::entity b) {
        return a.id() == b.id();
    }).begin(), entities.end());
    return entities;
}

std::string field_kind_name(ComponentFieldKind kind) {
    switch (kind) {
    case ComponentFieldKind::Bool: return "bool";
    case ComponentFieldKind::I32: return "i32";
    case ComponentFieldKind::I64: return "i64";
    case ComponentFieldKind::U32: return "u32";
    case ComponentFieldKind::U64: return "u64";
    case ComponentFieldKind::F32: return "f32";
    case ComponentFieldKind::F64: return "f64";
    case ComponentFieldKind::String: return "string";
    case ComponentFieldKind::Vec2f: return "vec2f";
    case ComponentFieldKind::Vec2i: return "vec2i";
    case ComponentFieldKind::Color: return "color";
    case ComponentFieldKind::EntityRef: return "entity_ref";
    case ComponentFieldKind::AssetRef: return "asset_ref";
    }
    return "unknown";
}

std::optional<ComponentFieldKind> field_kind_from_name(std::string_view name) {
    if (name == "bool") return ComponentFieldKind::Bool;
    if (name == "i32") return ComponentFieldKind::I32;
    if (name == "i64") return ComponentFieldKind::I64;
    if (name == "u32") return ComponentFieldKind::U32;
    if (name == "u64") return ComponentFieldKind::U64;
    if (name == "f32") return ComponentFieldKind::F32;
    if (name == "f64") return ComponentFieldKind::F64;
    if (name == "string") return ComponentFieldKind::String;
    if (name == "vec2f") return ComponentFieldKind::Vec2f;
    if (name == "vec2i") return ComponentFieldKind::Vec2i;
    if (name == "color") return ComponentFieldKind::Color;
    if (name == "entity_ref") return ComponentFieldKind::EntityRef;
    if (name == "asset_ref") return ComponentFieldKind::AssetRef;
    return std::nullopt;
}

void write_field_json(JsonWriter& json, const ComponentFieldSnapshot& field) {
    json.begin_object();
    json.field("name", std::string_view{field.name});
    json.field("kind", field_kind_name(field.kind));
    json.key("value");
    write_component_field_value_json(json, field.value);
    json.end_object();
}

std::optional<ComponentFieldSnapshot> field_from_json(const JsonValue& value, std::string& error) {
    if (!value.is_object()) {
        error = "component field entry must be an object";
        return std::nullopt;
    }
    const std::string name = value.string_at("name");
    const std::string kind_name = value.string_at("kind");
    const JsonValue* field_value = value.find("value");
    const std::optional<ComponentFieldKind> kind = field_kind_from_name(kind_name);
    if (name.empty()) {
        error = "component field name is required";
        return std::nullopt;
    }
    if (!kind) {
        error = "unknown component field kind '" + kind_name + "'";
        return std::nullopt;
    }
    if (!field_value) {
        error = "component field '" + name + "' is missing value";
        return std::nullopt;
    }
    std::optional<ComponentFieldValue> converted = component_field_value_from_json(*kind, *field_value, error);
    if (!converted) {
        return std::nullopt;
    }
    return ComponentFieldSnapshot{
        .name = name,
        .kind = *kind,
        .value = std::move(*converted),
    };
}

std::optional<SceneDocument> scene_document_from_json_value(const JsonValue& value, std::string& error) {
    const JsonValue* entities = value.find("entities");
    if (!entities || !entities->is_array()) {
        error = "scene document requires an entities array";
        return std::nullopt;
    }

    SceneDocument document;
    for (const JsonValue& entity_value : entities->items()) {
        if (!entity_value.is_object()) {
            error = "scene entity entry must be an object";
            return std::nullopt;
        }
        SceneEntityDocument entity{
            .id = entity_value.string_at("id"),
            .name = entity_value.string_at("name"),
            .parent = entity_value.string_at("parent"),
        };
        if (entity.id.empty()) {
            error = "scene entity id is required";
            return std::nullopt;
        }
        const JsonValue* components = entity_value.find("components");
        if (components && !components->is_array()) {
            error = "scene entity components must be an array";
            return std::nullopt;
        }
        if (components) {
            for (const JsonValue& component_value : components->items()) {
                if (!component_value.is_object()) {
                    error = "scene component entry must be an object";
                    return std::nullopt;
                }
                SceneComponentDocument component{
                    .name = component_value.string_at("name"),
                };
                if (component.name.empty()) {
                    error = "scene component name is required";
                    return std::nullopt;
                }
                const JsonValue* fields = component_value.find("fields");
                if (fields && !fields->is_array()) {
                    error = "scene component fields must be an array";
                    return std::nullopt;
                }
                if (fields) {
                    for (const JsonValue& field_value : fields->items()) {
                        std::optional<ComponentFieldSnapshot> field = field_from_json(field_value, error);
                        if (!field) {
                            return std::nullopt;
                        }
                        component.fields.push_back(std::move(*field));
                    }
                }
                entity.components.push_back(std::move(component));
            }
        }
        document.entities.push_back(std::move(entity));
    }
    return document;
}

ComponentFieldValue document_field_value(EcsWorld& world, const ComponentFieldValue& value) {
    if (const ComponentEntityRef* ref = std::get_if<ComponentEntityRef>(&value)) {
        ComponentEntityRef document_ref = *ref;
        if (document_ref.authored_id.empty() && document_ref.id != 0) {
            EcsEntity entity = world.entities().find_by_id(document_ref.id);
            if (entity) {
                document_ref.authored_id = world.entities().authored_id(entity).value_or(EntityAuthoredId{});
            }
        }
        return document_ref;
    }
    return value;
}

bool resolve_entity_ref_value(EcsWorld& world,
                              const std::unordered_map<std::string, EcsEntity>& created,
                              ComponentFieldValue& value,
                              std::string& error) {
    ComponentEntityRef* ref = std::get_if<ComponentEntityRef>(&value);
    if (!ref || ref->authored_id.empty()) {
        return true;
    }
    if (const auto found = created.find(ref->authored_id); found != created.end()) {
        ref->id = found->second.id();
        return true;
    }
    EcsEntity existing = world.entities().find_by_authored_id(ref->authored_id);
    if (existing) {
        ref->id = existing.id();
        return true;
    }
    error = "unknown entity reference '" + ref->authored_id + "'";
    return false;
}

} // namespace

std::string component_field_kind_name(ComponentFieldKind kind) {
    return field_kind_name(kind);
}

std::optional<ComponentFieldKind> component_field_kind_from_name(std::string_view name) {
    return field_kind_from_name(name);
}

EcsComponentRegistry::EcsComponentRegistry(EcsWorld& world)
    : _world(&world) {
    _world->component<detail::DataComponentPayload>("kin::ecs::data::Payload")
        .add(flecs::OnInstantiate, flecs::Inherit);
}

EcsComponentRegistry::~EcsComponentRegistry() = default;

EcsComponentRegistry::DataComponentBuilder EcsComponentRegistry::data(std::string name) {
    if (name.empty()) {
        _last_error = "component name is required";
        return DataComponentBuilder{*this, nullptr};
    }
    if (find_record(name) != nullptr) {
        _last_error = "duplicate component '" + name + "'";
        return DataComponentBuilder{*this, nullptr};
    }

    flecs::entity component = _world->raw().entity(("kin::ecs::data::" + name).c_str());
    component.add(flecs::OnInstantiate, flecs::Inherit);
    auto record = std::make_shared<detail::ComponentRecord>();
    record->descriptor = ComponentDescriptor{
        .id = static_cast<ComponentId>(component.id()),
        .name = std::move(name),
        .kind = ComponentKind::Data,
    };
    record->has = [component_id = record->descriptor.id](EcsEntity entity) {
        return entity.raw().has(static_cast<flecs::id_t>(component_id)) ||
               data_values_for_const(entity, component_id) != nullptr;
    };
    record->add = [component_id = record->descriptor.id,
                   defaults = &record->data_defaults](EcsEntity entity) {
        entity.raw().add(static_cast<flecs::id_t>(component_id));
        if (detail::DataComponentPayload* payload = ensure_data_payload(entity)) {
            payload->values[component_id] = *defaults;
        }
    };
    record->remove = [component_id = record->descriptor.id](EcsEntity entity) {
        if (detail::DataComponentPayload* payload = entity.get_mut<detail::DataComponentPayload>()) {
            payload->values.erase(component_id);
            if (payload->values.empty()) {
                entity.remove<detail::DataComponentPayload>();
            }
        }
        entity.raw().remove(static_cast<flecs::id_t>(component_id));
    };
    record->mutable_ptr = [component_id = record->descriptor.id](EcsEntity entity) -> void* {
        return data_values_for(entity, component_id);
    };
    record->const_ptr = [component_id = record->descriptor.id](EcsEntity entity) -> const void* {
        return data_values_for_const(entity, component_id);
    };
    record->modified = [](EcsEntity entity) {
        entity.modified<detail::DataComponentPayload>();
    };
    record->collect_entities = [this, component_id = record->descriptor.id](std::vector<EcsEntity>& entities) {
        const_cast<flecs::world&>(_world->raw()).each([&](flecs::entity entity) {
            if (entity.has(static_cast<flecs::id_t>(component_id))) {
                entities.push_back(EcsEntity{entity});
            }
        });
    };
    record->registration_sequence = _next_registration_sequence++;

    detail::ComponentRecord* record_ptr = record.get();
    _by_id[record->descriptor.id] = record;
    _by_name[record->descriptor.name] = record;
    _records.push_back(std::move(record));
    _last_error.clear();
    return DataComponentBuilder{*this, record_ptr};
}

const ComponentDescriptor* EcsComponentRegistry::find(ComponentId id) const {
    const detail::ComponentRecord* record = find_record(id);
    return record ? &record->descriptor : nullptr;
}

const ComponentDescriptor* EcsComponentRegistry::find(std::string_view name) const {
    const detail::ComponentRecord* record = find_record(name);
    return record ? &record->descriptor : nullptr;
}

std::vector<ComponentDescriptor> EcsComponentRegistry::descriptors() const {
    std::vector<const detail::ComponentRecord*> sorted;
    sorted.reserve(_records.size());
    for (const std::shared_ptr<detail::ComponentRecord>& record : _records) {
        sorted.push_back(record.get());
    }
    std::ranges::sort(sorted, [](const detail::ComponentRecord* a, const detail::ComponentRecord* b) {
        if (a->registration_sequence != b->registration_sequence) {
            return a->registration_sequence < b->registration_sequence;
        }
        return a->descriptor.name < b->descriptor.name;
    });

    std::vector<ComponentDescriptor> result;
    result.reserve(sorted.size());
    for (const detail::ComponentRecord* record : sorted) {
        result.push_back(record->descriptor);
    }
    return result;
}

bool EcsComponentRegistry::has(EcsEntity entity, ComponentId id) const {
    const detail::ComponentRecord* record = find_record(id);
    if (!record) {
        return set_error("unknown component id '" + std::to_string(id) + "'");
    }
    _last_error.clear();
    return record->has(entity);
}

bool EcsComponentRegistry::has(EcsEntity entity, std::string_view name) const {
    const detail::ComponentRecord* record = find_record(name);
    if (!record) {
        return set_error("unknown component '" + std::string{name} + "'");
    }
    _last_error.clear();
    return record->has(entity);
}

bool EcsComponentRegistry::add(EcsEntity entity, ComponentId id) {
    detail::ComponentRecord* record = find_record(id);
    if (!record) {
        return set_error("unknown component id '" + std::to_string(id) + "'");
    }
    // Only register a suppression when the OnAdd observer will actually fire,
    // i.e. when the entity does not already own the component. (A component that
    // is merely inherited via IsA is not owned, so adding an owned copy still
    // fires OnAdd and must be suppressed; re-adding an already-owned component
    // fires nothing, so a suppression there would leak.)
    if (record->observed && !entity.raw().owns(static_cast<flecs::id_t>(record->descriptor.id))) {
        _world->events().suppress_next_observer(EcsEventKind::ComponentAdded, entity.id(), record->descriptor.id);
    }
    record->add(entity);
    _world->events().emit_component(EcsEventKind::ComponentAdded,
                                    EcsEventSource::KinApi,
                                    entity,
                                    record->descriptor.id,
                                    record->descriptor.name);
    _last_error.clear();
    return true;
}

bool EcsComponentRegistry::add(EcsEntity entity, std::string_view name) {
    detail::ComponentRecord* record = find_record(name);
    if (!record) {
        return set_error("unknown component '" + std::string{name} + "'");
    }
    return add(entity, record->descriptor.id);
}

bool EcsComponentRegistry::remove(EcsEntity entity, ComponentId id) {
    detail::ComponentRecord* record = find_record(id);
    if (!record) {
        return set_error("unknown component id '" + std::to_string(id) + "'");
    }
    // Symmetric to add(): OnRemove only fires when the entity owns the
    // component, so only suppress in that case.
    if (record->observed && entity.raw().owns(static_cast<flecs::id_t>(record->descriptor.id))) {
        _world->events().suppress_next_observer(EcsEventKind::ComponentRemoved, entity.id(), record->descriptor.id);
    }
    record->remove(entity);
    _world->events().emit_component(EcsEventKind::ComponentRemoved,
                                    EcsEventSource::KinApi,
                                    entity,
                                    record->descriptor.id,
                                    record->descriptor.name);
    _last_error.clear();
    return true;
}

bool EcsComponentRegistry::remove(EcsEntity entity, std::string_view name) {
    detail::ComponentRecord* record = find_record(name);
    if (!record) {
        return set_error("unknown component '" + std::string{name} + "'");
    }
    return remove(entity, record->descriptor.id);
}

std::optional<ComponentSnapshot> EcsComponentRegistry::snapshot(EcsEntity entity, ComponentId id) const {
    const detail::ComponentRecord* record = find_record(id);
    if (!record) {
        set_error("unknown component id '" + std::to_string(id) + "'");
        return std::nullopt;
    }

    const void* raw = record->const_ptr(entity);
    if (!raw) {
        set_error("entity does not have component '" + record->descriptor.name + "'");
        return std::nullopt;
    }

    ComponentSnapshot result{
        .descriptor = record->descriptor,
    };
    result.fields.reserve(record->fields.size());
    for (const detail::ComponentFieldRecord& field : record->fields) {
        result.fields.push_back({
            .name = field.descriptor.name,
            .kind = field.descriptor.kind,
            .value = field.read(raw),
        });
    }
    _last_error.clear();
    return result;
}

std::optional<ComponentSnapshot> EcsComponentRegistry::snapshot(EcsEntity entity, std::string_view name) const {
    const detail::ComponentRecord* record = find_record(name);
    if (!record) {
        set_error("unknown component '" + std::string{name} + "'");
        return std::nullopt;
    }
    return snapshot(entity, record->descriptor.id);
}

bool EcsComponentRegistry::visit_fields(EcsEntity entity,
                                        ComponentId id,
                                        const FieldVisitor& visitor) const {
    const detail::ComponentRecord* record = find_record(id);
    if (!record) {
        return set_error("unknown component id '" + std::to_string(id) + "'");
    }
    const void* raw = record->const_ptr(entity);
    if (!raw) {
        return set_error("entity does not have component '" + record->descriptor.name + "'");
    }
    for (const detail::ComponentFieldRecord& field : record->fields) {
        const ComponentFieldValue value = field.read(raw);
        visitor(field.descriptor.name, field.descriptor.kind, value);
    }
    _last_error.clear();
    return true;
}

bool EcsComponentRegistry::visit_fields(EcsEntity entity,
                                        std::string_view name,
                                        const FieldVisitor& visitor) const {
    const detail::ComponentRecord* record = find_record(name);
    if (!record) {
        return set_error("unknown component '" + std::string{name} + "'");
    }
    return visit_fields(entity, record->descriptor.id, visitor);
}

std::vector<ComponentSnapshot> EcsComponentRegistry::snapshots(EcsEntity entity) const {
    std::vector<ComponentSnapshot> result;
    for (const ComponentDescriptor& descriptor : descriptors()) {
        const detail::ComponentRecord* record = find_record(descriptor.id);
        if (!record || !record->has(entity)) {
            continue;
        }
        if (std::optional<ComponentSnapshot> item = snapshot(entity, descriptor.id)) {
            result.push_back(std::move(*item));
        }
    }
    _last_error.clear();
    return result;
}

std::vector<EcsEntity> EcsComponentRegistry::entities() const {
    std::vector<EcsEntity> collected;
    for (const std::shared_ptr<detail::ComponentRecord>& record : _records) {
        record->collect_entities(collected);
    }
    std::ranges::sort(collected, [](EcsEntity a, EcsEntity b) {
        if (a.id() != b.id()) {
            return a.id() < b.id();
        }
        return a.name() < b.name();
    });
    collected.erase(std::ranges::unique(collected, [](EcsEntity a, EcsEntity b) {
        return a.id() == b.id();
    }).begin(), collected.end());
    return collected;
}

bool EcsComponentRegistry::patch_field(EcsEntity entity, ComponentId id, std::string_view field_name, const ComponentFieldValue& value) {
    detail::ComponentRecord* record = find_record(id);
    if (!record) {
        return set_error("unknown component id '" + std::to_string(id) + "'");
    }

    void* raw = record->mutable_ptr(entity);
    if (!raw) {
        return set_error("entity does not have component '" + record->descriptor.name + "'");
    }

    const auto found = std::ranges::find_if(record->fields, [&](const detail::ComponentFieldRecord& field) {
        return field.descriptor.name == field_name;
    });
    if (found == record->fields.end()) {
        return set_error("unknown field '" + std::string{field_name} + "' on component '" + record->descriptor.name + "'");
    }
    if (!found->patch(raw, value)) {
        return set_error("type mismatch for field '" + std::string{field_name} + "' on component '" + record->descriptor.name + "'");
    }

    if (record->observed) {
        _world->events().suppress_next_observer(EcsEventKind::ComponentChanged, entity.id(), record->descriptor.id);
    }
    record->modified(entity);
    _world->events().emit_component(EcsEventKind::ComponentChanged,
                                    EcsEventSource::KinApi,
                                    entity,
                                    record->descriptor.id,
                                    record->descriptor.name,
                                    std::string{field_name});
    _last_error.clear();
    return true;
}

bool EcsComponentRegistry::patch_field(EcsEntity entity,
                                       std::string_view component,
                                       std::string_view field,
                                       const ComponentFieldValue& value) {
    detail::ComponentRecord* record = find_record(component);
    if (!record) {
        return set_error("unknown component '" + std::string{component} + "'");
    }
    return patch_field(entity, record->descriptor.id, field, value);
}

bool EcsComponentRegistry::migrate_data_schema(std::string_view component, const std::vector<ComponentFieldSnapshot>& fields) {
    detail::ComponentRecord* record = find_record(component);
    if (!record) {
        return set_error("unknown component '" + std::string{component} + "'");
    }
    if (record->descriptor.kind != ComponentKind::Data) {
        return set_error("component '" + record->descriptor.name + "' is not a data component");
    }

    std::unordered_set<std::string> names;
    for (const ComponentFieldSnapshot& field : fields) {
        if (field.name.empty()) {
            return set_error("component field name is required");
        }
        if (!names.insert(field.name).second) {
            return set_error("duplicate field '" + field.name + "' on component '" + record->descriptor.name + "'");
        }
        if (!detail::field_value_matches_kind(field.value, field.kind)) {
            return set_error("default value type mismatch for field '" + field.name + "' on component '" + record->descriptor.name + "'");
        }
    }

    std::unordered_map<std::string, std::size_t> old_indices;
    for (std::size_t i = 0; i < record->descriptor.fields.size(); ++i) {
        old_indices[record->descriptor.fields[i].name] = i;
    }

    const ComponentId component_id = record->descriptor.id;
    std::vector<EcsEntity> entities = _world->entities().all();
    for (EcsEntity entity : entities) {
        std::vector<ComponentFieldValue>* values = data_values_for(entity, component_id);
        if (!values) {
            continue;
        }
        std::vector<ComponentFieldValue> migrated;
        migrated.reserve(fields.size());
        for (const ComponentFieldSnapshot& field : fields) {
            if (const auto found = old_indices.find(field.name);
                found != old_indices.end() &&
                found->second < record->descriptor.fields.size() &&
                record->descriptor.fields[found->second].kind == field.kind &&
                found->second < values->size()) {
                migrated.push_back((*values)[found->second]);
            } else {
                migrated.push_back(field.value);
            }
        }
        *values = std::move(migrated);
        entity.modified<detail::DataComponentPayload>();
    }

    record->descriptor.fields.clear();
    record->fields.clear();
    record->data_defaults.clear();
    for (const ComponentFieldSnapshot& field : fields) {
        const std::size_t index = record->data_defaults.size();
        detail::ComponentFieldRecord field_record;
        field_record.descriptor = {.name = field.name, .kind = field.kind};
        field_record.read = [index](const void* raw) -> ComponentFieldValue {
            const auto& values = *static_cast<const std::vector<ComponentFieldValue>*>(raw);
            // The values vector is normally sized to the field count, but an
            // entity that received the component before a schema migration (and
            // was not reachable by the migration pass) can be shorter. Guard the
            // index rather than read out of bounds.
            if (index >= values.size()) {
                return {};
            }
            return values[index];
        };
        field_record.patch = [index, kind = field.kind](void* raw, const ComponentFieldValue& value) -> bool {
            if (!detail::field_value_matches_kind(value, kind)) {
                return false;
            }
            auto& values = *static_cast<std::vector<ComponentFieldValue>*>(raw);
            if (index >= values.size()) {
                return false;
            }
            values[index] = value;
            return true;
        };
        record->descriptor.fields.push_back(field_record.descriptor);
        record->fields.push_back(std::move(field_record));
        record->data_defaults.push_back(field.value);
    }
    _last_error.clear();
    return true;
}

detail::ComponentRecord* EcsComponentRegistry::find_record(ComponentId id) {
    const auto found = _by_id.find(id);
    return found == _by_id.end() ? nullptr : found->second.get();
}

const detail::ComponentRecord* EcsComponentRegistry::find_record(ComponentId id) const {
    const auto found = _by_id.find(id);
    return found == _by_id.end() ? nullptr : found->second.get();
}

detail::ComponentRecord* EcsComponentRegistry::find_record(std::string_view name) {
    const auto found = _by_name.find(std::string{name});
    return found == _by_name.end() ? nullptr : found->second.get();
}

const detail::ComponentRecord* EcsComponentRegistry::find_record(std::string_view name) const {
    const auto found = _by_name.find(std::string{name});
    return found == _by_name.end() ? nullptr : found->second.get();
}

bool EcsComponentRegistry::set_error(std::string error) const {
    _last_error = std::move(error);
    return false;
}

EcsRelationRegistry::EcsRelationRegistry(EcsWorld& world)
    : _world(&world) {
}

EcsRelationRegistry::~EcsRelationRegistry() = default;

RelationId EcsRelationRegistry::relation(std::string name) {
    if (name.empty()) {
        set_error("relation name is required");
        return 0;
    }
    if (const RelationDescriptor* existing = find(name)) {
        _last_error.clear();
        return existing->id;
    }
    flecs::entity relation = _world->raw().entity(("kin::ecs::relation::" + name).c_str());
    RelationDescriptor descriptor{
        .id = static_cast<RelationId>(relation.id()),
        .name = std::move(name),
    };
    const std::size_t index = _records.size();
    _by_id[descriptor.id] = index;
    _by_name[descriptor.name] = index;
    _records.push_back(std::move(descriptor));
    _last_error.clear();
    return _records.back().id;
}

const RelationDescriptor* EcsRelationRegistry::find(RelationId id) const {
    const auto found = _by_id.find(id);
    return found == _by_id.end() ? nullptr : &_records[found->second];
}

const RelationDescriptor* EcsRelationRegistry::find(std::string_view name) const {
    const auto found = _by_name.find(std::string{name});
    return found == _by_name.end() ? nullptr : &_records[found->second];
}

std::vector<RelationDescriptor> EcsRelationRegistry::descriptors() const {
    return _records;
}

bool EcsRelationRegistry::add(EcsEntity source, std::string_view relation_name, EcsEntity target) {
    if (!source || !target) {
        return set_error("cannot add relation pair with invalid entity");
    }
    RelationId id = relation(std::string{relation_name});
    if (id == 0) {
        return false;
    }
    source.raw().add(static_cast<flecs::id_t>(id), target.raw());
    _last_error.clear();
    return true;
}

bool EcsRelationRegistry::remove(EcsEntity source, std::string_view relation_name, EcsEntity target) {
    const RelationDescriptor* descriptor = find(relation_name);
    if (!descriptor) {
        return set_error("unknown relation '" + std::string{relation_name} + "'");
    }
    if (!source || !target) {
        return set_error("cannot remove relation pair with invalid entity");
    }
    source.raw().remove(static_cast<flecs::id_t>(descriptor->id), target.raw());
    _last_error.clear();
    return true;
}

bool EcsRelationRegistry::has(EcsEntity source, std::string_view relation_name, EcsEntity target) const {
    const RelationDescriptor* descriptor = find(relation_name);
    if (!descriptor) {
        return set_error("unknown relation '" + std::string{relation_name} + "'");
    }
    if (!source || !target) {
        return set_error("cannot check relation pair with invalid entity");
    }
    _last_error.clear();
    return source.raw().has(static_cast<flecs::id_t>(descriptor->id), target.raw());
}

std::vector<RelationPairSnapshot> EcsRelationRegistry::snapshots(EcsEntity source) const {
    std::vector<RelationPairSnapshot> result;
    if (!source) {
        return result;
    }
    for (const RelationDescriptor& relation : _records) {
        for (EcsEntity target : _world->entities().all()) {
            if (!target || !source.raw().has(static_cast<flecs::id_t>(relation.id), target.raw())) {
                continue;
            }
            result.push_back({
                .relation = relation,
                .target = target.id(),
                .target_name = target.name(),
                .target_authored_id = _world->entities().authored_id(target).value_or(EntityAuthoredId{}),
            });
        }
    }
    return result;
}

bool EcsRelationRegistry::set_error(std::string error) const {
    _last_error = std::move(error);
    return false;
}

EcsComponentRegistry::DataComponentBuilder& EcsComponentRegistry::DataComponentBuilder::field(
    std::string name,
    ComponentFieldKind kind,
    ComponentFieldValue default_value) {
    if (!_record) {
        return *this;
    }
    if (name.empty()) {
        _registry._last_error = "component field name is required";
        return *this;
    }
    if (!detail::field_value_matches_kind(default_value, kind)) {
        _registry._last_error = "default value type mismatch for field '" + name + "' on component '" + _record->descriptor.name + "'";
        return *this;
    }
    for (const detail::ComponentFieldRecord& field : _record->fields) {
        if (field.descriptor.name == name) {
            _registry._last_error = "duplicate field '" + name + "' on component '" + _record->descriptor.name + "'";
            return *this;
        }
    }

    const std::size_t index = _record->data_defaults.size();
    detail::ComponentFieldRecord record;
    record.descriptor = ComponentFieldDescriptor{
        .name = std::move(name),
        .kind = kind,
    };
    record.read = [index](const void* raw) -> ComponentFieldValue {
        const auto& values = *static_cast<const std::vector<ComponentFieldValue>*>(raw);
        // Guard against a values vector shorter than the field count (an entity
        // that predates a schema migration the migration pass could not reach).
        if (index >= values.size()) {
            return {};
        }
        return values[index];
    };
    record.patch = [index, kind](void* raw, const ComponentFieldValue& value) -> bool {
        if (!detail::field_value_matches_kind(value, kind)) {
            return false;
        }
        auto& values = *static_cast<std::vector<ComponentFieldValue>*>(raw);
        if (index >= values.size()) {
            return false;
        }
        values[index] = value;
        return true;
    };
    _record->descriptor.fields.push_back(record.descriptor);
    _record->fields.push_back(std::move(record));
    _record->data_defaults.push_back(std::move(default_value));
    _registry._last_error.clear();
    return *this;
}

EcsComponentRegistry::DataComponentBuilder& EcsComponentRegistry::DataComponentBuilder::label(std::string value) {
    return update_last_field([&](ComponentFieldDescriptor& field) { field.label = std::move(value); });
}

EcsComponentRegistry::DataComponentBuilder& EcsComponentRegistry::DataComponentBuilder::category(std::string value) {
    return update_last_field([&](ComponentFieldDescriptor& field) { field.category = std::move(value); });
}

EcsComponentRegistry::DataComponentBuilder& EcsComponentRegistry::DataComponentBuilder::description(std::string value) {
    return update_last_field([&](ComponentFieldDescriptor& field) { field.description = std::move(value); });
}

EcsComponentRegistry::DataComponentBuilder& EcsComponentRegistry::DataComponentBuilder::units(std::string value) {
    return update_last_field([&](ComponentFieldDescriptor& field) { field.units = std::move(value); });
}

EcsComponentRegistry::DataComponentBuilder& EcsComponentRegistry::DataComponentBuilder::readonly(bool value) {
    return update_last_field([&](ComponentFieldDescriptor& field) { field.readonly = value; });
}

EcsComponentRegistry::DataComponentBuilder& EcsComponentRegistry::DataComponentBuilder::hidden(bool value) {
    return update_last_field([&](ComponentFieldDescriptor& field) { field.hidden = value; });
}

EcsComponentRegistry::DataComponentBuilder& EcsComponentRegistry::DataComponentBuilder::range(f64 min, f64 max) {
    return update_last_field([&](ComponentFieldDescriptor& field) {
        field.min_value = min;
        field.max_value = max;
    });
}

template <typename Func>
EcsComponentRegistry::DataComponentBuilder& EcsComponentRegistry::DataComponentBuilder::update_last_field(Func&& func) {
    if (!_record || _record->descriptor.fields.empty() || _record->fields.empty()) {
        _registry._last_error = "component field metadata requires a preceding field";
        return *this;
    }
    func(_record->descriptor.fields.back());
    _record->fields.back().descriptor = _record->descriptor.fields.back();
    _registry._last_error.clear();
    return *this;
}

EntitySnapshot EcsWorld::snapshot(EcsEntity entity) const {
    EntitySnapshot result;
    if (!entity.valid()) {
        return result;
    }

    result.descriptor.id = entity.id();
    result.descriptor.authored_id = entities().authored_id(entity).value_or(EntityAuthoredId{});
    result.descriptor.name = entity.name();
    result.descriptor.alive = entity.alive();
    result.descriptor.enabled = entity.raw().enabled();
    result.descriptor.registered = entities().registered(entity);
    result.descriptor.handle_generation = entities().generation(entity);
    result.descriptor.creation_sequence = entities().creation_sequence(entity);
    result.descriptor.parent = entity.parent() ? entity.parent().id() : 0;
    entity.raw().children([&](flecs::entity child) {
        if (!is_editor_internal_entity(child)) {
            result.descriptor.children.push_back(static_cast<EcsId>(child.id()));
        }
    });
    std::ranges::sort(result.descriptor.children);
    result.components = components().snapshots(entity);
    return result;
}

WorldSnapshot EcsWorld::snapshot() const {
    return snapshot(WorldSnapshotOptions{});
}

WorldSnapshot EcsWorld::snapshot(const WorldSnapshotOptions& options) const {
    WorldSnapshot result{
        .components = components().descriptors(),
    };
    for (flecs::entity entity : sorted_world_entities(*this)) {
        EntitySnapshot item = snapshot(EcsEntity{entity});
        if (!options.include_components) {
            item.components.clear();
        }
        result.entities.push_back(std::move(item));
    }
    return result;
}

EcsQueryPlan EcsWorld::build_query_plan(const EcsQueryPlanDescriptor& descriptor) const {
    EcsQueryPlan plan{
        .valid = true,
        .reads = descriptor.reads,
        .writes = descriptor.writes,
    };

    const auto add_component = [&](std::string_view name, std::vector<ComponentId>& out, std::string_view term) {
        const ComponentDescriptor* component = components().find(name);
        if (!component) {
            plan.valid = false;
            plan.diagnostics.push_back("unknown " + std::string{term} + " component '" + std::string{name} + "'");
            return;
        }
        out.push_back(component->id);
    };
    for (const std::string& name : descriptor.all) {
        add_component(name, plan.all, "all");
    }
    for (const std::string& name : descriptor.none) {
        add_component(name, plan.none, "none");
    }
    if (plan.valid) {
        auto builder = const_cast<flecs::world&>(raw()).query_builder();
        for (const ComponentId id : plan.all) {
            builder.with(static_cast<flecs::id_t>(id));
        }
        for (const ComponentId id : plan.none) {
            builder.without(static_cast<flecs::id_t>(id));
        }
        auto compiled = std::make_shared<flecs::query<>>(builder.cached().build());
        _compiled_queries.push_back(compiled);
        plan.compiled = compiled;
        plan.matched_count = static_cast<i32>(query_entities(plan).size());
    }
    return plan;
}

std::vector<EcsEntity> EcsWorld::query_entities(const EcsQueryPlan& plan) const {
    std::vector<EcsEntity> result;
    if (!plan.valid) {
        return result;
    }
    if (const std::shared_ptr<flecs::query<>> compiled = plan.compiled.lock()) {
        std::vector<flecs::entity> matched;
        compiled->each([&](flecs::entity entity) {
            if (!is_editor_internal_entity(entity)) {
                matched.push_back(entity);
            }
        });
        sort_world_entities(matched);
        result.reserve(matched.size());
        for (const flecs::entity entity : matched) {
            result.push_back(EcsEntity{entity});
        }
        return result;
    }
    for (flecs::entity entity : sorted_world_entities(*this)) {
        bool matches = true;
        for (const ComponentId id : plan.all) {
            if (!entity.has(static_cast<flecs::id_t>(id))) {
                matches = false;
                break;
            }
        }
        if (!matches) {
            continue;
        }
        for (const ComponentId id : plan.none) {
            if (entity.has(static_cast<flecs::id_t>(id))) {
                matches = false;
                break;
            }
        }
        if (matches) {
            result.push_back(EcsEntity{entity});
        }
    }
    return result;
}

void write_component_field_value_json(JsonWriter& json, const ComponentFieldValue& value) {
    std::visit([&](const auto& typed) {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<T, Vec2f>) {
            json.begin_object().field("x", static_cast<f64>(typed.x)).field("y", static_cast<f64>(typed.y)).end_object();
        } else if constexpr (std::is_same_v<T, Vec2i>) {
            json.begin_object().field("x", typed.x).field("y", typed.y).end_object();
        } else if constexpr (std::is_same_v<T, Color>) {
            json.begin_object()
                .field("r", static_cast<u32>(typed.r))
                .field("g", static_cast<u32>(typed.g))
                .field("b", static_cast<u32>(typed.b))
                .field("a", static_cast<u32>(typed.a))
                .end_object();
        } else if constexpr (std::is_same_v<T, ComponentEntityRef>) {
            if (!typed.authored_id.empty()) {
                json.value(std::string_view{typed.authored_id});
            } else {
                json.value(typed.id);
            }
        } else if constexpr (std::is_same_v<T, ComponentAssetRef>) {
            json.value(std::string_view{typed.id});
        } else if constexpr (std::is_same_v<T, std::string>) {
            json.value(std::string_view{typed});
        } else if constexpr (std::is_same_v<T, f32>) {
            json.value(static_cast<f64>(typed));
        } else {
            json.value(typed);
        }
    }, value);
}

void write_component_snapshot_json(JsonWriter& json, const ComponentSnapshot& snapshot) {
    json.begin_object();
    json.field("id", snapshot.descriptor.id);
    json.field("name", std::string_view{snapshot.descriptor.name});
    json.key("fields").begin_array();
    for (const ComponentFieldSnapshot& field : snapshot.fields) {
        write_field_json(json, field);
    }
    json.end_array();
    json.end_object();
}

void write_entity_snapshot_json(JsonWriter& json, const EntitySnapshot& snapshot) {
    json.begin_object();
    json.field("id", snapshot.descriptor.id);
    json.field("authored_id", std::string_view{snapshot.descriptor.authored_id});
    json.field("name", std::string_view{snapshot.descriptor.name});
    json.field("alive", snapshot.descriptor.alive);
    json.field("enabled", snapshot.descriptor.enabled);
    json.field("registered", snapshot.descriptor.registered);
    json.field("handle_generation", snapshot.descriptor.handle_generation);
    json.field("creation_sequence", snapshot.descriptor.creation_sequence);
    json.field("parent", snapshot.descriptor.parent);
    json.key("children").begin_array();
    for (const EcsId child : snapshot.descriptor.children) {
        json.value(child);
    }
    json.end_array();
    json.key("components").begin_array();
    for (const ComponentSnapshot& component : snapshot.components) {
        write_component_snapshot_json(json, component);
    }
    json.end_array();
    json.end_object();
}

void write_world_snapshot_json(JsonWriter& json, const WorldSnapshot& snapshot) {
    json.begin_object();
    json.key("components").begin_array();
    for (const ComponentDescriptor& component : snapshot.components) {
        json.begin_object();
        json.field("id", component.id);
        json.field("name", std::string_view{component.name});
        json.key("fields").begin_array();
        for (const ComponentFieldDescriptor& field : component.fields) {
            json.begin_object();
            json.field("name", std::string_view{field.name});
            json.field("kind", field_kind_name(field.kind));
            if (!field.label.empty()) json.field("label", std::string_view{field.label});
            if (!field.category.empty()) json.field("category", std::string_view{field.category});
            if (!field.description.empty()) json.field("description", std::string_view{field.description});
            if (!field.units.empty()) json.field("units", std::string_view{field.units});
            if (field.readonly) json.field("readonly", field.readonly);
            if (field.hidden) json.field("hidden", field.hidden);
            if (field.min_value) json.field("min", *field.min_value);
            if (field.max_value) json.field("max", *field.max_value);
            json.end_object();
        }
        json.end_array();
        json.end_object();
    }
    json.end_array();
    json.key("entities").begin_array();
    for (const EntitySnapshot& entity : snapshot.entities) {
        write_entity_snapshot_json(json, entity);
    }
    json.end_array();
    json.end_object();
}

void write_query_plan_json(JsonWriter& json, const EcsQueryPlan& plan) {
    json.begin_object();
    json.field("valid", plan.valid);
    json.field("matched_count", plan.matched_count);
    json.key("diagnostics").begin_array();
    for (const std::string& diagnostic : plan.diagnostics) {
        json.value(std::string_view{diagnostic});
    }
    json.end_array();
    json.end_object();
}

std::optional<ComponentFieldValue> component_field_value_from_json(ComponentFieldKind kind,
                                                                   const JsonValue& value,
                                                                   std::string& error) {
    const auto number_member = [&](std::string_view name) -> const JsonValue* {
        const JsonValue* member = value.find(name);
        return member && member->is_number() ? member : nullptr;
    };
    const auto number_at = [&](std::size_t index) -> const JsonValue* {
        if (!value.is_array() || index >= value.items().size() || !value.items()[index].is_number()) {
            return nullptr;
        }
        return &value.items()[index];
    };
    const auto color_channel = [](const JsonValue& channel) {
        return static_cast<u8>(std::clamp<i64>(channel.as_int(), 0, 255));
    };

    switch (kind) {
    case ComponentFieldKind::Bool:
        if (!value.is_bool()) break;
        return ComponentFieldValue{value.as_bool()};
    case ComponentFieldKind::I32:
        if (!value.is_number()) break;
        return ComponentFieldValue{static_cast<i32>(value.as_int())};
    case ComponentFieldKind::I64:
        if (!value.is_number()) break;
        return ComponentFieldValue{static_cast<i64>(value.as_int())};
    case ComponentFieldKind::U32:
        if (!value.is_number()) break;
        return ComponentFieldValue{static_cast<u32>(std::max<i64>(0, value.as_int()))};
    case ComponentFieldKind::U64:
        if (!value.is_number()) break;
        return ComponentFieldValue{static_cast<u64>(std::max<i64>(0, value.as_int()))};
    case ComponentFieldKind::F32:
        if (!value.is_number()) break;
        return ComponentFieldValue{static_cast<f32>(value.as_number())};
    case ComponentFieldKind::F64:
        if (!value.is_number()) break;
        return ComponentFieldValue{static_cast<f64>(value.as_number())};
    case ComponentFieldKind::String:
        if (!value.is_string()) break;
        return ComponentFieldValue{value.as_string()};
    case ComponentFieldKind::Vec2f:
        if (const JsonValue* x = number_member("x"); x != nullptr) {
            if (const JsonValue* y = number_member("y"); y != nullptr) {
                return ComponentFieldValue{Vec2f{static_cast<f32>(x->as_number()), static_cast<f32>(y->as_number())}};
            }
        }
        if (const JsonValue* x = number_at(0); x != nullptr) {
            if (const JsonValue* y = number_at(1); y != nullptr) {
                return ComponentFieldValue{Vec2f{static_cast<f32>(x->as_number()), static_cast<f32>(y->as_number())}};
            }
        }
        break;
    case ComponentFieldKind::Vec2i:
        if (const JsonValue* x = number_member("x"); x != nullptr) {
            if (const JsonValue* y = number_member("y"); y != nullptr) {
                return ComponentFieldValue{Vec2i{static_cast<i32>(x->as_int()), static_cast<i32>(y->as_int())}};
            }
        }
        if (const JsonValue* x = number_at(0); x != nullptr) {
            if (const JsonValue* y = number_at(1); y != nullptr) {
                return ComponentFieldValue{Vec2i{static_cast<i32>(x->as_int()), static_cast<i32>(y->as_int())}};
            }
        }
        break;
    case ComponentFieldKind::Color: {
        const JsonValue* r = number_member("r");
        const JsonValue* g = number_member("g");
        const JsonValue* b = number_member("b");
        const JsonValue* a = number_member("a");
        if (r && g && b) {
            return ComponentFieldValue{Color::rgba(color_channel(*r),
                                                   color_channel(*g),
                                                   color_channel(*b),
                                                   a ? color_channel(*a) : 255)};
        }
        r = number_at(0);
        g = number_at(1);
        b = number_at(2);
        a = number_at(3);
        if (r && g && b) {
            return ComponentFieldValue{Color::rgba(color_channel(*r),
                                                   color_channel(*g),
                                                   color_channel(*b),
                                                   a ? color_channel(*a) : 255)};
        }
        break;
    }
    case ComponentFieldKind::EntityRef:
        if (value.is_string()) return ComponentFieldValue{ComponentEntityRef{.authored_id = value.as_string()}};
        if (value.is_number()) return ComponentFieldValue{ComponentEntityRef{.id = static_cast<EcsId>(value.as_int())}};
        if (value.is_object()) {
            return ComponentFieldValue{ComponentEntityRef{
                .id = static_cast<EcsId>(value.int_at("id")),
                .authored_id = value.string_at("authored_id"),
            }};
        }
        break;
    case ComponentFieldKind::AssetRef:
        if (value.is_string()) return ComponentFieldValue{ComponentAssetRef{.id = value.as_string()}};
        if (value.is_object()) return ComponentFieldValue{ComponentAssetRef{.id = value.string_at("id")}};
        break;
    }
    error = "invalid JSON value for field kind '" + field_kind_name(kind) + "'";
    return std::nullopt;
}

std::vector<ComponentFieldSnapshot> parse_component_fields(
    std::string_view component_name,
    const JsonValue& json,
    const EcsComponentRegistry& components,
    std::vector<std::string>& diagnostics) {
    std::vector<ComponentFieldSnapshot> fields;
    const ComponentDescriptor* descriptor = components.find(component_name);
    if (!descriptor) {
        diagnostics.push_back("unknown component '" + std::string{component_name} + "'");
        return fields;
    }
    if (!json.is_object()) {
        diagnostics.push_back("component '" + std::string{component_name} + "' must be an object");
        return fields;
    }

    for (const auto& [field_name, field_json] : json.members()) {
        const auto found = std::ranges::find_if(descriptor->fields, [&](const ComponentFieldDescriptor& field) {
            return field.name == field_name;
        });
        if (found == descriptor->fields.end()) {
            diagnostics.push_back("unknown field '" + field_name + "' on component '" + std::string{component_name} + "'");
            continue;
        }
        std::string error;
        std::optional<ComponentFieldValue> value = component_field_value_from_json(found->kind, field_json, error);
        if (!value) {
            diagnostics.push_back("invalid field '" + field_name + "' on component '" + std::string{component_name} + "'");
            continue;
        }
        fields.push_back({
            .name = field_name,
            .kind = found->kind,
            .value = std::move(*value),
        });
    }
    return fields;
}

SceneDocument scene_document_from_world(EcsWorld& world) {
    SceneDocument document;
    WorldSnapshot snapshot = world.snapshot();
    std::unordered_map<EcsId, std::string> ids;
    for (const EntitySnapshot& entity : snapshot.entities) {
        ids[entity.descriptor.id] = entity.descriptor.authored_id.empty()
            ? "entity." + std::to_string(entity.descriptor.id)
            : entity.descriptor.authored_id;
    }
    for (const EntitySnapshot& entity : snapshot.entities) {
        SceneEntityDocument item{
            .id = ids[entity.descriptor.id],
            .name = entity.descriptor.name,
            .parent = entity.descriptor.parent != 0 && ids.contains(entity.descriptor.parent) ? ids[entity.descriptor.parent] : std::string{},
        };
        for (const ComponentSnapshot& component : entity.components) {
            SceneComponentDocument component_doc{
                .name = component.descriptor.name,
            };
            component_doc.fields.reserve(component.fields.size());
            for (ComponentFieldSnapshot field : component.fields) {
                field.value = document_field_value(world, field.value);
                component_doc.fields.push_back(std::move(field));
            }
            item.components.push_back(std::move(component_doc));
        }
        document.entities.push_back(std::move(item));
    }
    return document;
}

void write_scene_document_json(JsonWriter& json, const SceneDocument& document) {
    json.begin_object();
    json.key("entities").begin_array();
    for (const SceneEntityDocument& entity : document.entities) {
        json.begin_object();
        json.field("id", std::string_view{entity.id});
        json.field("name", std::string_view{entity.name});
        if (!entity.parent.empty()) {
            json.field("parent", std::string_view{entity.parent});
        }
        json.key("components").begin_array();
        for (const SceneComponentDocument& component : entity.components) {
            json.begin_object();
            json.field("name", std::string_view{component.name});
            json.key("fields").begin_array();
            for (const ComponentFieldSnapshot& field : component.fields) {
                write_field_json(json, field);
            }
            json.end_array();
            json.end_object();
        }
        json.end_array();
        json.end_object();
    }
    json.end_array();
    json.end_object();
}

std::string serialize_scene(EcsWorld& world) {
    std::ostringstream out;
    JsonWriter json(out, false);
    write_scene_document_json(json, scene_document_from_world(world));
    return out.str();
}

bool instantiate_scene(EcsWorld& world, const SceneDocument& document, std::string& error) {
    std::unordered_set<std::string> ids;
    for (const SceneEntityDocument& entity : document.entities) {
        if (entity.id.empty()) {
            error = "scene entity id is required";
            return false;
        }
        if (!ids.insert(entity.id).second) {
            error = "duplicate scene entity id '" + entity.id + "'";
            return false;
        }
        if (world.entities().find_by_authored_id(entity.id)) {
            error = "duplicate authored entity id '" + entity.id + "'";
            return false;
        }
        if (!entity.parent.empty() && !ids.contains(entity.parent)) {
            const bool parent_exists_later = std::ranges::any_of(document.entities, [&](const SceneEntityDocument& candidate) {
                return candidate.id == entity.parent;
            });
            if (!parent_exists_later) {
                error = "unknown parent id '" + entity.parent + "'";
                return false;
            }
        }
        for (const SceneComponentDocument& component : entity.components) {
            const ComponentDescriptor* descriptor = world.components().find(component.name);
            if (!descriptor) {
                error = "unknown component '" + component.name + "'";
                return false;
            }
            for (const ComponentFieldSnapshot& field : component.fields) {
                const auto found = std::ranges::find_if(descriptor->fields, [&](const ComponentFieldDescriptor& registered) {
                    return registered.name == field.name;
                });
                if (found == descriptor->fields.end()) {
                    error = "unknown field '" + field.name + "' on component '" + component.name + "'";
                    return false;
                }
                if (found->kind != field.kind) {
                    error = "field kind mismatch for '" + field.name + "' on component '" + component.name + "'";
                    return false;
                }
            }
        }
    }

    std::unordered_map<std::string, EcsEntity> created;
    for (const SceneEntityDocument& entity : document.entities) {
        EcsEntity created_entity = world.entities().create({
            .name = entity.name,
            .authored_id = entity.id,
        });
        if (!created_entity) {
            error = world.entities().last_error();
            return false;
        }
        created[entity.id] = created_entity;
    }
    for (const SceneEntityDocument& entity : document.entities) {
        EcsEntity created_entity = created.at(entity.id);
        if (!entity.parent.empty()) {
            created_entity.child_of(created.at(entity.parent));
        }
        for (const SceneComponentDocument& component : entity.components) {
            if (!world.components().add(created_entity, component.name)) {
                error = world.components().last_error();
                return false;
            }
            for (const ComponentFieldSnapshot& field : component.fields) {
                ComponentFieldValue value = field.value;
                if (!resolve_entity_ref_value(world, created, value, error)) {
                    return false;
                }
                if (!world.components().patch_field(created_entity, component.name, field.name, value)) {
                    error = world.components().last_error();
                    return false;
                }
            }
        }
    }
    error.clear();
    return true;
}

} // namespace kin
