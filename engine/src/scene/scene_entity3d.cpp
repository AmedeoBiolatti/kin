#include "scene_identity3d.hpp"

#include <kin/scene/scene_entity3d.hpp>

namespace kin {
using detail::SceneIdentity3D;
using detail::SceneLease3D;
EcsEntity SceneEntity3D::resolve(EcsWorld& world) const {
    const auto* identity = world.raw().get<SceneIdentity3D>();
    if (!identity || identity->session != session || identity->generation != generation)
        return {};
    auto result = world.entities().resolve(entity);
    if (!result)
        return {};
    if (const auto* lease = result.get<SceneLease3D>()) {
        if (lease->instance != instance || !lease->active)
            return {};
    } else if (instance)
        return {};
    return result;
}
} // namespace kin
