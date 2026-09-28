#pragma once

#include <kin/core/types.hpp>

#include <flecs.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kin {

using EcsId = u64;
using SystemId = std::string;
using ComponentId = EcsId;

class EcsComponentRegistry;
class EcsEntityRegistry;
class EcsEventRegistry;
class EcsRelationRegistry;
class EcsSystemRegistry;
class EcsWorld;
enum class SystemPhase;
enum class SystemExecutionMode;
struct WorldSnapshotOptions;
struct WorldSnapshot;
struct EntitySnapshot;
struct EcsQueryPlanDescriptor;
struct EcsQueryPlan;

using RelationId = EcsId;

class EcsEntity {
public:
    EcsEntity() = default;
    explicit EcsEntity(flecs::entity entity)
        : _entity(entity) {
    }

    EcsId id() const { return static_cast<EcsId>(_entity.id()); }
    bool valid() const { return _entity.is_valid(); }
    bool alive() const { return _entity.is_alive(); }
    explicit operator bool() const { return valid() && alive(); }

    std::string name() const {
        const flecs::string_view value = _entity.name();
        return {value.c_str(), value.length()};
    }

    template <typename T>
    EcsEntity& add() {
        _entity.add<T>();
        return *this;
    }

    template <typename T>
    EcsEntity& remove() {
        _entity.remove<T>();
        return *this;
    }

    template <typename T>
    bool has() const {
        // Mirror flecs' own has<T>(), which follows IsA inheritance via
        // ecs_search_relation. This keeps has() consistent with get<T>(), which
        // also resolves components inherited from prefab bases.
        return _entity.has<T>();
    }

    template <typename T>
    EcsEntity& set(const T& value) {
        _entity.set<T>(value);
        return *this;
    }

    template <typename T>
    EcsEntity& set(T&& value) {
        _entity.set<T>(std::forward<T>(value));
        return *this;
    }

    template <typename T, typename... Args>
    EcsEntity& emplace(Args&&... args) {
        _entity.emplace<T>(std::forward<Args>(args)...);
        return *this;
    }

    template <typename T>
    const T* get() const {
        if (const T* value = _entity.get<T>()) {
            return value;
        }
        for (int32_t i = 0;; ++i) {
            flecs::entity base = _entity.target(flecs::IsA, i);
            if (!base) {
                break;
            }
            if (const T* value = EcsEntity{base}.get<T>()) {
                return value;
            }
        }
        return nullptr;
    }

    // Returns a mutable pointer to this entity's own T. When the entity only
    // inherits T from an IsA base (prefab), the inherited value is first copied
    // down onto the entity (copy-on-write) and a pointer to that private copy is
    // returned, so callers can never mutate shared base state. Returns nullptr
    // only when the entity neither owns nor inherits T.
    template <typename T>
    T* get_mut() {
        if (const T* inherited = get<T>(); inherited && !_entity.owns<T>()) {
            _entity.set<T>(*inherited);
        }
        return _entity.get_mut<T>();
    }

    // Like get_mut, but adds a default-constructed T when the entity neither
    // owns nor inherits one. An inherited value is copied down (copy-on-write)
    // rather than overwritten with a default.
    template <typename T>
    T& ensure() {
        if (const T* inherited = get<T>(); inherited && !_entity.owns<T>()) {
            _entity.set<T>(*inherited);
        }
        return _entity.ensure<T>();
    }

    template <typename T>
    void modified() {
        _entity.modified<T>();
    }

    void destroy() {
        _entity.destruct();
    }

    void enable() {
        _entity.enable();
    }

    void disable() {
        _entity.disable();
    }

    EcsEntity& child_of(const EcsEntity& parent) {
        if (parent.valid()) {
            _entity.child_of(parent.raw());
        }
        return *this;
    }

    EcsEntity parent() const {
        return EcsEntity{_entity.parent()};
    }

    // Escape hatch: direct access to the underlying flecs::entity. Bypasses
    // Kin's event/dirty tracking and authored-id bookkeeping, so prefer the
    // wrapper methods above. Provided for capabilities the wrapper does not yet
    // expose.
    flecs::entity& raw() { return _entity; }
    const flecs::entity& raw() const { return _entity; }

private:
    flecs::entity _entity;
};

