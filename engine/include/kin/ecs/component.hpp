#pragma once

#include <kin/core/types.hpp>
#include <kin/core/json.hpp>
#include <kin/core/json_value.hpp>
#include <kin/ecs/world.hpp>
#include <kin/renderer/color.hpp>

#include <flecs.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace kin {

enum class ComponentKind {
    Native,
    Data,
};

enum class ComponentFieldKind {
    Bool,
    I32,
    I64,
    U32,
    U64,
    F32,
    F64,
    String,
    Vec2f,
    Vec2i,
    Color,
    EntityRef,
    AssetRef,
};

struct ComponentEntityRef {
    EcsId id = 0;
    EntityAuthoredId authored_id;

    friend bool operator==(const ComponentEntityRef&, const ComponentEntityRef&) = default;
};

struct ComponentAssetRef {
    std::string id;

    friend bool operator==(const ComponentAssetRef&, const ComponentAssetRef&) = default;
};

using ComponentFieldValue = std::variant<bool,
                                         i32,
                                         i64,
                                         u32,
                                         u64,
                                         f32,
                                         f64,
                                         std::string,
                                         Vec2f,
                                         Vec2i,
                                         Color,
                                         ComponentEntityRef,
                                         ComponentAssetRef>;

struct ComponentFieldDescriptor {
    std::string name;
    ComponentFieldKind kind = ComponentFieldKind::Bool;
    std::string label;
    std::string category;
    std::string description;
    std::string units;
    bool readonly = false;
    bool hidden = false;
    std::optional<f64> min_value;
    std::optional<f64> max_value;
};

struct ComponentDescriptor {
    ComponentId id = 0;
    std::string name;
    ComponentKind kind = ComponentKind::Native;
    std::vector<ComponentFieldDescriptor> fields;
};

struct ComponentFieldSnapshot {
    std::string name;
    ComponentFieldKind kind = ComponentFieldKind::Bool;
    ComponentFieldValue value = false;
};

struct ComponentSnapshot {
    ComponentDescriptor descriptor;
    std::vector<ComponentFieldSnapshot> fields;
};

struct EntityDescriptor {
    EcsId id = 0;
    EntityAuthoredId authored_id;
    std::string name;
    bool alive = false;
    bool enabled = true;
    bool registered = false;
    u64 handle_generation = 0;
    u64 creation_sequence = 0;
    EcsId parent = 0;
    std::vector<EcsId> children;
};

struct EntitySnapshot {
    EntityDescriptor descriptor;
    std::vector<ComponentSnapshot> components;
};

struct WorldSnapshotOptions {
    bool include_components = true;
};

struct WorldSnapshot {
    std::vector<ComponentDescriptor> components;
    std::vector<EntitySnapshot> entities;
};

struct EcsQueryPlanDescriptor {
    std::vector<std::string> all;
    std::vector<std::string> none;
    std::vector<std::string> reads;
    std::vector<std::string> writes;
};

struct EcsQueryPlan {
    bool valid = false;
    std::vector<std::string> diagnostics;
    std::vector<ComponentId> all;
    std::vector<ComponentId> none;
    std::vector<std::string> reads;
    std::vector<std::string> writes;
    i32 matched_count = 0;
    // Cached Flecs matcher: execution visits matching tables instead of
    // reconstructing and filtering the complete world on every run.
    std::weak_ptr<flecs::query<>> compiled;
};

struct SceneComponentDocument {
    std::string name;
    std::vector<ComponentFieldSnapshot> fields;
};

struct SceneEntityDocument {
    std::string id;
    std::string name;
    std::string parent;
    std::vector<SceneComponentDocument> components;
};

struct SceneDocument {
    std::vector<SceneEntityDocument> entities;
};

