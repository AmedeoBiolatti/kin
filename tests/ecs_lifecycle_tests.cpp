#include <kin/ecs/lifecycle.hpp>
#include <kin/ecs/spawn.hpp>

#include <cassert>

namespace {

struct Position {
    kin::f32 x = 0.0f;
};

struct EffectTag {
};

void test_count_with_counts_tagged_entities() {
    kin::EcsWorld world;
    world.component<kin::TransientEntity>("TransientEntity");
    world.component<Position>("Position");

    kin::spawn(world, [](kin::EntityBuilder& entity) {
        entity.add<kin::TransientEntity>().set(Position{1.0f});
    });
    kin::spawn(world, [](kin::EntityBuilder& entity) {
        entity.add<kin::TransientEntity>().set(Position{2.0f});
    });
    world.entity("persistent").set(Position{3.0f});

    assert(kin::count_with<kin::TransientEntity>(world) == 2);
    assert(world.count<Position>() == 3);
}

void test_destroy_with_removes_only_tagged_entities() {
    kin::EcsWorld world;
    world.component<EffectTag>("EffectTag");
    world.component<Position>("Position");

    kin::EcsEntity first = kin::spawn(world, [](kin::EntityBuilder& entity) {
        entity.add<EffectTag>().set(Position{1.0f});
    });
    kin::EcsEntity second = kin::spawn(world, [](kin::EntityBuilder& entity) {
        entity.add<EffectTag>().set(Position{2.0f});
    });
    kin::EcsEntity persistent = world.entity("persistent").set(Position{3.0f});

    const kin::i32 destroyed = kin::destroy_with<EffectTag>(world);

    assert(destroyed == 2);
    assert(!first.alive());
    assert(!second.alive());
    assert(persistent.alive());
    assert(kin::count_with<EffectTag>(world) == 0);
    assert(world.count<Position>() == 1);
}

} // namespace

int main() {
    test_count_with_counts_tagged_entities();
    test_destroy_with_removes_only_tagged_entities();
    return 0;
}
