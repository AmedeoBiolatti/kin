#pragma once

// Summit's sound (docs/3d_plan.md, P4b). Two halves:
//
//  - `CueGenerator` turns the game's events and state into a list of `SoundCue`s, once a step: plain data (what, where,
//  how loud,
//    which pitch), so that a test can count the grabs of a scripted run or check that a fall made a whoosh and an
//    impact without a sound card. The curves it uses (`wind_gain`, `footstep_cadence`, ...) are free functions for the
//    same reason.
//  - `SummitAudio` plays them through kin::AudioEngine, with clips synthesised at start-up (no files: wind, footsteps
//  by surface,
//    grabs, breath, falls, rope, fire, stings), positional where it makes sense (the engine's 3D listener), and three
//    volumes (master, effects, ambience). Muted, or with no device, it mixes into the engine's null backend: same code,
//    no sound.
//
// Presentation only: it reads a `Game` and never writes it.

#include "game.hpp"

#include <array>
#include <kin/audio/audio.hpp>
#include <string>
#include <vector>

namespace summit {
using namespace kin;

enum class CueKind : u8 {
    Footstep,
    Jump,
    Land, // a landing with no damage
    Grab,
    Release,
    Impact, // damage from a fall: a thump by how much
    FallStart,
    Breath, // effort breathing at low stamina
    Exhausted,
    RopeCreak,
    Hurt,
    Died,
    GotUp,
    Camp,
    Summit,
    Count,
};
inline constexpr std::size_t cue_kind_count = static_cast<std::size_t>(CueKind::Count);
const char* cue_name(CueKind k);

enum class SurfaceSound : u8 { Grass, Rock, Snow };

struct SoundCue {
    u64 step = 0;
    CueKind kind = CueKind::Footstep;
    Vec3f position{};
    f32 gain = 1.0f;
    f32 pitch = 1.0f;
    i32 variant = 0; // a footstep's SurfaceSound; a grab's hand (0 left, 1 right)
    friend bool operator==(const SoundCue&, const SoundCue&) = default;
};

// ---- the curves (pure)
// ------------------------------------------------------------------------------------------------------

// The wind's loudness (0 to 1) at an altitude of a mountain `height` high for a wind speed in m/s: it rises with height
// and with the wind's strength.
f32 wind_gain(f32 altitude, f32 height, f32 wind_speed);
f32 wind_pitch(f32 wind_speed);
// The whoosh of a fall by its speed down (m/s): none under walking speed, full by 20.
f32 fall_whoosh_gain(f32 speed_down);
f32 fall_whoosh_pitch(f32 speed_down);
// Footfalls a second at a ground speed (0 under 0.5 m/s, rising with speed).
f32 footstep_cadence(f32 ground_speed);
// Seconds between breaths for the share of the usable stamina left (0: no breathing: not tired).
f32 breath_interval(f32 usable_fraction, bool exhausted);
// A thump's loudness for the health lost.
f32 impact_gain(f32 damage);
// What the ground under the player sounds like.
SurfaceSound surface_under(const Game& game, i32 player);

// Where a camp's fire is (for the crackle and the lights): beside the camp's marker.
Vec3f camp_fire_position(const Camp& camp);

// ---- the cues
// ---------------------------------------------------------------------------------------------------------------

class CueGenerator {
public:
    // Call once a step after Game::step(): appends this step's cues to `out`.
    void observe(const Game& game, i32 player, std::vector<SoundCue>& out);
    void reset() { *this = {}; }

private:
    f32 _foot_phase = 0.0f;
    f32 _breath_timer = -1.0f; // negative: not tired
    f32 _rope_timer = 0.0f;
    bool _was_on_ground = true;
};

// ---- playing them
// -----------------------------------------------------------------------------------------------------------

struct AudioVolumes {
    f32 master = 0.8f;
    f32 sfx = 1.0f;
    f32 ambience = 0.7f;
};

class SummitAudio {
public:
    // `muted`: the engine's null backend (a silent mix, the same code path). A machine with no device falls back to it
    // too.
    explicit SummitAudio(bool muted);
    ~SummitAudio();

    void set_volumes(const AudioVolumes& v);
    const AudioVolumes& volumes() const { return _volumes; }
    // Plays one cue now (at its position for 3D ones).
    void play(const SoundCue& cue);
    // One of "ui_move", "ui_select".
    void play_ui(const char* name);
    // Once a frame: the listener (the camera), the loops that follow the game (wind, whoosh, fires), and the engine's
    // clock. `paused`: the menu is up (the effects hold, the ambience goes quiet).
    void update(const Game& game, i32 player, const AudioListener3D& listener, f32 dt, bool paused = false);

    // How many of a kind were played (a test and the report).
    i32 played(CueKind k) const { return _played[std::size_t(k)]; }
    i32 played_total() const;
    AudioEngineStats stats() const { return _engine.stats(); }
    // The loops' current volumes: what the curves gave this frame (before the buses).
    f32 wind_level() const { return _wind_level; }
    f32 whoosh_level() const { return _whoosh_level; }
    bool device_available() const { return _engine.device_available(); }
    // Mixes `frames` of the output into `out` (offline: tests look at it is not silent).
    AudioEngine& engine() { return _engine; }

private:
    AudioEngine _engine;
    AudioCatalog _catalog;
    AudioVolumes _volumes{};
    AudioHandle _wind, _whoosh;
    std::vector<AudioHandle> _fires;
    std::array<i32, cue_kind_count> _played{};
    f32 _wind_level = 0.0f, _whoosh_level = 0.0f;
    bool _paused = false;
};

} // namespace summit
