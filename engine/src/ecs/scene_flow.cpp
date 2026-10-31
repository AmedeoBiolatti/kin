#include <kin/ecs/scene_flow.hpp>

#include <kin/ecs/render.hpp>
#include <kin/platform/input.hpp>
#if KIN_ENABLE_PHYSICS
#include <kin/ecs/physics.hpp>
#endif

#include <algorithm>
#include <cmath>
#include <utility>

namespace kin {

SceneTravel travel_of(const SceneDoor& door, std::optional<EcsId> id) {
    return {
        .scene = door.scene.id,
        .arrive = door.arrive,
        .transition = door.transition,
        .duration = door.duration,
        .color = door.color,
        .door = id,
    };
}

bool door_contains(const SceneDoor& door, const WorldTransform& at, Vec2f point) {
    const Vec2f half{std::abs(door.size.x * at.scale.x) * 0.5f, std::abs(door.size.y * at.scale.y) * 0.5f};
    const Vec2f centre{at.pos.x + door.offset.x * at.scale.x, at.pos.y + door.offset.y * at.scale.y};
    return std::abs(point.x - centre.x) <= half.x && std::abs(point.y - centre.y) <= half.y;
}

std::vector<SceneFlow::Inside> SceneFlow::standing(EcsWorld& world) const {
    std::vector<std::pair<EcsId, Vec2f>> travellers;
    world.raw().each([&](flecs::entity entity, const SceneTraveller& traveller) {
        if (traveller.enabled && entity.enabled()) {
            travellers.emplace_back(static_cast<EcsId>(entity.id()), current_world_transform(entity).pos);
        }
    });
    std::vector<Inside> inside;
    if (travellers.empty()) {
        return inside;
    }
    world.raw().each([&](flecs::entity entity, const SceneDoor& door) {
        if (!door.enabled || !entity.enabled()) {
            return;
        }
        const WorldTransform at = current_world_transform(entity);
        for (const auto& [id, point] : travellers) {
            if (id != static_cast<EcsId>(entity.id()) && door_contains(door, at, point)) {
                inside.push_back({id, static_cast<EcsId>(entity.id())});
            }
        }
    });
    return inside;
}

void SceneFlow::update(EcsWorld& world, const Input& input) {
    std::vector<Inside> now = standing(world);
    if (!_noted) {
        // Where they stand as the flow starts (or the scene was read again): no one goes through yet.
        _inside = std::move(now);
        _noted = true;
        return;
    }
    for (const Inside& in : now) {
        if (_pending) {
            break; // one travel at a time
        }
        const flecs::entity entity = world.raw().entity(in.door);
        const SceneDoor* door = entity.is_alive() ? entity.get<SceneDoor>() : nullptr;
        if (!door) {
            continue;
        }
        const bool entered = std::ranges::find(_inside, in) == _inside.end();
        const bool used = door->trigger == DoorTrigger::Touch ? entered : input.pressed(door->action);
        if (used) {
            _pending = travel_of(*door, in.door);
        }
    }
    _inside = std::move(now);
}

void SceneFlow::go(SceneTravel travel) {
    _pending = std::move(travel);
}

std::optional<SceneTravel> SceneFlow::take() {
    return std::exchange(_pending, std::nullopt);
}

void SceneFlow::reset() {
    _inside.clear();
    _noted = false;
}

bool SceneFlow::arrive(EcsWorld& world, std::string_view arrive) {
    if (arrive.empty()) {
        reset();
        return true;
    }
    const EcsEntity target = world.entities().find_by_name(arrive);
    if (!target) {
        reset();
        return false;
    }
    WorldTransform at = current_world_transform(target);
    if (const SceneDoor* door = target.raw().get<SceneDoor>()) {
        at.pos.x += door->exit.x * at.scale.x;
        at.pos.y += door->exit.y * at.scale.y;
    }
    std::vector<flecs::entity> travellers;
    world.raw().each([&](flecs::entity entity, const SceneTraveller& traveller) {
        if (traveller.enabled && entity.id() != target.raw().id()) {
            travellers.push_back(entity);
        }
    });
    for (flecs::entity entity : travellers) {
        Transform2D* transform = entity.get_mut<Transform2D>();
        if (!transform) {
            continue;
        }
        const flecs::entity parent = entity.parent();
        if (parent && parent.is_alive()) {
            const WorldTransform here = current_world_transform(entity);
            const Transform2D local = to_local(current_world_transform(parent), {at.pos, here.rotation, here.scale});
            transform->pos = local.pos;
        } else {
            transform->pos = at.pos;
        }
        entity.modified<Transform2D>();
#if KIN_ENABLE_PHYSICS
        if (entity.has<PhysicsBodyComponent>()) {
            entity.add<PhysicsTeleport>(); // its body with it, at the next step
        }
#endif
    }
    // They stand where they arrived: the doors there wait for them to step out.
    _inside = standing(world);
    _noted = true;
    return true;
}

std::optional<EcsEntity> SceneFlow::action_door(EcsWorld& world) const {
    for (const Inside& in : standing(world)) {
        const flecs::entity entity = world.raw().entity(in.door);
        const SceneDoor* door = entity.get<SceneDoor>();
        if (door && door->trigger == DoorTrigger::Action) {
            return EcsEntity{entity};
        }
    }
    return std::nullopt;
}

} // namespace kin
