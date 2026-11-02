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

// What a camera keeps to follow its entity with: it smooths, has a dead zone, or looks ahead.
bool follows(const CameraComponent& camera) {
    return camera.smoothing > 0.0f || camera.dead_zone.x > 0.0f || camera.dead_zone.y > 0.0f || camera.look_ahead.x > 0.0f ||
           camera.look_ahead.y > 0.0f;
}

// A u64's bits stirred (splitmix64's finish).
u64 scramble(u64 x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

// Smooth noise, -1 to 1, along `t` (value noise between whole steps): the
// same for the same `seed` and `t`.
f32 noise(u64 seed, f64 t) {
    const f64 step = std::floor(t);
    const auto at = [&](f64 i) {
        const u64 bits = scramble(seed ^ scramble(static_cast<u64>(static_cast<i64>(i))));
        return static_cast<f32>(bits >> 40) / static_cast<f32>(1u << 23) - 1.0f; // 24 bits, -1 to 1
    };
    const f32 f = static_cast<f32>(t - step);
    const f32 eased = f * f * (3.0f - 2.0f * f);
    return at(step) + (at(step + 1.0) - at(step)) * eased;
}

// How much of the way to its goal something easing over `seconds` goes in `dt` (63% in `seconds`).
f32 eased(f32 dt, f32 seconds) {
    return seconds > 0.0f ? 1.0f - std::exp(-std::max(dt, 0.0f) / seconds) : 1.0f;
}

// `goal` moved along an axis no further than needed to keep `target` inside
// a box `size` big around it (an axis of size 0: on `target`).
f32 in_zone(f32 goal, f32 target, f32 size) {
    if (size <= 0.0f) {
        return target;
    }
    const f32 half = size * 0.5f;
    return std::clamp(goal, target - half, target + half);
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
    const f32 zoom = component->zoom_smoothing > 0.0f && component->shown_zoom > 0.0f ? component->shown_zoom : component->zoom;
    camera.zoom = zoom > 0.0f ? zoom : 1.0f;
    camera.rotation = world.rotation;
    Vec2f centre = follows(*component) && component->placed ? component->at : world.pos;
    if (component->limited) {
        const Vec2f half{viewport.x * 0.5f / camera.zoom, viewport.y * 0.5f / camera.zoom};
        centre = {limited(centre.x, half.x, component->limit_min.x, component->limit_max.x),
                  limited(centre.y, half.y, component->limit_min.y, component->limit_max.y)};
    }
    camera.look_at(centre);
    camera.shake_offset = component->shake_offset; // on top of the limits: a shake may show past them
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
    std::vector<flecs::entity> moving;
    raw.each([&](flecs::entity entity, const CameraComponent& camera) {
        if (follows(camera) || camera.placed || camera.zoom_smoothing > 0.0f || camera.shown_zoom > 0.0f || camera.trauma > 0.0f ||
            camera.shake_offset.x != 0.0f || camera.shake_offset.y != 0.0f) {
            moving.push_back(entity);
        }
    });
    dt = std::max(dt, 0.0f);
    for (const flecs::entity found : moving) {
        EcsEntity entity{found};
        // Its own copy, when it is a prefab's (described components are inherited).
        auto* camera = entity.get_mut<CameraComponent>();
        if (!camera) {
            continue;
        }

        // Following its entity.
        const Vec2f target = current_world_transform(entity).pos;
        if (!follows(*camera)) {
            camera->placed = false;
        } else if (!camera->placed) {
            camera->at = target;
            camera->focus = target;
            camera->lead = {};
            camera->last_target = target;
            camera->placed = true;
        } else {
            // Ahead the way it moves: its motion over look_ahead_time, no further than look_ahead.
            Vec2f wanted{};
            if (dt > 0.0f) {
                const auto ahead = [&](f32 moved, f32 most) {
                    return most > 0.0f ? std::clamp(moved / dt * camera->look_ahead_time, -most, most) : 0.0f;
                };
                wanted = {ahead(target.x - camera->last_target.x, camera->look_ahead.x),
                          ahead(target.y - camera->last_target.y, camera->look_ahead.y)};
            }
            const f32 turn = eased(dt, std::max(camera->look_ahead_time, 0.05f));
            camera->lead = {camera->lead.x + (wanted.x - camera->lead.x) * turn, camera->lead.y + (wanted.y - camera->lead.y) * turn};
            camera->last_target = target;
            // Where it means to look: moved only as far as keeps the entity (and
            // the way ahead) inside its dead zone.
            const Vec2f goal{target.x + camera->lead.x, target.y + camera->lead.y};
            camera->focus = {in_zone(camera->focus.x, goal.x, camera->dead_zone.x), in_zone(camera->focus.y, goal.y, camera->dead_zone.y)};
            // Caught up with, at once or over `smoothing`.
            const f32 t = eased(dt, camera->smoothing);
            camera->at = {camera->at.x + (camera->focus.x - camera->at.x) * t, camera->at.y + (camera->focus.y - camera->at.y) * t};
        }

        // Zooming.
        if (camera->zoom_smoothing > 0.0f) {
            if (camera->shown_zoom <= 0.0f) {
                camera->shown_zoom = camera->zoom;
            }
            // Eased in proportion (a zoom from 1 to 2 feels as long as one from 2 to 4).
            const f32 from = std::log(std::max(camera->shown_zoom, 0.001f));
            const f32 to = std::log(std::max(camera->zoom, 0.001f));
            camera->shown_zoom = std::exp(from + (to - from) * eased(dt, camera->zoom_smoothing));
        } else {
            camera->shown_zoom = 0.0f;
        }

        // Shaking: trauma wearing off, the camera thrown about by its square.
        camera->trauma = std::max(0.0f, camera->trauma - std::max(camera->shake_decay, 0.0f) * dt);
        if (camera->trauma <= 0.0f) {
            camera->shake_offset = {};
            camera->shake_time = 0.0;
            continue;
        }
        camera->shake_time += dt;
        const f64 t = camera->shake_time * static_cast<f64>(std::max(camera->shake_frequency, 0.0f));
        const f32 amount = camera->trauma * camera->trauma;
        const u64 seed = scramble(static_cast<u64>(entity.id()));
        camera->shake_offset = {camera->shake_strength * amount * noise(seed ^ 0x9e3779b97f4a7c15ull, t),
                                camera->shake_strength * amount * noise(seed ^ 0xc2b2ae3d27d4eb4full, t)};
    }
}

void shake_camera(EcsEntity camera, f32 trauma) {
    if (!camera || !camera.raw().has<CameraComponent>()) {
        return;
    }
    auto* component = camera.get_mut<CameraComponent>();
    component->trauma = std::clamp(component->trauma + std::max(trauma, 0.0f), 0.0f, 1.0f);
}

void shake_camera(EcsWorld& world, f32 trauma) {
    std::vector<flecs::entity> current;
    world.raw().each([&](flecs::entity entity, const CameraComponent& camera) {
        if (camera.current) {
            current.push_back(entity);
        }
    });
    for (const flecs::entity entity : current) {
        shake_camera(EcsEntity{entity}, trauma);
    }
}

} // namespace kin
