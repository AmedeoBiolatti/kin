#pragma once

#include <kin/audio/audio_clip.hpp>
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
};

struct AudioBus {
    std::string id;
    std::string parent;
    f32 volume = 1.0f;
    bool muted = false;
    bool paused = false;
    f32 fade_target = 1.0f;
    f32 fade_seconds = 0.0f;
};

class AudioCatalog {
public:
    void clear();
    void set_root(std::filesystem::path root);
    const std::filesystem::path& root() const { return _root; }

    void add_clip(AudioClipRef clip);
    void add_bus(AudioBus bus);
    void add_cue(AudioCue cue);

    const AudioClipRef* clip(std::string_view id) const;
    const AudioBus* bus(std::string_view id) const;
    AudioBus* bus(std::string_view id);
    const AudioCue* cue(std::string_view id) const;

    std::filesystem::path resolve_clip_path(std::string_view clip_id) const;
    f32 effective_bus_volume(std::string_view bus_id) const;

    const std::unordered_map<std::string, AudioClipRef>& clips() const { return _clips; }
    const std::unordered_map<std::string, AudioBus>& buses() const { return _buses; }
    const std::unordered_map<std::string, AudioCue>& cues() const { return _cues; }

private:
    std::filesystem::path _root;
    std::unordered_map<std::string, AudioClipRef> _clips;
    std::unordered_map<std::string, AudioBus> _buses;
    std::unordered_map<std::string, AudioCue> _cues;
};

std::string_view audio_category_name(AudioCategory category);
bool parse_audio_category(std::string_view value, AudioCategory& out);

AudioCatalog load_audio_catalog(const std::filesystem::path& path);
bool save_audio_catalog(const AudioCatalog& catalog, const std::filesystem::path& path);
void register_audio_catalog_loader(AssetManager& assets);

} // namespace kin