// A component handle that stays valid across structural changes. Raw component
// pointers obtained from queries or get_mut() dangle as soon as ANY entity in
// the same table moves archetype (tag add/remove, destroy: rows swap-remove
// and columns may reallocate), which yields heap-timing-dependent reads that
// look fine single-threaded and are brutal to debug. EcsRef re-resolves the
// component's current location on every access instead.
//
// Pattern: when collecting entities from a query and then performing
// structural ops in a second pass, store EcsEntity (re-get_mut per iteration)
// or EcsRef<T> — never T*/T& captured during the query.
template <typename T>
class EcsRef {
public:
    EcsRef() = default;
    explicit EcsRef(EcsEntity entity)
        : _ref(entity.raw()) {
    }

    // Current location of the component; nullptr when the entity no longer
    // has T or was destroyed.
    T* get() { return _ref.try_get(); }
    bool has() { return _ref.try_get() != nullptr; }
    EcsEntity entity() const { return EcsEntity{_ref.entity()}; }

private:
    flecs::ref<T> _ref;
};

using EntityAuthoredId = std::string;

enum class EcsEventKind {
    EntityCreated,
    EntityDestroyed,
    EntityRenamed,
    EntityReparented,
    EntityEnabled,
    EntityDisabled,
    ComponentAdded,
    ComponentRemoved,
    ComponentChanged,
    SystemAdded,
    SystemRemoved,
    SystemEnabled,
    SystemDisabled,
    SystemError,
};

enum class EcsEventSource {
    KinApi,
    FlecsObserver,
};

struct EcsEvent {
    u64 sequence = 0;
    u64 frame = 0;
    EcsEventKind kind = EcsEventKind::EntityCreated;
    EcsEventSource source = EcsEventSource::KinApi;
    EcsId entity = 0;
    ComponentId component_id = 0;
    std::string component_name;
    std::string field_name;
    SystemId system_id;
    std::string message;
};

struct EcsDirtyState {
    bool world_dirty = false;
    bool hierarchy_dirty = false;
    bool components_dirty = false;
    bool systems_dirty = false;
    bool render_dirty = false;
    u64 last_event_sequence = 0;
};

class EcsEventRegistry {
public:
    explicit EcsEventRegistry(EcsWorld& world);
    ~EcsEventRegistry();

    EcsEventRegistry(const EcsEventRegistry&) = delete;
    EcsEventRegistry& operator=(const EcsEventRegistry&) = delete;

    const std::vector<EcsEvent>& snapshot() const { return _events; }
    std::vector<EcsEvent> since(u64 sequence) const;
    const EcsDirtyState& dirty_state() const { return _dirty; }
    void clear();
    void clear_dirty();
    void begin_frame();

    // Retention cap for the event log. The buffer keeps at least this many of
    // the most recent events (it is trimmed in batches, so it may transiently
    // hold up to ~2x before the oldest are dropped). Set to 0 for an unbounded
    // log. Defaults to a bounded value so a long-running loop that never calls
    // clear() does not grow without limit. Lowering the cap trims immediately.
    void set_max_events(std::size_t max_events);
    std::size_t max_events() const { return _max_events; }
    // Oldest sequence still retained, or the next sequence to be assigned when
    // the log is empty. A caller polling with since() can compare this against
    // its cursor to detect that older events were dropped.
    u64 oldest_retained_sequence() const {
        return _events.empty() ? _next_sequence : _events.front().sequence;
    }

    void emit(EcsEvent event);
    void emit_entity(EcsEventKind kind, EcsEntity entity, std::string message = {});
    void emit_component(EcsEventKind kind,
                        EcsEventSource source,
                        EcsEntity entity,
                        ComponentId component_id,
                        std::string component_name,
                        std::string field_name = {},
                        std::string message = {});
    void emit_system(EcsEventKind kind, std::string system_id, std::string message = {});
    void suppress_next_observer(EcsEventKind kind, EcsId entity, ComponentId component_id);

    template <typename T>
    void observe_component(ComponentId component_id, std::string component_name);

private:
    struct ObserverSuppression {
        EcsEventKind kind = EcsEventKind::ComponentChanged;
        EcsId entity = 0;
        ComponentId component_id = 0;
    };

