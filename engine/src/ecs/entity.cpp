#include <kin/ecs/component.hpp>

#include <algorithm>
#include <limits>
#include <string>
#include <unordered_set>

namespace kin {
namespace {

std::string entity_name(flecs::entity entity) {
    const flecs::string_view value = entity.name();
    return {value.c_str(), value.length()};
}

void collect_descendants(EcsEntity entity, std::vector<EcsEntity>& out, std::unordered_set<EcsId>& visited) {
    std::vector<flecs::entity> children;
    entity.raw().children([&](flecs::entity child) {
        if (visited.insert(static_cast<EcsId>(child.id())).second) {
            children.push_back(child);
        }
    });
    std::ranges::sort(children, [](flecs::entity a, flecs::entity b) {
        const std::string a_name = entity_name(a);
        const std::string b_name = entity_name(b);
        if (a_name != b_name) {
            return a_name < b_name;
        }
        return a.id() < b.id();
    });
    for (flecs::entity child : children) {
        EcsEntity wrapped{child};
        out.push_back(wrapped);
        collect_descendants(wrapped, out, visited);
    }
}

} // namespace

EcsEntityRegistry::EcsEntityRegistry(EcsWorld& world)
    : _world(&world) {
}

EcsEntityRegistry::~EcsEntityRegistry() = default;

EcsEntity EcsEntityRegistry::create(const EntityCreateDescriptor& descriptor) {
    if (!descriptor.authored_id.empty() && _by_authored_id.contains(descriptor.authored_id)) {
        set_error("duplicate authored entity id '" + descriptor.authored_id + "'");
        return {};
    }
    EcsEntity entity{_world->raw().entity()};
    if (!descriptor.name.empty()) {
        const std::string flecs_name = find_by_name(descriptor.name)
            ? descriptor.name + "." + std::to_string(entity.id())
            : descriptor.name;
        ecs_set_name(_world->raw().c_ptr(), static_cast<ecs_entity_t>(entity.id()), flecs_name.c_str());
    }
    register_existing(entity, descriptor.authored_id);
    if (descriptor.parent) {
        if (!reparent(entity, descriptor.parent)) {
            entity.destroy();
            return {};
        }
    }
    if (!descriptor.enabled) {
        entity.disable();
    }
    _world->events().emit_entity(EcsEventKind::EntityCreated, entity);
    if (descriptor.parent) {
        _world->events().emit_entity(EcsEventKind::EntityReparented, entity);
    }
    if (!descriptor.enabled) {
        _world->events().emit_entity(EcsEventKind::EntityDisabled, entity);
    }
    _last_error.clear();
    return entity;
}

EcsEntity EcsEntityRegistry::register_existing(EcsEntity entity, EntityAuthoredId authored_id) {
    if (!entity || entity.id() == 0) {
        set_error("cannot register invalid entity");
        return {};
    }
    if (!authored_id.empty()) {
        const auto duplicate = _by_authored_id.find(authored_id);
        if (duplicate != _by_authored_id.end() && duplicate->second != entity.id()) {
            set_error("duplicate authored entity id '" + authored_id + "'");
            return {};
        }
    }

    Record& item = _records[entity.id()];
    if (item.id == 0) {
        item.id = entity.id();
        item.generation = 1;
        item.creation_sequence = _next_creation_sequence++;
    }
    if (!item.authored_id.empty()) {
        _by_authored_id.erase(item.authored_id);
    }
    item.authored_id = std::move(authored_id);
    if (!item.authored_id.empty()) {
        _by_authored_id[item.authored_id] = item.id;
    }
    index_name(entity.id());
    _last_error.clear();
    return entity;
}

bool EcsEntityRegistry::destroy(EcsEntityHandle handle) {
    return destroy(resolve(handle));
}

bool EcsEntityRegistry::destroy(EcsEntity entity) {
    if (!entity) {
        return set_error("cannot destroy invalid entity");
    }
    std::vector<EcsEntity> to_forget{entity};
    std::unordered_set<EcsId> visited{entity.id()};
    collect_descendants(entity, to_forget, visited);
    entity.destroy();
    for (EcsEntity item : to_forget) {
        _world->events().emit({
            .kind = EcsEventKind::EntityDestroyed,
            .source = EcsEventSource::KinApi,
            .entity = item.id(),
        });
        forget(item.id());
    }
    _last_error.clear();
    return true;
}

bool EcsEntityRegistry::destroy(EcsId id) {
    return destroy(entity_from_id(id));
}

bool EcsEntityRegistry::enable(EcsEntityHandle handle, bool enabled) {
    return enable(resolve(handle), enabled);
}

bool EcsEntityRegistry::enable(EcsEntity entity, bool enabled) {
    if (!entity) {
        return set_error("cannot change enabled state for invalid entity");
    }
    if (enabled) {
        entity.enable();
        _world->events().emit_entity(EcsEventKind::EntityEnabled, entity);
    } else {
        entity.disable();
        _world->events().emit_entity(EcsEventKind::EntityDisabled, entity);
    }
    _last_error.clear();
    return true;
}

bool EcsEntityRegistry::disable(EcsEntityHandle handle) {
    return enable(handle, false);
}

bool EcsEntityRegistry::disable(EcsEntity entity) {
    return enable(entity, false);
}

bool EcsEntityRegistry::rename(EcsEntityHandle handle, std::string_view name) {
    return rename(resolve(handle), name);
}

bool EcsEntityRegistry::rename(EcsEntity entity, std::string_view name) {
    if (!entity) {
        return set_error("cannot rename invalid entity");
    }
    const std::string owned{name};
    ecs_set_name(_world->raw().c_ptr(), static_cast<ecs_entity_t>(entity.id()), owned.empty() ? nullptr : owned.c_str());
    register_existing(entity, authored_id(entity).value_or(EntityAuthoredId{}));
    _world->events().emit_entity(EcsEventKind::EntityRenamed, entity);
    _last_error.clear();
    return true;
}

bool EcsEntityRegistry::set_authored_id(EcsEntityHandle handle, EntityAuthoredId authored_id) {
    return set_authored_id(resolve(handle), std::move(authored_id));
}

bool EcsEntityRegistry::set_authored_id(EcsEntity entity, EntityAuthoredId authored_id) {
    if (!entity) {
        return set_error("cannot set authored id on invalid entity");
    }
    return static_cast<bool>(register_existing(entity, std::move(authored_id)));
}

bool EcsEntityRegistry::reparent(EcsEntityHandle child, EcsEntityHandle parent) {
    return reparent(resolve(child), resolve(parent));
}

bool EcsEntityRegistry::reparent(EcsEntity child, EcsEntity parent) {
    if (!child || !parent) {
        return set_error("cannot reparent invalid entity");
    }
    if (child.id() == parent.id() || would_create_parent_cycle(child, parent)) {
        return set_error("reparent would create an entity hierarchy cycle");
    }
    child.child_of(parent);
    _world->events().emit_entity(EcsEventKind::EntityReparented, child);
    _last_error.clear();
    return true;
}

bool EcsEntityRegistry::detach_parent(EcsEntityHandle child) {
    return detach_parent(resolve(child));
}

bool EcsEntityRegistry::detach_parent(EcsEntity child) {
    if (!child) {
        return set_error("cannot detach invalid entity");
    }
    EcsEntity parent = child.parent();
    if (parent) {
        ecs_remove_pair(_world->raw().c_ptr(),
                        static_cast<ecs_entity_t>(child.id()),
                        EcsChildOf,
                        static_cast<ecs_entity_t>(parent.id()));
        _world->events().emit_entity(EcsEventKind::EntityReparented, child);
    }
    _last_error.clear();
    return true;
}

EcsEntityHandle EcsEntityRegistry::handle(EcsEntity entity) const {
    const Record* item = record(entity.id());
    if (!entity || !item) {
        return {};
    }
    return {.id = entity.id(), .generation = item->generation};
}

EcsEntity EcsEntityRegistry::resolve(EcsEntityHandle handle) const {
    const Record* item = record(handle.id);
    if (!item || item->generation != handle.generation) {
        return {};
    }
    EcsEntity entity = entity_from_id(handle.id);
    return entity ? entity : EcsEntity{};
}

bool EcsEntityRegistry::valid(EcsEntityHandle handle) const {
    return static_cast<bool>(resolve(handle));
}

EcsEntity EcsEntityRegistry::find_by_id(EcsId id) const {
    return entity_from_id(id);
}

EcsEntity EcsEntityRegistry::find_by_authored_id(std::string_view authored_id) const {
    const auto found = _by_authored_id.find(std::string{authored_id});
    if (found == _by_authored_id.end()) {
        return {};
    }
    return entity_from_id(found->second);
}

EcsEntity EcsEntityRegistry::find_by_name(std::string_view name) const {
    const auto found = _by_name.find(std::string{name});
    if (found == _by_name.end()) {
        return {};
    }
    for (const EcsId id : found->second) {
        EcsEntity entity = entity_from_id(id);
        if (entity) {
            return entity;
        }
    }
    return {};
}

std::vector<EcsEntity> EcsEntityRegistry::all() const {
    std::vector<EcsEntity> result;
    result.reserve(_records.size());
    for (const auto& [id, item] : _records) {
        (void)item;
        EcsEntity entity = entity_from_id(id);
        if (entity) {
            result.push_back(entity);
        }
    }
    std::ranges::sort(result, [&](EcsEntity a, EcsEntity b) {
        const u64 a_sequence = creation_sequence(a);
        const u64 b_sequence = creation_sequence(b);
        if (a_sequence != b_sequence) {
            return a_sequence < b_sequence;
        }
        if (a.name() != b.name()) {
            return a.name() < b.name();
        }
        return a.id() < b.id();
    });
    return result;
}

std::vector<EcsEntity> EcsEntityRegistry::hierarchy_roots() const {
    std::vector<EcsEntity> result;
    for (EcsEntity entity : all()) {
        if (!entity.parent()) {
            result.push_back(entity);
        }
    }
    return result;
}

EntityCloneResult EcsEntityRegistry::clone(EcsEntity source, const EntityCloneOptions& options) {
    EntityCloneResult result;
    if (!source) {
        result.diagnostics.push_back("cannot clone invalid entity");
        return result;
    }

    std::vector<EcsEntity> sources{source};
    if (options.deep) {
        std::unordered_set<EcsId> visited{source.id()};
        collect_descendants(source, sources, visited);
    }

    std::unordered_set<std::string> authored_ids;
    if (!options.authored_id.empty()) {
        authored_ids.insert(options.authored_id);
    }
    for (std::size_t i = 0; i < sources.size(); ++i) {
        if (i == 0 || !options.preserve_child_authored_ids) {
            continue;
        }
        if (std::optional<EntityAuthoredId> existing = authored_id(sources[i]); existing && !existing->empty()) {
            if (_by_authored_id.contains(*existing) || !authored_ids.insert(*existing).second) {
                result.diagnostics.push_back("duplicate authored entity id '" + *existing + "'");
                return result;
            }
        }
    }
    if (!options.authored_id.empty() && _by_authored_id.contains(options.authored_id)) {
        result.diagnostics.push_back("duplicate authored entity id '" + options.authored_id + "'");
        return result;
    }

    for (std::size_t i = 0; i < sources.size(); ++i) {
        EcsEntity original = sources[i];
        EntityAuthoredId clone_authored_id;
        if (i == 0) {
            clone_authored_id = options.authored_id;
        } else if (options.preserve_child_authored_ids) {
            clone_authored_id = authored_id(original).value_or(EntityAuthoredId{});
        }

        EcsEntity parent;
        if (i == 0) {
            parent = options.parent;
        } else {
            const EcsId parent_id = original.parent().id();
            parent = result.original_to_clone.at(parent_id);
        }

        EcsEntity copy = create({
            .name = original.name(),
            .authored_id = std::move(clone_authored_id),
            .parent = parent,
            .enabled = original.raw().enabled(),
        });
        if (!copy) {
            result.diagnostics.push_back(_last_error);
            return result;
        }
        for (const ComponentSnapshot& component : _world->components().snapshots(original)) {
            if (!_world->components().add(copy, component.descriptor.id)) {
                result.diagnostics.push_back(_world->components().last_error());
                return result;
            }
            for (const ComponentFieldSnapshot& field : component.fields) {
                if (!_world->components().patch_field(copy, component.descriptor.id, field.name, field.value)) {
                    result.diagnostics.push_back(_world->components().last_error());
                    return result;
                }
            }
        }
        result.original_to_clone[original.id()] = copy;
        if (i == 0) {
            result.root = copy;
        }
    }
    return result;
}

std::optional<EntityAuthoredId> EcsEntityRegistry::authored_id(EcsEntity entity) const {
    const Record* item = record(entity.id());
    if (!item) {
        return std::nullopt;
    }
    return item->authored_id;
}

u64 EcsEntityRegistry::generation(EcsEntity entity) const {
    const Record* item = record(entity.id());
    return item ? item->generation : 0;
}

u64 EcsEntityRegistry::creation_sequence(EcsEntity entity) const {
    const Record* item = record(entity.id());
    return item ? item->creation_sequence : std::numeric_limits<u64>::max();
}

bool EcsEntityRegistry::registered(EcsEntity entity) const {
    return record(entity.id()) != nullptr;
}

EcsEntity EcsEntityRegistry::entity_from_id(EcsId id) const {
    if (id == 0) {
        return {};
    }
    EcsEntity entity{flecs::entity{const_cast<flecs::world&>(_world->raw()).c_ptr(), static_cast<flecs::entity_t>(id)}};
    return entity ? entity : EcsEntity{};
}

EcsEntityRegistry::Record* EcsEntityRegistry::record(EcsId id) {
    const auto found = _records.find(id);
    return found == _records.end() ? nullptr : &found->second;
}

const EcsEntityRegistry::Record* EcsEntityRegistry::record(EcsId id) const {
    const auto found = _records.find(id);
    return found == _records.end() ? nullptr : &found->second;
}

bool EcsEntityRegistry::set_error(std::string error) const {
    _last_error = std::move(error);
    return false;
}

bool EcsEntityRegistry::would_create_parent_cycle(EcsEntity child, EcsEntity parent) const {
    std::unordered_set<EcsId> visited;
    for (EcsEntity current = parent; current; current = current.parent()) {
        if (!visited.insert(current.id()).second) {
            return false;
        }
        if (current.id() == child.id()) {
            return true;
        }
    }
    return false;
}

void EcsEntityRegistry::forget(EcsId id) {
    auto found = _records.find(id);
    if (found == _records.end()) {
        return;
    }
    unindex_name(id);
    if (!found->second.authored_id.empty()) {
        _by_authored_id.erase(found->second.authored_id);
    }
    // The record is about to be erased, so bumping its generation here had no
    // effect: flecs hands out a fresh 64-bit id (new generation bits) for any
    // recycled entity, so a stale handle is already rejected by resolve() when
    // its id no longer maps to a record.
    _records.erase(found);
}

// Insert/move an entity into the _by_name bucket for its current flecs name,
// keeping the bucket ordered by (creation_sequence, id) so find_by_name returns
// the oldest match first. Idempotent when the name has not changed.
void EcsEntityRegistry::index_name(EcsId id) {
    Record* item = record(id);
    if (!item) {
        return;
    }
    EcsEntity entity = entity_from_id(id);
    const std::string current = entity ? entity.name() : std::string{};
    if (item->name_indexed && item->indexed_name == current) {
        return;
    }
    if (item->name_indexed) {
        unindex_name(id);
    }
    std::vector<EcsId>& bucket = _by_name[current];
    const auto position = std::ranges::lower_bound(bucket, id, [&](EcsId a, EcsId b) {
        const Record* a_record = record(a);
        const Record* b_record = record(b);
        const u64 a_sequence = a_record ? a_record->creation_sequence : std::numeric_limits<u64>::max();
        const u64 b_sequence = b_record ? b_record->creation_sequence : std::numeric_limits<u64>::max();
        if (a_sequence != b_sequence) {
            return a_sequence < b_sequence;
        }
        return a < b;
    });
    bucket.insert(position, id);
    item->indexed_name = current;
    item->name_indexed = true;
}

void EcsEntityRegistry::unindex_name(EcsId id) {
    Record* item = record(id);
    if (!item || !item->name_indexed) {
        return;
    }
    const auto bucket_it = _by_name.find(item->indexed_name);
    if (bucket_it != _by_name.end()) {
        std::erase(bucket_it->second, id);
        if (bucket_it->second.empty()) {
            _by_name.erase(bucket_it);
        }
    }
    item->name_indexed = false;
    item->indexed_name.clear();
}

EcsEntity EcsWorld::entity(std::string_view name) {
    return _entities->create({.name = std::string{name}});
}

EcsEntityRegistry& EcsWorld::entities() {
    return *_entities;
}

const EcsEntityRegistry& EcsWorld::entities() const {
    return *_entities;
}

} // namespace kin
