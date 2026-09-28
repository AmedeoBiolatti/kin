#pragma once

#include <kin/core/types.hpp>
#include <kin/ecs/world.hpp>

namespace kin {

struct Timer {
    f32 duration = 0.0f;
    f32 elapsed = 0.0f;
    bool repeating = false;
    bool finished = false;
    i32 ticks = 0;
};

struct Lifetime {
    f32 remaining = 0.0f;
};

void restart_timer(Timer& timer);
void update_timers(EcsWorld& world, f32 dt);
void update_timers(flecs::world& world, f32 dt);
void update_lifetimes(EcsWorld& world, f32 dt);
void update_lifetimes(flecs::world& world, f32 dt);

} // namespace kin
