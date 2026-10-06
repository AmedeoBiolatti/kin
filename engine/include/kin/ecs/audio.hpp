#pragma once

#include <kin/anim/events.hpp>
#include <kin/audio/audio_engine.hpp>
#include <kin/ecs/render.hpp>
#include <kin/ecs/world.hpp>

#include <string>

namespace kin {

// What an AudioEmitter does once its sound has finished.
enum class AudioEmitterEnd {
    Restart, // play it again (a looping cue never finishes)
    Keep,    // stay, silent, with playing = false
    Remove,  // remove the AudioEmitter
    Despawn, // destroy the entity: a sound fired and forgotten
};

// A sound that follows its entity's Transform2D. update_audio_emitters starts
// it (when auto_start), keeps it positioned, applies volume, pitch and paused
// each frame, and stops it when the component or the entity goes away.
struct AudioEmitter {
    std::string cue;
    bool playing = false; // written by update_audio_emitters
    bool auto_start = true;
    f32 volume = 1.0f; // on top of the cue's
    f32 pitch = 1.0f;
    bool paused = false;
    f32 fade_in = 0.0f;
    AudioEmitterEnd when_done = AudioEmitterEnd::Restart;
    AudioHandle handle{};
};

struct AudioListener {
    i32 priority = 0;
};

// Plays once from the entity's position, then is removed.
struct AudioOneShot {
    std::string cue;
    Vec2f offset{};
    i32 priority_boost = 0;
    f32 volume = 1.0f;
    f32 pitch = 1.0f;
};

void update_audio_listeners(EcsWorld& world, AudioEngine& audio);
void update_audio_emitters(EcsWorld& world, AudioEngine& audio, const AudioCatalog& catalog);
void consume_audio_one_shots(EcsWorld& world, AudioEngine& audio, const AudioCatalog& catalog);
EventInterpreter make_audio_event_interpreter(AudioEngine& audio, const AudioCatalog& catalog);
void play_animation_audio_events(EcsWorld& world, AudioEngine& audio, const AudioCatalog& catalog);

} // namespace kin