namespace detail {

template <typename T>
struct ComponentFieldKindOf;

template <> struct ComponentFieldKindOf<bool> { static constexpr ComponentFieldKind value = ComponentFieldKind::Bool; };
template <> struct ComponentFieldKindOf<i32> { static constexpr ComponentFieldKind value = ComponentFieldKind::I32; };
template <> struct ComponentFieldKindOf<i64> { static constexpr ComponentFieldKind value = ComponentFieldKind::I64; };
template <> struct ComponentFieldKindOf<u32> { static constexpr ComponentFieldKind value = ComponentFieldKind::U32; };
template <> struct ComponentFieldKindOf<u64> { static constexpr ComponentFieldKind value = ComponentFieldKind::U64; };
template <> struct ComponentFieldKindOf<f32> { static constexpr ComponentFieldKind value = ComponentFieldKind::F32; };
template <> struct ComponentFieldKindOf<f64> { static constexpr ComponentFieldKind value = ComponentFieldKind::F64; };
template <> struct ComponentFieldKindOf<std::string> { static constexpr ComponentFieldKind value = ComponentFieldKind::String; };
template <> struct ComponentFieldKindOf<Vec2f> { static constexpr ComponentFieldKind value = ComponentFieldKind::Vec2f; };
template <> struct ComponentFieldKindOf<Vec2i> { static constexpr ComponentFieldKind value = ComponentFieldKind::Vec2i; };
template <> struct ComponentFieldKindOf<Color> { static constexpr ComponentFieldKind value = ComponentFieldKind::Color; };

// Field types a native component member can be reflected as. These are exactly
// the types ComponentFieldKindOf is specialized for above. Entity/asset refs are
// intentionally absent: they have dedicated authored-id serialization and are
// authored via the data-component path (DataComponentBuilder::field_entity_ref /
// field_asset_ref), not as native member fields. Keeping this list in sync with
// ComponentFieldKindOf ensures an unsupported native field fails on the
// static_assert in NativeComponentBuilder::field() with a clear message rather
// than an opaque "ComponentFieldKindOf has no member 'value'" error.
template <typename T>
inline constexpr bool supported_component_field_v =
    std::is_same_v<T, bool> ||
    std::is_same_v<T, i32> ||
    std::is_same_v<T, i64> ||
    std::is_same_v<T, u32> ||
    std::is_same_v<T, u64> ||
    std::is_same_v<T, f32> ||
    std::is_same_v<T, f64> ||
    std::is_same_v<T, std::string> ||
    std::is_same_v<T, Vec2f> ||
    std::is_same_v<T, Vec2i> ||
    std::is_same_v<T, Color>;

inline bool field_value_matches_kind(const ComponentFieldValue& value, ComponentFieldKind kind) {
    switch (kind) {
    case ComponentFieldKind::Bool: return std::holds_alternative<bool>(value);
    case ComponentFieldKind::I32: return std::holds_alternative<i32>(value);
    case ComponentFieldKind::I64: return std::holds_alternative<i64>(value);
    case ComponentFieldKind::U32: return std::holds_alternative<u32>(value);
    case ComponentFieldKind::U64: return std::holds_alternative<u64>(value);
    case ComponentFieldKind::F32: return std::holds_alternative<f32>(value);
    case ComponentFieldKind::F64: return std::holds_alternative<f64>(value);
    case ComponentFieldKind::String: return std::holds_alternative<std::string>(value);
    case ComponentFieldKind::Vec2f: return std::holds_alternative<Vec2f>(value);
    case ComponentFieldKind::Vec2i: return std::holds_alternative<Vec2i>(value);
    case ComponentFieldKind::Color: return std::holds_alternative<Color>(value);
    case ComponentFieldKind::EntityRef: return std::holds_alternative<ComponentEntityRef>(value);
    case ComponentFieldKind::AssetRef: return std::holds_alternative<ComponentAssetRef>(value);
    }
    return false;
}

struct ComponentFieldRecord {
    ComponentFieldDescriptor descriptor;
    std::function<ComponentFieldValue(const void*)> read;
    std::function<bool(void*, const ComponentFieldValue&)> patch;
};

struct DataComponentPayload {
    std::unordered_map<ComponentId, std::vector<ComponentFieldValue>> values;
};

struct ComponentRecord {
    ComponentDescriptor descriptor;
    std::type_index type = typeid(void);
    std::function<bool(EcsEntity)> has;
    std::function<void(EcsEntity)> add;
    std::function<void(EcsEntity)> remove;
    std::function<void*(EcsEntity)> mutable_ptr;
    std::function<const void*(EcsEntity)> const_ptr;
    std::function<void(EcsEntity)> modified;
    std::function<void(std::vector<EcsEntity>&)> collect_entities;
    std::vector<ComponentFieldRecord> fields;
    std::vector<ComponentFieldValue> data_defaults;
    u64 registration_sequence = 0;
    // True only for native components, which register flecs observers in
    // native<T>(). The KinApi mutation paths use this to decide whether a
    // suppression is worth registering: data components have no observer, so a
    // suppression for them would never be consumed and would leak.
    bool observed = false;
};

} // namespace detail

