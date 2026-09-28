#pragma once

#include <kin/anim/events.hpp>
#include <kin/ecs/world.hpp>
#include <kin/particles/particle_system.hpp>

#include <string_view>

namespace kin {

bool dispatch_particle_animation_event(const AnimationEvent& event,
                                       Vec2f anchor,
                                       ParticleSystem& particles,
                                       std::string_view event_name = "particle");
EventInterpreter make_particle_event_interpreter(ParticleSystem& particles,
                                                 std::string_view event_name = "particle");
void dispatch_particle_animation_events(EcsWorld& world,
                                        ParticleSystem& particles,
                                        std::string_view event_name = "particle");

} // namespace kin
