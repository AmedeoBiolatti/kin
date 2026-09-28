#include <kin/anim/events.hpp>
#include <kin/anim/player.hpp>

#include <utility>

namespace kin {

void AnimationEventDispatch::on(std::string channel, EventInterpreter fn) {
    _channels[std::move(channel)] = std::move(fn);
}

void AnimationEventDispatch::run(EcsWorld& world) {
    world.raw().each([this](flecs::entity entity, AnimationEventQueue& queue) {
        std::vector<AnimationEvent> unknown;
        unknown.reserve(queue.pending.size());

        for (const AnimationEvent& event : queue.pending) {
            const auto found = _channels.find(event.channel);
            if (found == _channels.end()) {
                unknown.push_back(event);
                continue;
            }
            EcsEntity target{entity};
            if (!event.target.empty()) {
                auto* player = entity.get_mut<AnimationPlayer>();
                if (!player) {
                    continue;
                }
                target = resolve_animation_target(EcsEntity{entity}, *player, event.target);
                if (!target) {
                    continue;
                }
            }
            found->second(target, event);
        }

        queue.pending = std::move(unknown);
    });
}

EventInterpreter make_anim_event_interpreter() {
    return [](EcsEntity target, const AnimationEvent& event) {
        auto* player = target.get_mut<AnimationPlayer>();
        if (!player || event.value.empty()) {
            return;
        }
        push(*player, event.value);
    };
}

} // namespace kin
