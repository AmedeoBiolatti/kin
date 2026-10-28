#include <kin/ecs/camera.hpp>

#include <kin/ecs/render.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace kin {
namespace {

// `centre` moved so a view of `half` (half its size, in world units) around
// it stays within min..max: centred on them along an axis they are smaller on.
f32 limited(f32 centre, f32 half, f32 min, f32 max) {
    if (max - min <= half * 2.0f) {
        return (min + max) * 0.5f;
    }
    return std::clamp(centre, min + half, max - half);
}

} // namespace

Camera2D camera_of(const EcsEntity& entity, Vec2f viewport) {
    Camera2D camera;
    camera.set_viewport(viewport);
    const auto* component = entity.get<CameraComponent>();
    if (!component) {
        return camera;
    }
    const WorldTransform world = current_world_transform(entity);
    camera.zoom = component->zoom > 0.0f ? component->zoom : 1.0f;
    camera.rotation = world.rotation;
    Vec2f centre = component->smoothing > 0.0f && component->placed ? component->at : world.pos;
    if (component->limited) {
        const Vec2f half{viewport.x * 0.5f / camera.zoom, viewport.y * 0.5f / camera.zoom};
        centre = {limited(centre.x, half.x, component->limit_min.x, component->limit_max.x),
                  limited(centre.y, half.y, component->limit_min.y, component->limit_max.y)};
    }
    camera.look_at(centre);
    return camera;
}

std::optional<Camera2D> scene_camera(EcsWorld& world, Vec2f viewport) {
    flecs::entity found;
    world.raw().each([&](flecs::entity entity, const CameraComponent& camera) {
        if (!found && camera.current) {
            found = entity;
        }
    });
    if (!found) {
        return std::nullopt;
    }
    return camera_of(EcsEntity{found}, viewport);
}

void update_cameras(EcsWorld& world, f32 dt) {
    flecs::world& raw = world.raw();
    std::vector<flecs::entity> smoothing;
    raw.each([&](flecs::entity entity, const CameraComponent& camera) {
        if (camera.smoothing > 0.0f || camera.placed) {
            smoothing.push_back(entity);
        }
    });
    for (const flecs::entity found : smoothing) {
        EcsEntity entity{found};
        // Its own copy, when it is a prefab's (described components are inherited).
        auto* camera = entity.get_mut<CameraComponent>();
        if (!camera) {
            continue;
        }
        const Vec2f target = current_world_transform(entity).pos;
        if (camera->smoothing <= 0.0f) {
            camera->placed = false;
            continue;
        }
        if (!camera->placed) {
            camera->at = target;
            camera->placed = true;
            continue;
        }
        // Closes most of the way (63%) in `smoothing` seconds, whatever the frame rate.
        const f32 t = 1.0f - std::exp(-std::max(dt, 0.0f) / camera->smoothing);
        camera->at = {camera->at.x + (target.x - camera->at.x) * t, camera->at.y + (target.y - camera->at.y) * t};
    }
}

} // namespace kin
