#include <kin/scene/live_scene2d.hpp>

#include <kin/scene/scene_asset.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>

namespace kin {
namespace {

template <class T> const T* named(const std::vector<T>& values, std::string_view name) {
    const auto it = std::ranges::find(values, name, &T::name);
    return it == values.end() ? nullptr : &*it;
}

const SceneEntityDocument* authored(const SceneDocument& document, std::string_view id) {
    const auto it = std::ranges::find(document.entities, id, &SceneEntityDocument::id);
    return it == document.entities.end() ? nullptr : &*it;
}

// Entity references compare by authored id: runtime ids differ between worlds.
bool same(const ComponentFieldValue& a, const ComponentFieldValue& b) {
    if (const auto* ref = std::get_if<ComponentEntityRef>(&a)) {
        const auto* other = std::get_if<ComponentEntityRef>(&b);
        return other && ref->authored_id == other->authored_id;
    }
    return a == b;
}

// A changed authored field, checked against its component's declaration.
struct FieldEdit {
    std::string entity;
    std::string component;
    std::string field;
    ComponentFieldValue value;
};

void check_edit(const ComponentFieldDescriptor& rule, const FieldEdit& edit, const SceneDocument& after) {
    if (!detail::field_value_matches_kind(edit.value, rule.kind)) {
        throw std::runtime_error("type mismatch for field " + edit.component + "." + edit.field);
    }
    std::visit(
        [&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_arithmetic_v<T> && !std::is_same_v<T, bool>) {
                const double number = static_cast<double>(value);
                if (!std::isfinite(number) || (rule.min_value && number < *rule.min_value) ||
                    (rule.max_value && number > *rule.max_value)) {
                    throw std::runtime_error("field is outside its declared range: " + edit.field);
                }
            } else if constexpr (std::is_same_v<T, std::string>) {
                if (!rule.choices.empty() && std::ranges::find(rule.choices, value) == rule.choices.end()) {
                    throw std::runtime_error("invalid field choice: " + edit.field);
                }
            } else if constexpr (std::is_same_v<T, ComponentEntityRef>) {
                if (!value.authored_id.empty() && !authored(after, value.authored_id)) {
                    throw std::runtime_error("field refers to an entity the scene does not have: " + edit.field);
                }
            }
        },
        edit.value);
}

struct Patch {
    EcsWorld* world = nullptr;
    EcsWorld backup; // the authored entities' components before activation
    SceneDocument before;
    SceneDocument after;
    std::vector<FieldEdit> edits;
    std::map<std::string, EcsEntity, std::less<>> live;
    std::map<std::string, EcsEntity, std::less<>> saved;
    std::map<std::string, EntityDescriptor, std::less<>> descriptors;
    std::map<std::string, std::vector<EcsId>, std::less<>> bases;
    std::map<std::string, std::set<std::string>, std::less<>> owned;
    std::vector<EcsEntity> created;
    std::set<std::string> removing;

    void activate_structure(EcsComponentRegistry& registry) {
        for (const auto& row : after.entities) {
            if (!live.contains(row.id)) {
                auto entity = world->entities().create({.name = row.name, .authored_id = row.id});
                if (!entity) {
                    throw std::runtime_error("cannot create authored entity " + row.id);
                }
                live[row.id] = entity;
                created.push_back(entity);
            }
        }
        for (const auto& row : after.entities) {
            const SceneEntityDocument* old = authored(before, row.id);
            const EcsEntity entity = live.at(row.id);
            if ((!old || old->name != row.name) && !world->entities().rename(entity, row.name)) {
                throw std::runtime_error("cannot rename " + row.id);
            }
            if (!old || old->parent != row.parent) {
                const bool moved = row.parent.empty() ? world->entities().detach_parent(entity)
                                                      : world->entities().reparent(entity, live.at(row.parent));
                if (!moved) {
                    throw std::runtime_error("cannot reparent " + row.id);
                }
            }
            for (const auto& component : row.components) {
                const bool authored_before = old && named(old->components, component.name);
                if (!authored_before && !registry.add(entity, component.name)) {
                    throw std::runtime_error(registry.last_error());
                }
            }
            if (!old) {
                continue;
            }
            for (const auto& component : old->components) {
                if (named(row.components, component.name)) {
                    continue;
                }
                const bool removed = entity.has<PrefabEntityComponent>()
                                         ? remove_prefab_component(*world, entity, component.name)
                                         : registry.remove(entity, component.name);
                if (!removed) {
                    throw std::runtime_error("cannot remove component " + component.name);
                }
            }
        }
    }

