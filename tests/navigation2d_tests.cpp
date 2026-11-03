#include <cassert>
#include <cmath>
#include <kin/ecs/lifecycle.hpp>
#include <kin/ecs/render.hpp>
#include <kin/pathfinding/navigation2d.hpp>
#include <limits>

using namespace kin;
namespace {
std::shared_ptr<NavGridSnapshot> grid() {
    auto g = std::make_shared<NavGridSnapshot>();
    g->cols = 12;
    g->rows = 10;
    g->tile_size = 10;
    g->version = 1;
    g->blocked.assign(120, 0);
    for (int y = 0; y < 10; ++y)
        if (y != 6)
            g->blocked[y * 12 + 6] = 1;
    return g;
}
void test_follow_and_replan() {
    kin::EcsWorld world;
    auto g = grid();
    Navigation2D navigation{world, g};
    auto e = world.entities().create();
    e.set(Transform2D{.pos = {25, 25}});
    e.set(NavigationAgent2D{
        .target = {94, 25}, .speed = 35, .radius = 1, .arrival_distance = .2f, .waypoint_distance = .05f});
    bool detour = false;
    int arrivals = 0;
    for (int i = 0; i < 500; ++i) {
        navigation.update(.02f);
        auto p = e.get<Transform2D>()->pos;
        assert(!g->is_blocked(g->world_to_tile(p)));
        detour |= p.y > 60;
        for (const auto& event : navigation.events())
            arrivals += event.kind == NavigationEventKind2D::Arrived;
    }
    assert(detour && arrivals == 1 && e.get<NavigationAgent2D>()->arrived);
    assert(std::abs(e.get<Transform2D>()->pos.x - 94) < .21f);
    e.get_mut<NavigationAgent2D>()->target = {95, 85};
    navigation.update(.02f);
    assert(!e.get<NavigationAgent2D>()->arrived);
    for (int i = 0; i < 200; ++i)
        navigation.update(.02f);
    assert(e.get<NavigationAgent2D>()->arrived);
    e.get_mut<NavigationAgent2D>()->target = {65, 25};
    navigation.update(.02f);
    assert(e.get<NavigationAgent2D>()->unreachable);
    e.get_mut<NavigationAgent2D>()->target = {25, 25};
    e.get_mut<NavigationAgent2D>()->radius = 6; // cannot fit through the one-cell gap
    navigation.update(.02f);
    navigation.update(.02f);
    assert(e.get<NavigationAgent2D>()->unreachable);
}
void test_stuck_and_lifetime() {
    kin::EcsWorld world;
    Navigation2D navigation{world, grid()};
    auto e = world.entities().create();
    e.set(Transform2D{.pos = {25, 25}});
    e.set(NavigationAgent2D{.target = {45, 25},
                            .speed = 10,
                            .radius = 1,
                            .arrival_distance = .1f,
                            .waypoint_distance = .05f,
                            .stuck_seconds = .1f,
                            .apply_motion = false});
    int stuck = 0;
    for (int i = 0; i < 20; ++i) {
        navigation.update(.02f);
        for (const auto& event : navigation.events())
            stuck += event.kind == NavigationEventKind2D::Stuck;
    }
    assert(stuck == 1 && e.get<NavigationAgent2D>()->stuck && e.get<Transform2D>()->pos.x == 25);
    e.set(EntityLifetime{99});
    e.get_mut<NavigationAgent2D>()->target = {45, 45};
    navigation.update(.02f);
    assert(!e.get<NavigationAgent2D>()->stuck);
    e.get_mut<NavigationAgent2D>()->radius = std::numeric_limits<float>::quiet_NaN();
    navigation.update(.02f);
    assert(e.get<NavigationAgent2D>()->unreachable && e.get<NavigationAgent2D>()->velocity.x == 0);
}
} // namespace
void test_avoidance() {
    kin::EcsWorld world;
    auto g = grid();
    std::fill(g->blocked.begin(), g->blocked.end(), 0);
    Navigation2D navigation{world, g};
    auto left = world.entities().create(), right = world.entities().create();
    left.set(Transform2D{.pos = {25, 45}});
    right.set(Transform2D{.pos = {65, 45}});
    left.set(NavigationAgent2D{.target = {65, 45},
                               .speed = 15,
                               .radius = 2,
                               .arrival_distance = .2f,
                               .waypoint_distance = .1f,
                               .neighbour_distance = 30});
    right.set(NavigationAgent2D{.target = {25, 45},
                                .speed = 15,
                                .radius = 2,
                                .arrival_distance = .2f,
                                .waypoint_distance = .1f,
                                .neighbour_distance = 30});
    f32 separation = 40;
    bool sidestepped = false;
    for (int i = 0; i < 500; ++i) {
        navigation.update(.02f);
        const auto a = left.get<Transform2D>()->pos, b = right.get<Transform2D>()->pos;
        separation = std::min(separation, std::hypot(a.x - b.x, a.y - b.y));
        sidestepped |= std::abs(a.y - 45) > 1;
    }
    assert(sidestepped && separation > 4);
    assert(left.get<NavigationAgent2D>()->arrived && right.get<NavigationAgent2D>()->arrived);
}
void test_moving_target() {
    kin::EcsWorld world;
    auto g = grid();
    std::fill(g->blocked.begin(), g->blocked.end(), 0);
    Navigation2D navigation{world, g};
    auto actor = world.entities().create();
    actor.set(Transform2D{.pos = {15, 45}});
    actor.set(NavigationAgent2D{
        .target = {75, 45}, .speed = 15, .radius = 1, .arrival_distance = .1f, .waypoint_distance = .05f});
    for (int i = 0; i < 100; ++i) {
        actor.get_mut<NavigationAgent2D>()->target.x += .0005f;
        navigation.update(.02f);
    }
    assert(actor.get<Transform2D>()->pos.x > 40); // a moving goal cannot starve path delivery
    auto* agent = actor.get_mut<NavigationAgent2D>();
    agent->target = {45.0005f, 45};
    agent->arrival_distance = .0001f;
    agent->waypoint_distance = .1f;
    for (int i = 0; i < 300 && !actor.get<NavigationAgent2D>()->arrived; ++i)
        navigation.update(.02f);
    assert(actor.get<NavigationAgent2D>()->arrived && std::abs(actor.get<Transform2D>()->pos.x - 45.0005f) < .0002f);
}
int main() {
    test_follow_and_replan();
    test_stuck_and_lifetime();
    test_avoidance();
    test_moving_target();
}