    bool consume_observer_suppression(EcsEventKind kind, EcsId entity, ComponentId component_id);
    void apply_dirty_flags(const EcsEvent& event);
    void trim_events();

    EcsWorld* _world = nullptr;
    std::vector<EcsEvent> _events;
    std::vector<flecs::observer> _component_observers;
    std::vector<ObserverSuppression> _observer_suppressions;
    EcsDirtyState _dirty;
    u64 _next_sequence = 1;
    u64 _frame = 0;
    std::size_t _max_events = 16384;
};

struct EcsEntityHandle {
    EcsId id = 0;
    u64 generation = 0;

    friend constexpr bool operator==(EcsEntityHandle, EcsEntityHandle) = default;
};

struct EntityCreateDescriptor {
    std::string name;
    EntityAuthoredId authored_id;
    EcsEntity parent;
    bool enabled = true;
};

struct EntityCloneOptions {
    bool deep = true;
    EcsEntity parent;
    EntityAuthoredId authored_id;
    bool preserve_child_authored_ids = false;
};

struct EntityCloneResult {
    EcsEntity root;
    std::unordered_map<EcsId, EcsEntity> original_to_clone;
    std::vector<std::string> diagnostics;
};

class EcsEntityRegistry {
public:
    explicit EcsEntityRegistry(EcsWorld& world);
    ~EcsEntityRegistry();

    EcsEntityRegistry(const EcsEntityRegistry&) = delete;
    EcsEntityRegistry& operator=(const EcsEntityRegistry&) = delete;

    EcsEntity create(const EntityCreateDescriptor& descriptor = {});
    EcsEntity register_existing(EcsEntity entity, EntityAuthoredId authored_id = {});
    bool destroy(EcsEntityHandle handle);
    bool destroy(EcsEntity entity);
    bool destroy(EcsId id);
    bool enable(EcsEntityHandle handle, bool enabled = true);
    bool enable(EcsEntity entity, bool enabled = true);
    bool disable(EcsEntityHandle handle);
    bool disable(EcsEntity entity);
    bool rename(EcsEntityHandle handle, std::string_view name);
    bool rename(EcsEntity entity, std::string_view name);
    bool set_authored_id(EcsEntityHandle handle, EntityAuthoredId authored_id);
    bool set_authored_id(EcsEntity entity, EntityAuthoredId authored_id);
    bool reparent(EcsEntityHandle child, EcsEntityHandle parent);
    bool reparent(EcsEntity child, EcsEntity parent);
    bool detach_parent(EcsEntityHandle child);
    bool detach_parent(EcsEntity child);

    EcsEntityHandle handle(EcsEntity entity) const;
    EcsEntity resolve(EcsEntityHandle handle) const;
    bool valid(EcsEntityHandle handle) const;
    EcsEntity find_by_id(EcsId id) const;
    EcsEntity find_by_authored_id(std::string_view authored_id) const;
    EcsEntity find_by_name(std::string_view name) const;
    std::vector<EcsEntity> all() const;
    std::vector<EcsEntity> hierarchy_roots() const;

    EntityCloneResult clone(EcsEntity source, const EntityCloneOptions& options = {});
    std::optional<EntityAuthoredId> authored_id(EcsEntity entity) const;
    u64 generation(EcsEntity entity) const;
    u64 creation_sequence(EcsEntity entity) const;
    bool registered(EcsEntity entity) const;

    const std::string& last_error() const { return _last_error; }

private:
    struct Record {
        EcsId id = 0;
        EntityAuthoredId authored_id;
        u64 generation = 1;
        u64 creation_sequence = 0;
        // The name this entity is currently filed under in _by_name. Cached so
        // a rename or destroy can evict the old bucket entry incrementally
        // without re-walking every record.
        std::string indexed_name;
        bool name_indexed = false;
    };

    EcsEntity entity_from_id(EcsId id) const;
    Record* record(EcsId id);
    const Record* record(EcsId id) const;
    bool set_error(std::string error) const;
    bool would_create_parent_cycle(EcsEntity child, EcsEntity parent) const;
    void forget(EcsId id);
    void index_name(EcsId id);
    void unindex_name(EcsId id);

