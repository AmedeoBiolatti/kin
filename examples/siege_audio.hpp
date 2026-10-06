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
    explicit SiegeAudio(bool muted);

    // Plays the effects for `events`, heard from `listener` (world units).
    void play_events(std::span<const ArenaEvent> events, Vec2f listener);
    // One of "ui_move", "ui_select", "wave", "win", "lose".
    void play(std::string_view cue);
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