    bool activate(std::string& error) {
        try {
            auto& registry = world->components();
            activate_structure(registry);
            for (const FieldEdit& edit : edits) {
                const EcsEntity entity = live.at(edit.entity);
                ComponentFieldValue value = edit.value;
                if (auto* ref = std::get_if<ComponentEntityRef>(&value)) {
                    ref->id = ref->authored_id.empty() ? 0 : live.at(ref->authored_id).id();
                }
                if (!registry.has(entity, edit.component) && !registry.add(entity, edit.component)) {
                    throw std::runtime_error(registry.last_error());
                }
                if (!registry.patch_field(entity, edit.component, edit.field, value)) {
                    throw std::runtime_error(registry.last_error());
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
            for (auto base : bases.at(id)) {
                entity.raw().add(flecs::IsA, base);
            }
            for (const auto& component : registry.snapshots(entity)) {
                if (!backup.components().has(copy, component.descriptor.name)) {
                    registry.remove(entity, component.descriptor.name);
                }
            }
            for (const auto& component : backup.components().snapshots(copy)) {
                if (!backup.components().copy_component(copy, component.descriptor.name, registry, entity)) {
                    std::terminate();
                }
            }
            for (const auto& component : backup.components().snapshots(copy)) {
                if (!owned.at(id).contains(component.descriptor.name)) {
                    registry.remove(entity, component.descriptor.name);
                }
            }
            const auto& original = descriptors.at(id);
            world->entities().rename(entity, original.name);
            if (auto parent = world->entities().find_by_id(original.parent)) {
                world->entities().reparent(entity, parent);
            } else {
                world->entities().detach_parent(entity);
            }
        }
        for (auto entity : created) {
            if (entity) {
                world->entities().destroy(entity);
            }
        }
        created.clear();
    }

    // Deletions wait for the whole reload: entities that only lose their
    // authored parent are detached instead.
    void finalize() {
        for (const auto& id : removing) {
            const auto entity = live.at(id);
            if (!entity) {
                continue;
            }
            const auto children = world->snapshot(entity).descriptor.children;
            for (auto child : children) {
                auto handle = world->entities().find_by_id(child);
                const auto child_id = world->entities().authored_id(handle);
                if (!child_id || !removing.contains(*child_id)) {
                    world->entities().detach_parent(handle);
                }
            }
            world->entities().destroy(entity);
        }
    }

    // What activate changes, checked before anything live changes.
    void plan() {
        auto& registry = world->components();
        for (const auto& row : before.entities) {
            auto entity = world->entities().find_by_authored_id(row.id);
            if (!entity) {
                throw std::runtime_error("authored entity no longer exists: " + row.id);
            }
            live[row.id] = entity;
            descriptors[row.id] = world->snapshot(entity).descriptor;
            auto& entity_bases = bases[row.id];
            for (i32 i = 0;; ++i) {
                const auto base = ecs_get_target(world->raw().c_ptr(), entity.id(), flecs::IsA, i);
                if (!base) {
                    break;
                }
                entity_bases.push_back(base);
            }
            auto& entity_owned = owned[row.id];
            auto copy = backup.entities().create({.authored_id = row.id});
            saved[row.id] = copy;
            for (const auto& component : registry.snapshots(entity)) {
                if (entity.raw().owns(component.descriptor.id)) {
                    entity_owned.insert(component.descriptor.name);
                }
                if (!registry.copy_component(entity, component.descriptor.name, backup.components(), copy)) {
                    throw std::runtime_error(backup.components().last_error());
                }
            }
            if (!authored(after, row.id)) {
                removing.insert(row.id);
            }
        }
        for (const auto& row : after.entities) {
            const SceneEntityDocument* old = authored(before, row.id);
            if (!old && world->entities().find_by_authored_id(row.id)) {
                throw std::runtime_error("new authored ID collides with runtime entity: " + row.id);
            }
            if (!row.parent.empty() && !authored(after, row.parent)) {
                throw std::runtime_error("authored parent does not exist: " + row.parent);
            }
            for (const auto& component : row.components) {
                const ComponentDescriptor* descriptor = registry.find(component.name);
                if (!descriptor) {
                    throw std::runtime_error("unknown component: " + component.name);
                }
                const auto* prior = old ? named(old->components, component.name) : nullptr;
                for (const auto& field : component.fields) {
                    const auto* previous = prior ? named(prior->fields, field.name) : nullptr;
                    if (previous && same(previous->value, field.value)) {
                        continue;
                    }
                    const auto* rule = named(descriptor->fields, field.name);
                    if (!rule || (rule->readonly && previous)) {
                        throw std::runtime_error("field is missing or readonly: " + component.name + "." + field.name);
                    }
                    if (rule->readonly) {
                        continue;
                    }
                    FieldEdit edit{
                        .entity = row.id, .component = component.name, .field = field.name, .value = field.value};
                    check_edit(*rule, edit, after);
                    edits.push_back(std::move(edit));
                }
            }
        }
    }
};

} // namespace

SceneDocument authored_scene_document(EcsWorld& world, const SceneInstance& instance) {
    auto document = scene_document_from_world(world);
    std::erase_if(document.entities, [&](const auto& row) { return !instance.by_authored_id.contains(row.id); });
    for (auto& row : document.entities) {
        for (auto& component : row.components) {
            const auto* descriptor = world.components().find(component.name);
            if (!descriptor) {
                continue;
            }
            std::erase_if(component.fields, [&](const auto& field) {
                const auto* rule = named(descriptor->fields, field.name);
                return rule && rule->readonly;
            });
        }
    }
    return document;
}

std::unique_ptr<DevelopmentChange> prepare_scene_patch2d(EcsWorld& world, const SceneDocument& before,
                                                         const SceneDocument& after, std::string& error) {
    try {
        if (world.components().has_field_patch_handler()) {
            throw std::runtime_error("this world requires its owner's transactional field edit policy");
        }
        auto patch = std::make_shared<Patch>();
        patch->world = &world;
        patch->before = before;
        patch->after = after;
        world.components().copy_schema_to(patch->backup.components());
        patch->plan();
        return std::make_unique<DevelopmentChange>(DevelopmentChange{
            .activate = [patch](std::string& failure) { return patch->activate(failure); },
            .rollback = [patch] { patch->rollback(); },
            .finalize = [patch] { patch->finalize(); },
        });
    } catch (const std::exception& e) {
        error = e.what();
        return {};
    }
}

} // namespace kin