    EcsWorld* _world = nullptr;
    std::unordered_map<EcsId, Record> _records;
    std::unordered_map<EntityAuthoredId, EcsId> _by_authored_id;
    std::unordered_map<std::string, std::vector<EcsId>> _by_name;
    u64 _next_creation_sequence = 0;
    mutable std::string _last_error;
};

struct RelationDescriptor {
    RelationId id = 0;
    std::string name;
};

struct RelationPairSnapshot {
    RelationDescriptor relation;
    EcsId target = 0;
    std::string target_name;
    EntityAuthoredId target_authored_id;
};

class EcsRelationRegistry {
public:
    explicit EcsRelationRegistry(EcsWorld& world);
    ~EcsRelationRegistry();

    EcsRelationRegistry(const EcsRelationRegistry&) = delete;
    EcsRelationRegistry& operator=(const EcsRelationRegistry&) = delete;

    RelationId relation(std::string name);
    const RelationDescriptor* find(RelationId id) const;
    const RelationDescriptor* find(std::string_view name) const;
    std::vector<RelationDescriptor> descriptors() const;

    bool add(EcsEntity source, std::string_view relation, EcsEntity target);
    bool remove(EcsEntity source, std::string_view relation, EcsEntity target);
    bool has(EcsEntity source, std::string_view relation, EcsEntity target) const;
    std::vector<RelationPairSnapshot> snapshots(EcsEntity source) const;

    const std::string& last_error() const { return _last_error; }

private:
    bool set_error(std::string error) const;

    EcsWorld* _world = nullptr;
    std::vector<RelationDescriptor> _records;
    std::unordered_map<RelationId, std::size_t> _by_id;
    std::unordered_map<std::string, std::size_t> _by_name;
    mutable std::string _last_error;
};

template <typename... Components>
class EcsQuery {
public:
    explicit EcsQuery(flecs::query<Components...> query)
        : _query(query) {
    }

    void each(auto&& func) {
        _query.each(std::forward<decltype(func)>(func));
    }

    void each_entity(auto&& func) {
        _query.each([&](flecs::entity entity, Components&... components) {
            func(EcsEntity{entity}, components...);
        });
    }

    void run(auto&& func) {
        _query.run(std::forward<decltype(func)>(func));
    }

    i32 count() {
        i32 result = 0;
        run([&](flecs::iter& it) {
            while (it.next()) {
                result += static_cast<i32>(it.count());
            }
        });
        return result;
    }

    // Escape hatch: direct access to the underlying flecs::query. Prefer
    // each/each_entity/run above.
    flecs::query<Components...>& raw() { return _query; }
    const flecs::query<Components...>& raw() const { return _query; }

private:
    flecs::query<Components...> _query;
};

class EcsWorld {
public:
    EcsWorld();
    ~EcsWorld();

    EcsWorld(const EcsWorld&) = delete;
    EcsWorld& operator=(const EcsWorld&) = delete;
    EcsWorld(EcsWorld&&) noexcept;
    EcsWorld& operator=(EcsWorld&&) noexcept;

    template <typename T>
    flecs::entity component(std::string_view name = {}) {
        if (name.empty()) {
            return _world.component<T>();
        }
        return _world.component<T>(std::string{name}.c_str());
    }

    EcsEntity entity(std::string_view name = {});

    // Look up an entity by flecs name/path (e.g. "scene.player"). Returns an
    // invalid EcsEntity if none matches. Thin wrapper over the flecs lookup so
    // callers don't need raw() for name resolution.
    EcsEntity lookup(std::string_view path) {
        return EcsEntity{_world.lookup(std::string{path}.c_str())};
    }

    // Rebuild an EcsEntity handle from a raw entity id (e.g. a physics user_id
    // stored as u64), without reaching for raw().
    EcsEntity entity_by_id(EcsId id) {
        return EcsEntity{_world.entity(id)};
    }

    void each(auto&& func) {
        _world.each(std::forward<decltype(func)>(func));
    }

    template <typename... Components>
    EcsQuery<Components...> query() {
        return EcsQuery<Components...>{_world.query<Components...>()};
    }