class EcsComponentRegistry {
public:
    using FieldVisitor = std::function<void(std::string_view,
                                            ComponentFieldKind,
                                            const ComponentFieldValue&)>;
    template <typename T>
    class NativeComponentBuilder;
    class DataComponentBuilder;

    explicit EcsComponentRegistry(EcsWorld& world);
    ~EcsComponentRegistry();

    EcsComponentRegistry(const EcsComponentRegistry&) = delete;
    EcsComponentRegistry& operator=(const EcsComponentRegistry&) = delete;

    template <typename T>
    NativeComponentBuilder<T> native(std::string name) {
        if (name.empty()) {
            _last_error = "component name is required";
            return NativeComponentBuilder<T>{*this, nullptr};
        }
        if (find_record(name) != nullptr) {
            _last_error = "duplicate component '" + name + "'";
            return NativeComponentBuilder<T>{*this, nullptr};
        }

        flecs::entity component = _world->component<T>(name);
        component.add(flecs::OnInstantiate, flecs::Inherit);
        auto record = std::make_shared<detail::ComponentRecord>();
        record->descriptor = ComponentDescriptor{
            .id = static_cast<ComponentId>(component.id()),
            .name = std::move(name),
            .kind = ComponentKind::Native,
        };
        record->type = typeid(T);
        record->has = [](EcsEntity entity) {
            return entity.get<T>() != nullptr;
        };
        record->add = [](EcsEntity entity) {
            entity.add<T>();
        };
        record->remove = [](EcsEntity entity) {
            entity.remove<T>();
        };
        record->mutable_ptr = [](EcsEntity entity) -> void* {
            // EcsEntity::get_mut copies an inherited value down before handing
            // back a mutable pointer, so no explicit copy-on-write is needed here.
            return entity.get_mut<T>();
        };
        record->const_ptr = [](EcsEntity entity) -> const void* {
            return entity.get<T>();
        };
        record->modified = [](EcsEntity entity) {
            entity.modified<T>();
        };
        record->collect_entities = [this, component_id = record->descriptor.id](std::vector<EcsEntity>& entities) {
            _world->query<T>().each_entity([&](EcsEntity entity, const T&) {
                if (entity.id() != component_id) {
                    entities.push_back(entity);
                }
            });
        };
        record->registration_sequence = _next_registration_sequence++;
        record->observed = true;

        detail::ComponentRecord* record_ptr = record.get();
        _by_id[record->descriptor.id] = record;
        _by_name[record->descriptor.name] = record;
        _records.push_back(std::move(record));
        _world->events().observe_component<T>(record_ptr->descriptor.id, record_ptr->descriptor.name);
        _last_error.clear();
        return NativeComponentBuilder<T>{*this, record_ptr};
    }

    DataComponentBuilder data(std::string name);

    const ComponentDescriptor* find(ComponentId id) const;
    const ComponentDescriptor* find(std::string_view name) const;
    std::vector<ComponentDescriptor> descriptors() const;

    bool has(EcsEntity entity, ComponentId id) const;
    bool has(EcsEntity entity, std::string_view name) const;
    bool add(EcsEntity entity, ComponentId id);
    bool add(EcsEntity entity, std::string_view name);
    bool remove(EcsEntity entity, ComponentId id);
    bool remove(EcsEntity entity, std::string_view name);

    std::optional<ComponentSnapshot> snapshot(EcsEntity entity, ComponentId id) const;
    std::optional<ComponentSnapshot> snapshot(EcsEntity entity, std::string_view name) const;
    bool visit_fields(EcsEntity entity, ComponentId id, const FieldVisitor& visitor) const;
    bool visit_fields(EcsEntity entity, std::string_view name, const FieldVisitor& visitor) const;
    std::vector<ComponentSnapshot> snapshots(EcsEntity entity) const;
    std::vector<EcsEntity> entities() const;

    bool patch_field(EcsEntity entity, ComponentId id, std::string_view field, const ComponentFieldValue& value);
    bool patch_field(EcsEntity entity, std::string_view component, std::string_view field, const ComponentFieldValue& value);
    bool migrate_data_schema(std::string_view component, const std::vector<ComponentFieldSnapshot>& fields);

