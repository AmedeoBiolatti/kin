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

// A sound that follows its entity's Transform2D, or in 3D its WorldTransform3D
// (which wins where an entity has both). update_audio_emitters starts it (when
// auto_start), keeps it positioned, applies volume, pitch and paused each frame,
// and stops it when the component or the entity goes away.
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
    // In 3D: where the sound is in the entity's own space (an engine's exhaust), moved and turned with it.
    Vec3f offset_3d{};
};

// The ears: update_audio_listeners puts the listener at the highest priority
// one's Transform2D, or in 3D at its WorldTransform3D, facing its -Z with its +Y
// up (a camera's, or a head's). With no listener entity, a world with an enabled
// SceneCamera3D hears from the camera a SceneSession3D views it through.
struct AudioListener {
    i32 priority = 0;
};

// Plays once from the entity's position (Transform2D plus `offset`, or in 3D its
// WorldTransform3D at `offset_3d`, in the entity's own space), then is removed.
struct AudioOneShot {
    std::string cue;
    Vec2f offset{};
    i32 priority_boost = 0;
    f32 volume = 1.0f;
    f32 pitch = 1.0f;
    Vec3f offset_3d{};
};

void update_audio_listeners(EcsWorld& world, AudioEngine& audio);
void update_audio_emitters(EcsWorld& world, AudioEngine& audio, const AudioCatalog& catalog);
void consume_audio_one_shots(EcsWorld& world, AudioEngine& audio, const AudioCatalog& catalog);
EventInterpreter make_audio_event_interpreter(AudioEngine& audio, const AudioCatalog& catalog);
void play_animation_audio_events(EcsWorld& world, AudioEngine& audio, const AudioCatalog& catalog);

} // namespace kin
