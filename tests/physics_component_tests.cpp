// Links only kin::physics: no SDL, renderer, engine base or ECS.
#include <kin/components.hpp>
#include <kin/physics/physics.hpp>
#include <cassert>

int main() {
    static_assert(kin::components::physics);
    kin::PhysicsWorld world;
    auto body = world.create_body({.type = kin::PhysicsBodyType::Dynamic});
    auto fixture = world.add_fixture(body, {.shape = kin::PhysicsShape::circle(0.25f), .density = 1.0f});
    assert(world.is_alive(body));
    assert(world.is_alive(fixture));
    world.step(1.0f / 60.0f);
    world.destroy_body(body);
    assert(!world.is_alive(body));
    assert(!world.is_alive(fixture));
}
