#include <algorithm>
#include <kin/scene/live_scene2d.hpp>
#include <kin/scene/scene_asset.hpp>
#include <map>
#include <set>
#include <stdexcept>
namespace kin {
namespace {
template <class T> const T* named(const std::vector<T>& values, std::string_view name) {
    const auto it = std::ranges::find(values, name, &T::name);
    return it == values.end() ? nullptr : &*it;
}
bool same(const ComponentFieldValue& a, const ComponentFieldValue& b) {
    if (const auto* ref = std::get_if<ComponentEntityRef>(&a)) {
        const auto* other = std::get_if<ComponentEntityRef>(&b);
        return other && ref->authored_id == other->authored_id;
    }
    return a == b;
}
struct Patch {
    EcsWorld* world;
    EcsWorld backup, source;
    SceneDocument before, after;
    std::map<std::string, EcsEntity, std::less<>> live, saved;
    std::map<std::string, EntityDescriptor, std::less<>> descriptors;
    std::map<std::string, std::vector<EcsId>, std::less<>> bases;
    std::map<std::string, std::set<std::string>, std::less<>> owned;
    std::vector<EcsEntity> created;
    std::set<std::string> removing;
    const SceneEntityDocument* original(std::string_view id) const {
        const auto it = std::ranges::find(before.entities, id, &SceneEntityDocument::id);
        return it == before.entities.end() ? nullptr : &*it;
    }
    bool activate(std::string& error) {
        try {
            auto& registry = world->components();
            for (const auto& row : after.entities)
                if (!live.contains(row.id)) {
                    auto entity = world->entities().create({.name = row.name, .authored_id = row.id});
                    if (!entity)
                        throw std::runtime_error("cannot create authored entity " + row.id);
                    live[row.id] = entity;
                    created.push_back(entity);
                }
            for (const auto& row : after.entities) {
                const auto* old = original(row.id);
                auto entity = live.at(row.id);
                if (!old || old->name != row.name)
                    if (!world->entities().rename(entity, row.name))
                        throw std::runtime_error("cannot rename " + row.id);
                if (!old || old->parent != row.parent) {
                    const bool ok = row.parent.empty() ? world->entities().detach_parent(entity)
                                                       : world->entities().reparent(entity, live.at(row.parent));
                    if (!ok)
                        throw std::runtime_error("cannot reparent " + row.id);
                }
                for (const auto& component : row.components) {
                    const auto* prior = old ? named(old->components, component.name) : nullptr;
                    if (!prior && !registry.add(entity, component.name))
                        throw std::runtime_error(registry.last_error());
                    for (const auto& field : component.fields) {
                        const auto* previous = prior ? named(prior->fields, field.name) : nullptr;
                        if (previous && same(previous->value, field.value))
                            continue;
                        const auto* descriptor = registry.find(component.name);
                        const auto* rule = descriptor ? named(descriptor->fields, field.name) : nullptr;
                        if (rule && rule->readonly)
                            continue;
                        auto value = field.value;
                        if (auto* ref = std::get_if<ComponentEntityRef>(&value))
                            ref->id = ref->authored_id.empty() ? 0 : live.at(ref->authored_id).id();
                        if (!registry.has(entity, component.name) && !registry.add(entity, component.name))
                            throw std::runtime_error(registry.last_error());
                        if (!registry.patch_field(entity, component.name, field.name, value))
                            throw std::runtime_error(registry.last_error());
                    }
                }
                if (old)
                    for (const auto& component : old->components)
                        if (!named(row.components, component.name)) {
                            const bool removed = entity.has<PrefabEntityComponent>()
                                                     ? remove_prefab_component(*world, entity, component.name)
                                                     : registry.remove(entity, component.name);
                            if (!removed)
                                throw std::runtime_error("cannot remove component " + component.name);
                        }
            }
            return true;
        } catch (const std::exception& e) {
            error = e.what();
            return false;
        }
    }
    void rollback() {
        auto& registry = world->components();
        for (const auto& [id, copy] : saved) {
            auto entity = live.at(id);
            entity.raw().remove(flecs::IsA, flecs::Wildcard);
            for (auto base : bases.at(id))
                entity.raw().add(flecs::IsA, base);
            for (const auto& component : registry.snapshots(entity))
                if (!backup.components().has(copy, component.descriptor.name))
                    registry.remove(entity, component.descriptor.name);
            for (const auto& component : backup.components().snapshots(copy))
                if (!backup.components().copy_component(copy, component.descriptor.name, registry, entity))
                    std::terminate();
            for (const auto& component : backup.components().snapshots(copy))
                if (!owned.at(id).contains(component.descriptor.name))
                    registry.remove(entity, component.descriptor.name);
            const auto& original = descriptors.at(id);
            world->entities().rename(entity, original.name);
            auto parent = world->entities().find_by_id(original.parent);
            if (parent)
                world->entities().reparent(entity, parent);
            else
                world->entities().detach_parent(entity);
        }
        for (auto entity : created)
            if (entity)
                world->entities().destroy(entity);
        created.clear();
    }
    void finalize() {
        for (const auto& id : removing) {
            const auto entity = live.at(id);
            if (!entity)
                continue;
            const auto children = world->snapshot(entity).descriptor.children;
            for (auto child : children) {
                auto handle = world->entities().find_by_id(child);
                const auto authored = world->entities().authored_id(handle);
                if (!authored || !removing.contains(*authored))
                    world->entities().detach_parent(handle);
            }
            world->entities().destroy(entity);
        }
    }
};
} // namespace
SceneDocument authored_scene_document(EcsWorld& world, const SceneInstance& instance) {
    auto document = scene_document_from_world(world);
    std::erase_if(document.entities, [&](const auto& row) { return !instance.by_authored_id.contains(row.id); });
    for (auto& row : document.entities)
        for (auto& component : row.components) {
            const auto* descriptor = world.components().find(component.name);
            if (descriptor)
                std::erase_if(component.fields, [&](const auto& field) {
                    const auto* rule = named(descriptor->fields, field.name);
                    return rule && rule->readonly;
                });
        }
    return document;
}
std::unique_ptr<DevelopmentChange> prepare_scene_patch2d(EcsWorld& world, const SceneDocument& before,
                                                         const SceneDocument& after, std::string& error) {
    try {
        if (world.components().has_field_patch_handler())
            throw std::runtime_error("this world requires its owner's transactional field edit policy");
        auto patch = std::make_shared<Patch>();
        patch->world = &world;
        patch->before = before;
        patch->after = after;
        world.components().copy_schema_to(patch->backup.components());
        world.components().copy_schema_to(patch->source.components());
        if (!instantiate_scene(patch->source, after, error))
            return {};
        for (const auto& row : before.entities) {
            auto entity = world.entities().find_by_authored_id(row.id);
            if (!entity)
                throw std::runtime_error("authored entity no longer exists: " + row.id);
            patch->live[row.id] = entity;
            patch->descriptors[row.id] = world.snapshot(entity).descriptor;
            auto& bases = patch->bases[row.id];
            for (i32 i = 0;; ++i) {
                const auto base = ecs_get_target(world.raw().c_ptr(), entity.id(), flecs::IsA, i);
                if (!base)
                    break;
                bases.push_back(base);
            }
            auto& owned = patch->owned[row.id];
            for (const auto& component : world.components().snapshots(entity))
                if (entity.raw().owns(component.descriptor.id))
                    owned.insert(component.descriptor.name);
            auto saved = patch->backup.entities().create({.authored_id = row.id});
            patch->saved[row.id] = saved;
            for (const auto& component : world.components().snapshots(entity))
                if (!world.components().copy_component(entity, component.descriptor.name, patch->backup.components(),
                                                       saved))
                    throw std::runtime_error(patch->backup.components().last_error());
            if (std::ranges::find(after.entities, row.id, &SceneEntityDocument::id) == after.entities.end())
                patch->removing.insert(row.id);
        }
        for (const auto& row : after.entities) {
            const auto* old = patch->original(row.id);
            if (!old && world.entities().find_by_authored_id(row.id))
                throw std::runtime_error("new authored ID collides with runtime entity: " + row.id);
            auto source = patch->source.entities().find_by_authored_id(row.id);
            for (const auto& component : row.components)
                for (const auto& field : component.fields) {
                    const auto* prior = old ? named(old->components, component.name) : nullptr;
                    const auto* previous = prior ? named(prior->fields, field.name) : nullptr;
                    if (previous && same(previous->value, field.value))
                        continue;
                    const auto* descriptor = world.components().find(component.name);
                    const auto* rule = descriptor ? named(descriptor->fields, field.name) : nullptr;
                    if (!rule || (rule->readonly && previous))
                        throw std::runtime_error("field is missing or readonly: " + component.name + "." + field.name);
                    if (rule->readonly)
                        continue;
                    std::visit(
                        [&](const auto& value) {
                            using T = std::decay_t<decltype(value)>;
                            if constexpr (std::is_arithmetic_v<T> && !std::is_same_v<T, bool>) {
                                const double number = static_cast<double>(value);
                                if (!std::isfinite(number) || (rule->min_value && number < *rule->min_value) ||
                                    (rule->max_value && number > *rule->max_value))
                                    throw std::runtime_error("field is outside its declared range: " + field.name);
                            } else if constexpr (std::is_same_v<T, std::string>) {
                                if (!rule->choices.empty() &&
                                    std::ranges::find(rule->choices, value) == rule->choices.end())
                                    throw std::runtime_error("invalid field choice: " + field.name);
                            }
                        },
                        field.value);
                    if (!patch->source.components().patch_field(source, component.name, field.name, field.value))
                        throw std::runtime_error(patch->source.components().last_error());
                }
        }
        return std::make_unique<DevelopmentChange>(
            DevelopmentChange{[patch](std::string& e) { return patch->activate(e); }, [patch] { patch->rollback(); },
                              [patch] { patch->finalize(); }});
    } catch (const std::exception& e) {
        error = e.what();
        return {};
    }
}
} // namespace kin
