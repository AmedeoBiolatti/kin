#include <kin/ecs/spawn.hpp>

#include <cassert>
#include <string>

namespace {

struct Position {
    kin::f32 x = 0.0f;
    kin::f32 y = 0.0f;
};

struct Name {
    std::string value;
};

struct PlayerTag {
};

kin::SpawnRecipe actor(kin::f32 x, kin::f32 y, std::string name) {
    return [=](kin::EntityBuilder& entity) {
        entity.set(Position{x, y})
            .set(Name{name})
            .add<PlayerTag>();
    };
}

void test_spawn_applies_recipe() {
    kin::EcsWorld world;
    world.component<Position>("Position");
    world.component<Name>("Name");
    world.component<PlayerTag>("PlayerTag");

    kin::EcsEntity player = kin::spawn(world, "player", actor(2.0f, 3.0f, "Ada"));

    assert(player.valid());
    assert(player.alive());
    assert(player.name() == "player");
    assert(player.has<Position>());
    assert(player.has<Name>());
    assert(player.has<PlayerTag>());
    assert(player.get<Position>()->x == 2.0f);
    assert(player.get<Position>()->y == 3.0f);
    assert(player.get<Name>()->value == "Ada");
}

void test_spawn_allows_anonymous_entities() {
    kin::EcsWorld world;
    world.component<Position>("Position");

    kin::EcsEntity entity = kin::spawn(world, [](kin::EntityBuilder& builder) {
        builder.set(Position{4.0f, 5.0f});
    });

    assert(entity.valid());
    assert(entity.alive());
    assert(entity.has<Position>());
    assert(entity.get<Position>()->x == 4.0f);
    assert(entity.get<Position>()->y == 5.0f);
}

} // namespace

int main() {
    test_spawn_applies_recipe();
    test_spawn_allows_anonymous_entities();
    return 0;
}