    const std::string& last_error() const { return _last_error; }

private:
    detail::ComponentRecord* find_record(ComponentId id);
    const detail::ComponentRecord* find_record(ComponentId id) const;
    detail::ComponentRecord* find_record(std::string_view name);
    const detail::ComponentRecord* find_record(std::string_view name) const;
    bool set_error(std::string error) const;

    EcsWorld* _world = nullptr;
    std::vector<std::shared_ptr<detail::ComponentRecord>> _records;
    std::unordered_map<ComponentId, std::shared_ptr<detail::ComponentRecord>> _by_id;
    std::unordered_map<std::string, std::shared_ptr<detail::ComponentRecord>> _by_name;
    u64 _next_registration_sequence = 0;
    mutable std::string _last_error;
};

void write_component_field_value_json(JsonWriter& json, const ComponentFieldValue& value);
void write_component_snapshot_json(JsonWriter& json, const ComponentSnapshot& snapshot);
void write_entity_snapshot_json(JsonWriter& json, const EntitySnapshot& snapshot);
void write_world_snapshot_json(JsonWriter& json, const WorldSnapshot& snapshot);
void write_query_plan_json(JsonWriter& json, const EcsQueryPlan& plan);

std::string component_field_kind_name(ComponentFieldKind kind);
std::optional<ComponentFieldKind> component_field_kind_from_name(std::string_view name);
std::optional<ComponentFieldValue> component_field_value_from_json(ComponentFieldKind kind,
                                                                   const JsonValue& value,
                                                                   std::string& error);
// Parse an authored component object against the registry's descriptor.  Scene
// and prefab loaders both use this entry point so field validation and JSON
// conversion stay identical.
std::vector<ComponentFieldSnapshot> parse_component_fields(
    std::string_view component_name,
    const JsonValue& json,
    const EcsComponentRegistry& components,
    std::vector<std::string>& diagnostics);
void write_scene_document_json(JsonWriter& json, const SceneDocument& document);
std::string serialize_scene(EcsWorld& world);
SceneDocument scene_document_from_world(EcsWorld& world);
bool instantiate_scene(EcsWorld& world, const SceneDocument& document, std::string& error);

template <typename T>
class EcsComponentRegistry::NativeComponentBuilder {
public:
    NativeComponentBuilder(EcsComponentRegistry& registry, detail::ComponentRecord* record)
        : _registry(registry),
          _record(record) {
    }

    template <typename Field>
    NativeComponentBuilder& field(std::string name, Field T::* member) {
        using FieldType = std::remove_cv_t<Field>;
        static_assert(detail::supported_component_field_v<FieldType>, "unsupported ECS component field type");
        if (!_record) {
            return *this;
        }
        if (name.empty()) {
            _registry._last_error = "component field name is required";
            return *this;
        }
        for (const detail::ComponentFieldRecord& field : _record->fields) {
            if (field.descriptor.name == name) {
                _registry._last_error = "duplicate field '" + name + "' on component '" + _record->descriptor.name + "'";
                return *this;
            }
        }

        detail::ComponentFieldRecord record;
        record.descriptor = ComponentFieldDescriptor{
            .name = std::move(name),
            .kind = detail::ComponentFieldKindOf<FieldType>::value,
        };
        record.read = [member](const void* raw) -> ComponentFieldValue {
            const T& component = *static_cast<const T*>(raw);
            return component.*member;
        };
        record.patch = [member](void* raw, const ComponentFieldValue& value) -> bool {
            T& component = *static_cast<T*>(raw);
            if (const FieldType* typed = std::get_if<FieldType>(&value)) {
                component.*member = *typed;
                return true;
            }
            return false;
        };
        _record->descriptor.fields.push_back(record.descriptor);
        _record->fields.push_back(std::move(record));
        _registry._last_error.clear();
        return *this;
    }

