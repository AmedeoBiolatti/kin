#include <kin/runtime/state_hash.hpp>

#include <kin/core/json.hpp>
#include <kin/ecs/world.hpp>
#include <kin/scene/scene_manager.hpp>

#include <flecs.h>

#include <algorithm>
#include <sstream>
#include <tuple>
#include <unordered_map>

namespace kin {
namespace {

u64 mix(u64 x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebull;
    x ^= x >> 31;
    return x;
}

u64 key_hash(i32 scene, u64 entity, u64 id) {
    return mix(mix(mix(static_cast<u64>(scene) + 1) ^ entity) ^ id);
}

// Pseudo component ids for the items that are not components.
constexpr u64 type_item = ~u64{0};
constexpr u64 report_item = ~u64{0} - 1;

// Entities that are the ECS's own bookkeeping rather than game state:
// component and type definitions, modules, systems, observers, queries, and
// anything inside the flecs namespace. Their data holds function pointers and
// other addresses that differ between processes.
class InternalEntities {
public:
    explicit InternalEntities(ecs_world_t* world)
        : _world(world) {}

    bool operator()(ecs_table_t* table, ecs_entity_t entity) {
        const auto [found, inserted] = _tables.try_emplace(table, false);
        if (inserted) {
            found->second = ecs_table_has_id(_world, table, ecs_id(EcsComponent)) ||
                ecs_table_has_id(_world, table, EcsModule) ||
                ecs_table_has_id(_world, table, ecs_pair(ecs_id(EcsPoly), EcsWildcard)) || in_flecs(entity);
        }
        return found->second;
    }

private:
    // Entities of one table share their parent, so the answer holds per table.
    bool in_flecs(ecs_entity_t entity) const {
        for (ecs_entity_t parent = ecs_get_parent(_world, entity); parent; parent = ecs_get_parent(_world, parent)) {
            if (parent == EcsFlecs) {
                return true;
            }
        }
        return entity == EcsFlecs;
    }

    ecs_world_t* _world;
    std::unordered_map<ecs_table_t*, bool> _tables;
};

bool plain_data(const ecs_type_info_t* info) {
    return info && !info->hooks.dtor && !info->hooks.copy && !info->hooks.move;
}

std::string report_json(const Scene& scene) {
    std::ostringstream out;
    JsonWriter json(out, false);
    json.begin_object();
    scene.write_report(json);
    json.end_object();
    return out.str();
}

std::string id_name(ecs_world_t* world, ecs_id_t id) {
    char* text = ecs_id_str(world, id);
    std::string name = text ? text : std::to_string(id);
    ecs_os_free(text);
    return name;
}

// Calls item(scene, scene_ptr, world, entity, id, data, size) for each item of
// the scenes' state; data is null for a component type not compared.
template<typename Item>
void visit_state(SceneManager& scenes, Item&& item) {
    for (i32 index = 0; index < scenes.depth(); ++index) {
        Scene* scene = scenes.at_mut(index);
        if (!scene) {
            continue;
        }
        const std::string report = report_json(*scene);
        item(index, *scene, nullptr, 0, report_item, report.data(), report.size());
        EcsWorld* ecs = scene->world();
        if (!ecs) {
            continue;
        }
        ecs_world_t* world = ecs->raw().c_ptr();
        InternalEntities internal{world};
        const ecs_entities_t entities = ecs_get_entities(world);
        for (i32 i = 0; i < entities.alive_count; ++i) {
            const ecs_entity_t entity = entities.ids[i];
            const ecs_record_t* record = ecs_record_find(world, entity);
            if (!record || !record->table) {
                continue;
            }
            ecs_table_t* table = record->table;
            if (internal(table, entity)) {
                continue;
            }
            const ecs_type_t* type = ecs_table_get_type(table);
            const i32 row = ECS_RECORD_TO_ROW(record->row);
            item(index, *scene, world, entity, type_item, type->array, static_cast<std::size_t>(type->count) * sizeof(ecs_id_t));
            for (i32 t = 0; t < type->count; ++t) {
                const i32 column = ecs_table_type_to_column_index(table, t);
                if (column < 0) {
                    continue; // a tag: already in the type
                }
                const ecs_id_t id = type->array[t];
                if (!plain_data(ecs_get_type_info(world, id))) {
                    item(index, *scene, world, entity, id, nullptr, 0);
                    continue;
                }
                item(index, *scene, world, entity, id, ecs_table_get_column(table, column, row),
                     ecs_table_get_column_size(table, column));
            }
        }
    }
}

} // namespace

u64 hash_bytes(const void* data, std::size_t size, u64 seed) {
    const auto* bytes = static_cast<const u8*>(data);
    u64 hash = seed;
    for (std::size_t i = 0; i < size; ++i) {
        hash = (hash ^ bytes[i]) * 0x100000001b3ull;
    }
    return hash;
}

u64 hash_state(SceneManager& scenes, StateCoverage* coverage) {
    // Summed, so the order entities are stored in does not matter.
    u64 total = 0;
    u64 count = 0;
    std::vector<ecs_id_t> skipped;
    std::vector<std::pair<ecs_world_t*, ecs_id_t>> skipped_named;
    visit_state(scenes, [&](i32 scene, Scene&, ecs_world_t* world, u64 entity, u64 id, const void* data, std::size_t size) {
        if (!data) {
            if (coverage && std::ranges::find(skipped, id) == skipped.end()) {
                skipped.push_back(id);
                skipped_named.emplace_back(world, id);
            }
            return;
        }
        total += mix(key_hash(scene, entity, id) ^ hash_bytes(data, size));
        ++count;
        if (coverage) {
            coverage->scenes += id == report_item ? 1 : 0;
            coverage->entities += id == type_item ? 1 : 0;
            coverage->values += id != type_item && id != report_item ? 1 : 0;
        }
    });
    if (coverage) {
        for (const auto& [world, id] : skipped_named) {
            coverage->not_compared.push_back(id_name(world, id));
        }
        std::sort(coverage->not_compared.begin(), coverage->not_compared.end());
        coverage->not_compared.erase(std::unique(coverage->not_compared.begin(), coverage->not_compared.end()),
                                     coverage->not_compared.end());
    }
    return mix(total ^ mix(count));
}

std::vector<StateItem> describe_state(SceneManager& scenes) {
    std::vector<StateItem> items;
    visit_state(scenes, [&](i32 scene, Scene& owner, ecs_world_t* world, u64 entity, u64 id, const void* data, std::size_t size) {
        if (!data) {
            return;
        }
        StateItem item{.scene = scene, .scene_name = std::string{owner.name()}, .entity = entity};
        if (world) {
            const char* name = ecs_get_name(world, entity);
            item.entity_name = name ? name : "";
        }
        item.component = id == type_item ? "(type)" : id == report_item ? "(report)" : id_name(world, id);
        item.hash = hash_bytes(data, size);
        item.bytes.assign(static_cast<const u8*>(data), static_cast<const u8*>(data) + size);
        items.push_back(std::move(item));
    });
    std::sort(items.begin(), items.end(), [](const StateItem& a, const StateItem& b) {
        return std::tie(a.scene, a.entity, a.component) < std::tie(b.scene, b.entity, b.component);
    });
    return items;
}

} // namespace kin
