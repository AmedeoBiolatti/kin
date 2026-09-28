#pragma once

#include <kin/anim/events.hpp>
#include <kin/audio/audio_engine.hpp>
#include <kin/ecs/render.hpp>
#include <kin/ecs/world.hpp>

#include <string>

namespace kin {

struct AudioEmitter {
    std::string cue;
    bool playing = false;
    bool auto_start = true;
    AudioHandle handle{};
};

struct AudioListener {
    i32 priority = 0;
};

struct AudioOneShot {
    std::string cue;
    Vec2f offset{};
    i32 priority_boost = 0;
};

void update_audio_listeners(EcsWorld& world, AudioEngine& audio);
void update_audio_emitters(EcsWorld& world, AudioEngine& audio, const AudioCatalog& catalog);
void consume_audio_one_shots(EcsWorld& world, AudioEngine& audio, const AudioCatalog& catalog);
EventInterpreter make_audio_event_interpreter(AudioEngine& audio, const AudioCatalog& catalog);
void play_animation_audio_events(EcsWorld& world, AudioEngine& audio, const AudioCatalog& catalog);

} // namespace kin
