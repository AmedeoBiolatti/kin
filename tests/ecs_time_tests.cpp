#include <kin/ecs/time.hpp>

#include <cassert>

namespace {

void test_one_shot_timer_finishes_once() {
    kin::EcsWorld world;
    world.component<kin::Timer>("Timer");
    kin::EcsEntity entity = world.entity("timer").set(kin::Timer{.duration = 0.5f});

    kin::update_timers(world, 0.25f);
    const kin::Timer* halfway = entity.get<kin::Timer>();
    assert(halfway != nullptr);
    assert(!halfway->finished);
    assert(halfway->ticks == 0);

    kin::update_timers(world, 0.25f);
    const kin::Timer* finished = entity.get<kin::Timer>();
    assert(finished != nullptr);
    assert(finished->finished);
    assert(finished->ticks == 1);

    kin::update_timers(world, 1.0f);
    const kin::Timer* still_finished = entity.get<kin::Timer>();
    assert(still_finished != nullptr);
    assert(still_finished->finished);
    assert(still_finished->ticks == 1);
}

void test_repeating_timer_preserves_remainder() {
    kin::EcsWorld world;
    world.component<kin::Timer>("Timer");
    kin::EcsEntity entity = world.entity("timer").set(kin::Timer{
        .duration = 0.25f,
        .repeating = true,
    });

    kin::update_timers(world, 0.6f);
    const kin::Timer* timer = entity.get<kin::Timer>();
    assert(timer != nullptr);
    assert(timer->finished);
    assert(timer->ticks == 2);
    assert(timer->elapsed > 0.09f && timer->elapsed < 0.11f);

    kin::update_timers(world, 0.05f);
    timer = entity.get<kin::Timer>();
    assert(timer != nullptr);
    assert(!timer->finished);
    assert(timer->ticks == 2);
}

void test_restart_timer_clears_state() {
    kin::Timer timer{.duration = 1.0f, .elapsed = 1.0f, .finished = true, .ticks = 3};
    kin::restart_timer(timer);

    assert(timer.elapsed == 0.0f);
    assert(!timer.finished);
    assert(timer.ticks == 0);
}

void test_lifetime_destroys_expired_entities() {
    kin::EcsWorld world;
    world.component<kin::Lifetime>("Lifetime");
    kin::EcsEntity short_lived = world.entity("short").set(kin::Lifetime{.remaining = 0.5f});
    kin::EcsEntity long_lived = world.entity("long").set(kin::Lifetime{.remaining = 2.0f});

    kin::update_lifetimes(world, 0.5f);

    assert(!short_lived.alive());
    assert(long_lived.alive());
    assert(world.count<kin::Lifetime>() == 1);
}

} // namespace

int main() {
    test_one_shot_timer_finishes_once();
    test_repeating_timer_preserves_remainder();
    test_restart_timer_clears_state();
    test_lifetime_destroys_expired_entities();
    return 0;
}
