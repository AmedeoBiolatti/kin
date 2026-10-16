#pragma once

#include <kin/ecs/world.hpp>

namespace kin {
// Value handles never retain an ECS world or asset. Resolve through the owning
// session; cross-session, destroyed and previous-generation handles return empty.
struct SceneEntity3D {
    u64 session = 0, generation = 0;
    EcsEntityHandle entity;
    u64 instance = 0; // runtime lease; changes when a pooled slot is recycled
    explicit operator bool() const { return session && entity.id; }
    EcsEntity resolve(EcsWorld& world)const; // checked within a system's live world
    friend bool operator==(const SceneEntity3D&, const SceneEntity3D&) = default;
};
} // namespace kin
