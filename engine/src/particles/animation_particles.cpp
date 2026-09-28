#include <kin/particles/animation_particles.hpp>

#include <kin/ecs/render.hpp>

#include <string>

namespace kin {

bool dispatch_particle_animation_event(const AnimationEvent& event,
                                       Vec2f anchor,
                                       ParticleSystem& particles,
                                       std::string_view event_name) {
    if (event.channel != event_name) {
        return false;
    }
    return particles.emit(event.value, anchor, event.offset);
}

EventInterpreter make_particle_event_interpreter(ParticleSystem& particles,
                                                 std::string_view event_name) {
    return [&particles, event_name = std::string{event_name}](EcsEntity entity, const AnimationEvent& event) {
        dispatch_particle_animation_event(event, world_position(entity), particles, event_name);
    };
}

void dispatch_particle_animation_events(EcsWorld& world,
                                        ParticleSystem& particles,
                                        std::string_view event_name) {
    AnimationEventDispatch dispatch;
    dispatch.on(std::string{event_name}, make_particle_event_interpreter(particles, event_name));
    dispatch.run(world);
}

} // namespace kin
