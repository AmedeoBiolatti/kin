#pragma once

#include <kin/core/types.hpp>
#include <kin/ecs/world.hpp>

#include <vector>

namespace kin {

// Runtime incarnation of a reusable entity. Captured script/entity references
// must match this value before acting on a recycled entity.
struct EntityLifetime {
    u64 generation = 0;
};

// The entity's incarnation (0 for one that is never recycled).
inline u64 entity_lifetime(EcsEntity entity) {
    const EntityLifetime* lifetime = entity.get<EntityLifetime>();
    return lifetime ? lifetime->generation : 0;
}

// Whether `entity` is alive and still the incarnation a reference captured.
inline bool same_lifetime(EcsEntity entity, u64 generation) {
    return entity && entity_lifetime(entity) == generation;
}

struct SceneEntity {
};

struct GameplayEntity {
};

struct TransientEntity {
};

struct RenderEntity {
};

// Internal flecs-typed implementations. The public API below takes EcsWorld&;
// flecs stays out of the kin/ecs/lifecycle.hpp surface (templates must remain
// header-resident, so these live in a detail namespace rather than being
// blessed public symbols).
namespace ecs_detail {

template <typename Tag>
i32 destroy_with(flecs::world& world) {
    std::vector<flecs::entity> entities;
    world.query<Tag>().run([&](flecs::iter& it) {
        while (it.next()) {
            for (auto i : it) {
                entities.push_back(it.entity(i));
            }
        }
    });

    i32 destroyed = 0;
    for (flecs::entity entity : entities) {
        if (entity.is_alive()) {
            entity.destruct();
            ++destroyed;
        }
    }
    return destroyed;
}

} // namespace ecs_detail

template <typename Tag>
i32 count_with(EcsWorld& world) {
    return world.count<Tag>();
}

template <typename Tag>
i32 destroy_with(EcsWorld& world) {
    return ecs_detail::destroy_with<Tag>(world.raw());
}

} // namespace kin