    NativeComponentBuilder& label(std::string value) {
        return update_last_field([&](ComponentFieldDescriptor& field) { field.label = std::move(value); });
    }
    NativeComponentBuilder& category(std::string value) {
        return update_last_field([&](ComponentFieldDescriptor& field) { field.category = std::move(value); });
    }
    NativeComponentBuilder& description(std::string value) {
        return update_last_field([&](ComponentFieldDescriptor& field) { field.description = std::move(value); });
    }
    NativeComponentBuilder& units(std::string value) {
        return update_last_field([&](ComponentFieldDescriptor& field) { field.units = std::move(value); });
    }
    NativeComponentBuilder& readonly(bool value = true) {
        return update_last_field([&](ComponentFieldDescriptor& field) { field.readonly = value; });
    }
    NativeComponentBuilder& hidden(bool value = true) {
        return update_last_field([&](ComponentFieldDescriptor& field) { field.hidden = value; });
    }
    NativeComponentBuilder& range(f64 min, f64 max) {
        return update_last_field([&](ComponentFieldDescriptor& field) {
            field.min_value = min;
            field.max_value = max;
        });
    }

    explicit operator bool() const { return _record != nullptr; }

private:
    template <typename Func>
    NativeComponentBuilder& update_last_field(Func&& func) {
        if (!_record || _record->descriptor.fields.empty() || _record->fields.empty()) {
            _registry._last_error = "component field metadata requires a preceding field";
            return *this;
        }
        func(_record->descriptor.fields.back());
        _record->fields.back().descriptor = _record->descriptor.fields.back();
        _registry._last_error.clear();
        return *this;
    }

    EcsComponentRegistry& _registry;
    detail::ComponentRecord* _record = nullptr;
};

class EcsComponentRegistry::DataComponentBuilder {
public:
    DataComponentBuilder(EcsComponentRegistry& registry, detail::ComponentRecord* record)
        : _registry(registry),
          _record(record) {
    }

    DataComponentBuilder& field(std::string name, ComponentFieldKind kind, ComponentFieldValue default_value);
    DataComponentBuilder& label(std::string value);
    DataComponentBuilder& category(std::string value);
    DataComponentBuilder& description(std::string value);
    DataComponentBuilder& units(std::string value);
    DataComponentBuilder& readonly(bool value = true);
    DataComponentBuilder& hidden(bool value = true);
    DataComponentBuilder& range(f64 min, f64 max);
    DataComponentBuilder& field_bool(std::string name, bool default_value = false) {
        return field(std::move(name), ComponentFieldKind::Bool, default_value);
    }
    DataComponentBuilder& field_i32(std::string name, i32 default_value = 0) {
        return field(std::move(name), ComponentFieldKind::I32, default_value);
    }
    DataComponentBuilder& field_i64(std::string name, i64 default_value = 0) {
        return field(std::move(name), ComponentFieldKind::I64, default_value);
    }
    DataComponentBuilder& field_u32(std::string name, u32 default_value = 0) {
        return field(std::move(name), ComponentFieldKind::U32, default_value);
    }
    DataComponentBuilder& field_u64(std::string name, u64 default_value = 0) {
        return field(std::move(name), ComponentFieldKind::U64, default_value);
    }
    DataComponentBuilder& field_f32(std::string name, f32 default_value = 0.0f) {
        return field(std::move(name), ComponentFieldKind::F32, default_value);
    }
    DataComponentBuilder& field_f64(std::string name, f64 default_value = 0.0) {
        return field(std::move(name), ComponentFieldKind::F64, default_value);
    }
    DataComponentBuilder& field_string(std::string name, std::string default_value = {}) {
        return field(std::move(name), ComponentFieldKind::String, std::move(default_value));
    }
    DataComponentBuilder& field_vec2f(std::string name, Vec2f default_value = {}) {
        return field(std::move(name), ComponentFieldKind::Vec2f, default_value);
    }
    DataComponentBuilder& field_vec2i(std::string name, Vec2i default_value = {}) {
        return field(std::move(name), ComponentFieldKind::Vec2i, default_value);
    }
    DataComponentBuilder& field_color(std::string name, Color default_value = {}) {
        return field(std::move(name), ComponentFieldKind::Color, default_value);
    }
    DataComponentBuilder& field_entity_ref(std::string name, ComponentEntityRef default_value = {}) {
        return field(std::move(name), ComponentFieldKind::EntityRef, std::move(default_value));
    }
    DataComponentBuilder& field_asset_ref(std::string name, ComponentAssetRef default_value = {}) {
        return field(std::move(name), ComponentFieldKind::AssetRef, std::move(default_value));
    }

    explicit operator bool() const { return _record != nullptr; }

private:
    template <typename Func>
    DataComponentBuilder& update_last_field(Func&& func);

    EcsComponentRegistry& _registry;
    detail::ComponentRecord* _record = nullptr;
};

} // namespace kin