    template <typename... Components>
    i32 count() {
        return query<Components...>().count();
    }

    bool progress(f32 dt = 0.0f) {
        return _world.progress(dt);
    }

    EcsComponentRegistry& components();
    const EcsComponentRegistry& components() const;
    EcsEventRegistry& events();
    const EcsEventRegistry& events() const;
    EcsRelationRegistry& relations();
    const EcsRelationRegistry& relations() const;
    EcsEntityRegistry& entities();
    const EcsEntityRegistry& entities() const;
    EcsSystemRegistry& systems();
    const EcsSystemRegistry& systems() const;
    bool run_system(std::string_view id, f32 dt = 0.0f);
    bool run_phase(SystemPhase phase, f32 dt = 0.0f);
    bool run_phase(SystemPhase phase, f32 dt, SystemExecutionMode mode);
    bool run_frame(f32 dt = 0.0f);
    bool run_frame(f32 dt, SystemExecutionMode mode);
    WorldSnapshot snapshot() const;
    WorldSnapshot snapshot(const WorldSnapshotOptions& options) const;
    EntitySnapshot snapshot(EcsEntity entity) const;
    EcsQueryPlan build_query_plan(const EcsQueryPlanDescriptor& descriptor) const;
    std::vector<EcsEntity> query_entities(const EcsQueryPlan& plan) const;

    template <typename Func>
    void each(const EcsQueryPlan& plan, Func&& func) const;

    // Escape hatch: direct access to the underlying flecs::world. Bypasses
    // Kin's registries (events, entity bookkeeping, scheduling), so prefer the
    // wrapper API. Provided for capabilities not yet surfaced by EcsWorld.
    flecs::world& raw() { return _world; }
    const flecs::world& raw() const { return _world; }

private:
    flecs::world _world;
    // Owns dynamic query matchers before `_world` is destroyed. Query plans
    // keep weak references so a plan can safely outlive its source world.
    mutable std::vector<std::shared_ptr<flecs::query<>>> _compiled_queries;
    std::unique_ptr<EcsEventRegistry> _events;
    std::unique_ptr<EcsRelationRegistry> _relations;
    std::unique_ptr<EcsComponentRegistry> _components;
    std::unique_ptr<EcsEntityRegistry> _entities;
    std::unique_ptr<EcsSystemRegistry> _systems;
};

template <typename T>
void EcsEventRegistry::observe_component(ComponentId component_id, std::string component_name) {
    const std::string base_name = "kin::ecs::events::" + component_name;
    _component_observers.push_back(_world->raw()
                                       .observer<T>((base_name + "::on_add").c_str())
                                       .event(flecs::OnAdd)
                                       .each([this, component_id, component_name](flecs::entity entity, T&) {
                                           emit_component(EcsEventKind::ComponentAdded,
                                                          EcsEventSource::FlecsObserver,
                                                          EcsEntity{entity},
                                                          component_id,
                                                          component_name);
                                       }));
    _component_observers.push_back(_world->raw()
                                       .observer<T>((base_name + "::on_remove").c_str())
                                       .event(flecs::OnRemove)
                                       .each([this, component_id, component_name](flecs::entity entity, T&) {
                                           emit_component(EcsEventKind::ComponentRemoved,
                                                          EcsEventSource::FlecsObserver,
                                                          EcsEntity{entity},
                                                          component_id,
                                                          component_name);
                                       }));
    _component_observers.push_back(_world->raw()
                                       .observer<T>((base_name + "::on_set").c_str())
                                       .event(flecs::OnSet)
                                       .each([this, component_id, component_name](flecs::entity entity, T&) {
                                           emit_component(EcsEventKind::ComponentChanged,
                                                          EcsEventSource::FlecsObserver,
                                                          EcsEntity{entity},
                                                          component_id,
                                                          component_name);
                                       }));
}

} // namespace kin

#include <kin/ecs/component.hpp>

namespace kin {

template <typename Func>
void EcsWorld::each(const EcsQueryPlan& plan, Func&& func) const {
    for (EcsEntity entity : query_entities(plan)) {
        func(entity);
    }
}

} // namespace kin
