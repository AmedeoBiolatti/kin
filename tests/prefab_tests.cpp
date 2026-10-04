#include <kin/ecs/render.hpp>
#include <kin/prefab/prefab_registry.hpp>

#include <cassert>

namespace {

struct Marker {
    int value = 0;
};

struct Health {
    int hp = 0;
};

// flecs 4.0 keeps one id per C++ component type for the whole process: worlds
// that register the same types in different orders hand one type's id to
// another. Each test's world registers them in this order first.
kin::EcsWorld make_world() {
    kin::EcsWorld world;
    world.raw().component<Marker>();
    world.raw().component<Health>();
    world.raw().component<kin::Transform2D>();
    return world;
}

void test_spawns_registered_factory_prefab() {
    kin::EcsWorld world = make_world();
    kin::PrefabRegistry prefabs;

    assert(prefabs.add("marker", [](kin::EcsWorld& w, const kin::PrefabSpawn& spawn) {
        return w.entity()
            .set(Marker{42})
            .set(kin::Transform2D{.pos = spawn.pos});
    }));

    kin::EcsEntity entity = prefabs.spawn(world, "marker", {.pos = {4.0f, 8.0f}});
    assert(entity);
    assert(entity.has<Marker>());
    assert(entity.get<Marker>()->value == 42);
    assert(entity.has<kin::Transform2D>());
    assert((entity.get<kin::Transform2D>()->pos == kin::Vec2f{4.0f, 8.0f}));
}

void test_recipe_prefab_uses_spawn_name_and_parent() {
    kin::EcsWorld world = make_world();
    kin::PrefabRegistry prefabs;
    kin::EcsEntity parent = world.entity("parent");

    assert(prefabs.add_recipe("enemy", [](kin::EntityBuilder& builder, const kin::PrefabSpawn& spawn) {
        builder.set(Marker{7})
            .set(Health{100})
            .set(kin::Transform2D{.pos = spawn.pos});
    }));

    kin::EcsEntity entity = prefabs.spawn(world,
                                         "enemy",
                                         {
                                             .pos = {10.0f, 20.0f},
                                             .name = "enemy-a",
                                             .parent = parent,
                                         });

    assert(entity);
    assert(entity.name() == "enemy-a");
    assert(entity.parent().id() == parent.id());
    assert(entity.has<Marker>());
    assert(entity.has<Health>());
}

void test_missing_remove_and_names() {
    kin::EcsWorld world = make_world();
    kin::PrefabRegistry prefabs;
    assert(!prefabs.spawn(world, "missing"));
    assert(prefabs.names().empty());

    assert(prefabs.add("one", [](kin::EcsWorld& w, const kin::PrefabSpawn&) {
        return w.entity().set(Marker{1});
    }));
    assert(prefabs.has("one"));
    assert(prefabs.names().size() == 1);
    assert(prefabs.remove("one"));
    assert(!prefabs.has("one"));
    assert(!prefabs.remove("one"));
}

} // namespace

void run_prefab_data_tests();

int main() {
    test_spawns_registered_factory_prefab();
    test_recipe_prefab_uses_spawn_name_and_parent();
    test_missing_remove_and_names();
    run_prefab_data_tests();
    return 0;
}
