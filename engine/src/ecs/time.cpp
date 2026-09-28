#include <kin/ecs/time.hpp>

#include <algorithm>
#include <vector>

namespace kin {
namespace {

constexpr f32 min_timer_duration = 0.001f;

f32 clamped_dt(f32 dt) {
    return std::max(0.0f, dt);
}

f32 timer_duration(const Timer& timer) {
    return std::max(timer.duration, min_timer_duration);
}

} // namespace

void restart_timer(Timer& timer) {
    timer.elapsed = 0.0f;
    timer.finished = false;
    timer.ticks = 0;
}

void update_timers(EcsWorld& world, f32 dt) {
    update_timers(world.raw(), dt);
}

void update_timers(flecs::world& world, f32 dt) {
    const f32 step = clamped_dt(dt);
    world.each([step](Timer& timer) {
        if (timer.finished && !timer.repeating) {
            return;
        }

        timer.finished = false;
        timer.elapsed += step;

        const f32 duration = timer_duration(timer);
        while (timer.elapsed >= duration) {
            ++timer.ticks;
            timer.finished = true;

            if (!timer.repeating) {
                timer.elapsed = duration;
                break;
            }

            timer.elapsed -= duration;
        }
    });
}

void update_lifetimes(EcsWorld& world, f32 dt) {
    update_lifetimes(world.raw(), dt);
}

void update_lifetimes(flecs::world& world, f32 dt) {
    const f32 step = clamped_dt(dt);
    std::vector<flecs::entity> expired;

    world.each([step, &expired](flecs::entity entity, Lifetime& lifetime) {
        lifetime.remaining -= step;
        if (lifetime.remaining <= 0.0f) {
            expired.push_back(entity);
        }
    });

    for (flecs::entity entity : expired) {
        if (entity.is_alive()) {
            entity.destruct();
        }
    }
}

} // namespace kin
