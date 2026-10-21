#pragma once

// Example game code, shared with its lifecycle regression. No editor schema or
// host-side spawn/contact/despawn loop is needed.
#include <array>
#include <kin/ecs/system.hpp>
#include <kin/scene/scene_session3d.hpp>
#include <stdexcept>

namespace projectile_range {
using namespace kin;
struct Health {
    int value = 3;
};
struct Projectile {
    float lifetime = 2;
    int damage = 1;
};
struct Stats {
    int shots = 0, hits = 0, deaths = 0, respawns = 0, expired = 0, post_physics_steps = 0;
};
struct Resources {
    ScenePreparedPrefab3D target, projectile;
    ScenePrefabPool3D pool;
};
// Explicit level-loading work, between frames. Keep ownership in the session;
// the game stores generation-scoped values, never a second owning asset cache.
inline bool prepare(SceneSession3D& session) {
    Resources resources;
    resources.target = session.preload("prefabs/range-target.kinprefab").value_or({});
    if (!resources.target)
        return false;
    resources.projectile = session.preload("prefabs/range-projectile.kinprefab").value_or({});
    if (!resources.projectile)
        return false;
    resources.pool =
        session
            .create_pool(resources.projectile, {.capacity = 16,
                                                .reset = [](auto& context) { context.root.set(Projectile{}); },
                                                .release = [](auto& context) { context.root.set(Projectile{}); }})
            .value_or({});
    if (!resources.pool)
        return false;
    session.world()->raw().set(resources);
    return true;
}

inline void install(SceneSession3DContext& scene) {
    scene.world.component<Health>("RangeHealth");
    scene.world.component<Projectile>("RangeProjectile");
    struct Shot {
        SceneRequest3D birth, removal;
    };
    struct Game {
        std::array<Shot, 16> shots;
        SceneRequest3D target, removal;
        float fire = 0, respawn = 0;
        bool had_target = false;
    };
    auto game = std::make_shared<Game>();
    scene.world.raw().set(Stats{});
    const auto commands = scene.commands;
    scene.world.systems().register_task(
        {.id = "range.gameplay", .structural_mutation_policy = SystemStructuralMutationPolicy::CommandBufferOnly},
        [game, commands](SystemContext& context) {
            const auto require = [](const SceneRequest3D& request) {
                if (request.status() == SceneRequestStatus3D::Failed ||
                    request.status() == SceneRequestStatus3D::Cancelled)
                    throw std::runtime_error(request.error());
            };
            auto& world = *context.world;
            auto& buffer = *context.commands;
            const auto* resources = world.raw().get<Resources>();
            if (!resources)
                throw std::runtime_error("projectile resources must be prepared before stepping the level");
            auto* stats = world.raw().get_mut<Stats>();
            require(game->target);
            require(game->removal);
            auto target = game->target.prefab().root.resolve(world);
            for (auto& shot : game->shots) {
                require(shot.birth);
                require(shot.removal);
                if (shot.removal.status() == SceneRequestStatus3D::Succeeded) {
                    shot = {};
                    continue;
                }
            }
            if (game->removal.status() == SceneRequestStatus3D::Succeeded) {
                game->respawn -= context.dt;
                if (game->respawn <= 0) {
                    game->target = {};
                    game->removal = {};
                }
            }
            if (game->target.status() == SceneRequestStatus3D::Invalid) {
                game->target = commands.spawn(
                    buffer, resources->target,
                    {.position = Vec3f{2, 0, 0}, .configure = [](auto& ctx) { ctx.root.set(Health{}); }});
                if (game->had_target)
                    ++stats->respawns;
                game->had_target = true;
            }
            game->fire -= context.dt;
            if (target && target.get<Health>()->value > 0 && game->removal.status() == SceneRequestStatus3D::Invalid &&
                game->fire <= 0) {
                for (auto& shot : game->shots)
                    if (shot.birth.status() == SceneRequestStatus3D::Invalid) {
                        shot.birth = commands.spawn(buffer, resources->pool, {.position = Vec3f{-2.2f, .5f, 0}});
                        ++stats->shots;
                        game->fire = .35f;
                        break;
                    }
            }
        });
    scene.world.systems().register_task(
        {.id = "range.contacts",
         .phase = SystemPhase::PostPhysics,
         .structural_mutation_policy = SystemStructuralMutationPolicy::CommandBufferOnly},
        [game, commands](SystemContext& context) {
            auto& world = *context.world;
            auto& buffer = *context.commands;
            auto* stats = world.raw().get_mut<Stats>();
            ++stats->post_physics_steps;
            auto target = game->target.prefab().root.resolve(world);
            // Fresh contacts and physics writeback from this fixed step. New births
            // are ready at the preceding boundary, even on their first step.
            const auto* contacts = world.raw().get<PhysicsContacts3D>();
            for (auto& shot : game->shots) {
                auto entity = shot.birth.prefab().root.resolve(world);
                if (!entity || shot.removal.status() != SceneRequestStatus3D::Invalid)
                    continue;
                auto& projectile = *entity.get_mut<Projectile>();
                bool hit = false;
                const auto body = entity.get<PhysicsBody3D>()->body;
                const auto target_body = target ? target.get<PhysicsBody3D>()->body : Body3D{};
                if (target && contacts && game->removal.status() == SceneRequestStatus3D::Invalid)
                    for (const auto& contact : contacts->events)
                        if (contact.kind == ContactKind3D::Begin && ((contact.a == body && contact.b == target_body) ||
                                                                     (contact.b == body && contact.a == target_body))) {
                            target.get_mut<Health>()->value -= projectile.damage;
                            hit = true;
                            ++stats->hits;
                            break;
                        }
                projectile.lifetime -= context.dt;
                if (hit || projectile.lifetime <= 0) {
                    shot.removal = commands.despawn(buffer, shot.birth);
                    if (!hit)
                        ++stats->expired;
                }
            }
            if (target && target.get<Health>()->value <= 0 && game->removal.status() == SceneRequestStatus3D::Invalid) {
                game->removal = commands.despawn(buffer, game->target);
                game->respawn = .7f;
                ++stats->deaths;
            }
        });
}
} // namespace projectile_range
