#pragma once

#include <kin/audio/audio_clip.hpp>
#include <kin/audio/spatial_audio.hpp>
#include <kin/assets/asset_manager.hpp>
#include <kin/core/types.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kin {

enum class AudioCategory {
    Music,
    Ambient,
    Sound,
    Effect,
    Ui,
};

struct AudioClipRef {
    std::string id;
    std::string path;
    // Keep the file compressed and decode while playing (music, long ambience).
    bool stream = false;
    // Where a looping cue jumps back to, and where it jumps from, in frames at
    // the file's sample rate; loop_end 0 is the end of the file. An intro
    // plays once before the loop.
    i64 loop_start = 0;
    i64 loop_end = 0;
};

struct AudioCue {
    std::string id;
    std::vector<std::string> clips;
    AudioCategory category = AudioCategory::Sound;
    std::string bus = "sfx";
    f32 volume = 1.0f;
    f32 pitch = 1.0f;
    f32 pitch_variance = 0.0f;
    i32 priority = 0;
    i32 max_instances = 0;
    bool loop = false;
    bool spatial = false;
    f32 min_distance = 32.0f;
    f32 max_distance = 512.0f;
    AudioRolloff rolloff = AudioRolloff::Smooth;
    f32 rolloff_power = 1.0f;
    f32 pan_strength = 1.0f;
};

// Turns `bus` down to `volume` while anything plays on `when` (or a bus under
// it), e.g. music ducking under dialogue. `attack` and `release` are the
// seconds it takes to go down and come back.
struct AudioDuck {
    std::string bus;
    std::string when;
    f32 volume = 0.5f;
    f32 attack = 0.1f;
    f32 release = 0.5f;

    friend bool operator==(const AudioDuck&, const AudioDuck&) = default;
};

enum class AudioEffectType {
    LowPass,
    HighPass,
    Reverb,
    Compressor,
    BandPass,
    Notch,
    Peak,      // an EQ band: boosts or cuts around `cutoff` by `gain` dB
    LowShelf,  // boosts or cuts below `cutoff` by `gain` dB
    HighShelf, // boosts or cuts above `cutoff` by `gain` dB
    Delay,
};

// One effect in a bus's chain. Only the fields for its type matter.
struct AudioEffect {
    AudioEffectType type = AudioEffectType::LowPass;
    bool enabled = true;
    // Filters and EQ: the frequency in Hz (corner, centre or shelf) and the
    // resonance or bandwidth; Peak and the shelves boost or cut by `gain` dB.
    f32 cutoff = 1000.0f;
    f32 q = 0.7071f;
    f32 gain = 0.0f;
    // Delay: seconds between echoes and how much of each feeds the next (with
    // the reverb's `wet` and `dry` levels).
    f32 time = 0.25f;
    f32 feedback = 0.4f;
    // Reverb (Freeverb): room size and damping 0..1, wet and dry levels, and
    // stereo width 0..1.
    f32 room_size = 0.5f;
    f32 damping = 0.5f;
    f32 wet = 0.3f;
    f32 dry = 1.0f;
    f32 width = 1.0f;
    // Compressor: above `threshold` dB, the level rises 1/ratio as fast;
    // attack and release in seconds, makeup gain in dB.
    f32 threshold = -12.0f;
    f32 ratio = 4.0f;
    f32 attack = 0.01f;
    f32 release = 0.1f;
    f32 makeup = 0.0f;

    friend bool operator==(const AudioEffect&, const AudioEffect&) = default;
};

struct AudioBus {
    std::string id;
    std::string parent;
    f32 volume = 1.0f;
    bool muted = false;
    bool paused = false;
    f32 fade_target = 1.0f;
    f32 fade_seconds = 0.0f;
    // Applied in order to everything the bus plays, before its volume.
    std::vector<AudioEffect> effects;
};

class AudioCatalog {
public:
    void clear();
    void set_root(std::filesystem::path root);
    const std::filesystem::path& root() const { return _root; }

    void add_clip(AudioClipRef clip);
    void add_bus(AudioBus bus);
    void add_cue(AudioCue cue);
    void add_duck(AudioDuck duck);

    const AudioClipRef* clip(std::string_view id) const;
    const AudioBus* bus(std::string_view id) const;
    AudioBus* bus(std::string_view id);
    const AudioCue* cue(std::string_view id) const;

    std::filesystem::path resolve_clip_path(std::string_view clip_id) const;
    f32 effective_bus_volume(std::string_view bus_id) const;

    const std::unordered_map<std::string, AudioClipRef>& clips() const { return _clips; }
    const std::unordered_map<std::string, AudioBus>& buses() const { return _buses; }
    const std::unordered_map<std::string, AudioCue>& cues() const { return _cues; }
    const std::vector<AudioDuck>& ducks() const { return _ducks; }

private:
    std::filesystem::path _root;
    std::unordered_map<std::string, AudioClipRef> _clips;
    std::unordered_map<std::string, AudioBus> _buses;
    std::unordered_map<std::string, AudioCue> _cues;
    std::vector<AudioDuck> _ducks;
};

std::string_view audio_category_name(AudioCategory category);
std::string_view audio_effect_type_name(AudioEffectType type);
bool parse_audio_effect_type(std::string_view value, AudioEffectType& out);
bool parse_audio_category(std::string_view value, AudioCategory& out);
std::string_view audio_rolloff_name(AudioRolloff rolloff);
bool parse_audio_rolloff(std::string_view value, AudioRolloff& out);

AudioCatalog load_audio_catalog(const std::filesystem::path& path);
bool save_audio_catalog(const AudioCatalog& catalog, const std::filesystem::path& path);
void register_audio_catalog_loader(AssetManager& assets);

} // namespace kin
