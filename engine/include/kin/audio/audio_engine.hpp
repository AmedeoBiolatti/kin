#pragma once

#include <kin/audio/audio_catalog.hpp>
#include <kin/audio/backend.hpp>
#include <kin/audio/spatial_audio.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kin {

struct AudioHandle {
    u64 id = 0;
    bool valid() const { return id != 0; }
    explicit operator bool() const { return valid(); }
    friend bool operator==(AudioHandle, AudioHandle) = default;
};

struct AudioPlayRequest {
    std::string cue;
    Vec2f position{};
    bool has_position = false;
    f32 volume = 1.0f;
    f32 pitch = 1.0f;
    i32 priority_boost = 0;
};

struct AudioEngineConfig {
    i32 sample_rate = 48000;
    i32 channels = 2;
    i32 music_voices = 2;
    i32 ambient_voices = 12;
    i32 sound_effect_voices = 64;
    i32 ui_voices = 8;
    i32 queue_target_frames = 2048;
};

struct AudioEngineStats {
    i32 active_voices = 0;
    i32 played_requests = 0;
    i32 culled_requests = 0;
    i32 stolen_voices = 0;
    i32 mixed_frames = 0;
};

class AudioEngine {
public:
    explicit AudioEngine(AudioEngineConfig config = {});
    AudioEngine(std::unique_ptr<IAudioBackend> backend, AudioEngineConfig config = {});

    void update(f32 dt);
    AudioHandle play(const AudioCatalog& catalog, const AudioPlayRequest& request);
    void stop(AudioHandle handle, f32 fade = 0.0f);
    void stop_bus(std::string_view bus, f32 fade = 0.0f);
    void set_bus_volume(std::string_view bus, f32 volume);
    void set_listener(Vec2f position);
    void set_position(AudioHandle handle, Vec2f position);

    bool playing(AudioHandle handle) const;
    i32 active_voice_count() const;
    i32 active_voice_count(AudioCategory category) const;
    f32 bus_volume(std::string_view bus) const;
    f32 effective_bus_volume(const AudioCatalog& catalog, std::string_view bus) const;
    const AudioEngineStats& stats() const { return _stats; }
    void reset_stats();

private:
    struct Voice {
        AudioHandle handle;
        std::string cue_id;
        std::string bus;
        AudioCategory category = AudioCategory::Sound;
        const AudioClip* clip = nullptr;
        f64 cursor = 0.0;
        f32 volume = 1.0f;
        f32 pitch = 1.0f;
        i32 priority = 0;
        f32 age = 0.0f;
        bool loop = false;
        bool spatial = false;
        f32 min_distance = 32.0f;
        f32 max_distance = 512.0f;
        Vec2f position{};
        bool has_position = false;
    };

    const AudioClip* load_clip(const AudioCatalog& catalog, std::string_view clip_id);
    i32 category_cap(AudioCategory category) const;
    i32 category_bias(AudioCategory category) const;
    f32 distance_penalty(const AudioCue& cue, const AudioPlayRequest& request) const;
    f32 voice_score(const Voice& voice) const;
    bool allocate_voice(const AudioCue& cue, const AudioPlayRequest& request, i32& out_index);
    void mix_frames(const AudioCatalog* catalog, i32 frames);

    AudioEngineConfig _config;
    std::unique_ptr<IAudioBackend> _backend;
    const AudioCatalog* _catalog = nullptr;
    Vec2f _listener{};
    u64 _next_handle = 1;
    std::vector<Voice> _voices;
    std::unordered_map<std::string, AudioClip> _clip_cache;
    std::unordered_map<std::string, f32> _bus_overrides;
    AudioEngineStats _stats;
};

} // namespace kin
