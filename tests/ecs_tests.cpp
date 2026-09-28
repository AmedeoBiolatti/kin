#include <kin/ecs/ecs_scene.hpp>
#include <kin/ecs/world.hpp>

#include <cassert>

namespace {

struct Position {
    kin::f32 x = 0.0f;
    kin::f32 y = 0.0f;
};

struct Velocity {
    kin::f32 x = 0.0f;
    kin::f32 y = 0.0f;
};

struct PlayerTag {
};

} // namespace

int main() {
    kin::EcsWorld world;
    world.component<Position>("Position");
    world.component<Velocity>("Velocity");
    world.component<PlayerTag>("PlayerTag");

    kin::EcsEntity player = world.entity("player")
                                .set(Position{1.0f, 2.0f})
                                .set(Velocity{3.0f, 4.0f})
                                .add<PlayerTag>();

    assert(player.valid());
    assert(player.alive());
    assert(player.name() == "player");
    assert(player.has<Position>());
    assert(player.has<Velocity>());
    assert(player.has<PlayerTag>());
    assert(player.get<Position>()->x == 1.0f);

    Position* position = player.get_mut<Position>();
    assert(position != nullptr);
    position->x = 5.0f;
    player.modified<Position>();
    assert(player.get<Position>()->x == 5.0f);

    world.entity("static").set(Position{10.0f, 20.0f});
    assert(world.count<Position>() == 2);
    const bool moving_entity_count_ok = world.count<Position, Velocity>() == 1;
    assert(moving_entity_count_ok);

    kin::i32 moved = 0;
    world.query<Position, Velocity>().each([&](flecs::entity, Position& pos, const Velocity& vel) {
        pos.x += vel.x;
        pos.y += vel.y;
        ++moved;
    });

    assert(moved == 1);
    assert(player.get<Position>()->x == 8.0f);
    assert(player.get<Position>()->y == 6.0f);

    world.raw().system<Position, const Velocity>("Move")
        .each([](Position& pos, const Velocity& vel) {
            pos.x += vel.x;
            pos.y += vel.y;
        });
    world.progress(1.0f);

    assert(player.get<Position>()->x == 11.0f);
    assert(player.get<Position>()->y == 10.0f);

    player.remove<PlayerTag>();
    assert(!player.has<PlayerTag>());

    player.destroy();
    assert(!player.alive());

    kin::EcsScene owned_scene;
    assert(!owned_scene.uses_shared_world());
    owned_scene.ecs().entity("owned").set(Position{1.0f, 1.0f});
    assert(owned_scene.ecs().count<Position>() == 1);

    kin::EcsWorld shared_world;
    kin::EcsScene shared_scene{&shared_world};
    assert(shared_scene.uses_shared_world());
    shared_scene.ecs().entity("shared").set(Position{2.0f, 2.0f});
    assert(shared_world.count<Position>() == 1);

    return 0;
}
