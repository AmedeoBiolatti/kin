#pragma once

#include <kin/anim/track.hpp>
#include <kin/core/types.hpp>
#include <kin/ecs/world.hpp>

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace kin {

struct AnimationEvent {
    std::string channel;
    std::string name;
    std::string value;
    std::string target;
    Vec2f offset{};
};

struct EventKey {
    f32 time = 0.0f;
    AnimationEvent event;
};

struct EventTrack {
    std::string target;
    std::vector<EventKey> keys;
};

// Per-entity queue that `advance_animation_players` appends emitted events to.
//
// Ownership/draining contract: `AnimationEventDispatch::run` removes every event
// whose channel has a registered interpreter, but intentionally *retains* events
// on channels with no interpreter so game code can consume its own channels.
// Those retained events are NOT auto-expired: a channel that is emitted every
// frame (e.g. a looping clip) but never given an interpreter and never drained by
// the game will grow `pending` without bound. For any custom channel either
// register an interpreter via `AnimationEventDispatch::on`, or drain `pending`
// from a game system each frame.
struct AnimationEventQueue {
    std::vector<AnimationEvent> pending;
};

using EventInterpreter = std::function<void(EcsEntity, const AnimationEvent&)>;

class AnimationEventDispatch {
public:
    void on(std::string channel, EventInterpreter fn);
    // Consumes events on registered channels (removing them from each
    // AnimationEventQueue) and leaves events on unregistered channels in place —
    // see the AnimationEventQueue draining contract above.
    void run(EcsWorld& world);

private:
    std::unordered_map<std::string, EventInterpreter> _channels;
};

EventInterpreter make_anim_event_interpreter();

} // namespace kin
