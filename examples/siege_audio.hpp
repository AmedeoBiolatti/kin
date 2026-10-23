#pragma once

#include "workloads.hpp"

#include <kin/audio/audio.hpp>

#include <span>
#include <string_view>

namespace examples {

// Signal Siege's sound, synthesized at startup (no audio files) and played
// through kin::AudioEngine: effects for the arena's events, positioned around the
// player, UI cues and a looping ambient drone. Muted, it mixes into a silent
// backend, so headless runs exercise the same path without making noise.
class SiegeAudio {
public:
    // `distance_scale`: world units a pixel of Signal Siege's (where cues fade
    // over a screen's width); Signal Breach, in metres, passes 1/40.
    explicit SiegeAudio(bool muted, float distance_scale = 1.0f);

    // Plays the effects for `events`, heard from `listener` (world units).
    void play_events(std::span<const ArenaEvent> events, Vec2f listener);
    // One of "ui_move", "ui_select", "wave", "win", "lose".
    void play(std::string_view cue);
    // An effect cue ("shot", "enemy_shot", "hit", "kill", "pickup", "dash",
    // "hurt") from a place in 3D, heard from set_listener_3d's listener.
    void play_at(std::string_view cue, Vec3f position);
    void set_listener_3d(const AudioListener3D& listener) { _engine.set_listener_3d(listener); }
    void set_drone(bool playing);
    void update(float dt);

    AudioEngineStats stats() const { return _engine.stats(); }
    const AudioCatalog& catalog() const { return _catalog; }

private:
    AudioEngine _engine;
    AudioCatalog _catalog;
    AudioHandle _drone;
};

} // namespace examples
