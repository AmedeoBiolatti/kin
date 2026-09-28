#include <kin/audio/audio_engine.hpp>

#include <kin/platform/log.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace kin {

namespace {

f32 clamp_gain(f32 value) {
    return std::clamp(value, 0.0f, 1.0f);
}

bool same_voice_bucket(AudioCategory a, AudioCategory b) {
    if ((a == AudioCategory::Sound || a == AudioCategory::Effect) &&
        (b == AudioCategory::Sound || b == AudioCategory::Effect)) {
        return true;
    }
    return a == b;
}

} // namespace

AudioEngine::AudioEngine(AudioEngineConfig config)
    : _config(config) {
    try {
        _backend = create_sdl_audio_backend(config.sample_rate, config.channels);
        KIN_LOG_INFO_F("audio",
                       "audio engine initialized",
                       (LogFields{
                           {.name = "backend", .value = "sdl"},
                           {.name = "sample_rate", .value = std::to_string(config.sample_rate)},
                           {.name = "channels", .value = std::to_string(config.channels)},
                       }));
    } catch (const std::exception& error) {
        KIN_LOG_WARN_F("audio",
                       "audio backend unavailable, using null backend",
                       (LogFields{
                           {.name = "backend", .value = "sdl"},
                           {.name = "sample_rate", .value = std::to_string(config.sample_rate)},
                           {.name = "channels", .value = std::to_string(config.channels)},
                           {.name = "reason", .value = error.what()},
                       }));
        _backend = create_null_audio_backend(config.sample_rate, config.channels);
    }
}

AudioEngine::AudioEngine(std::unique_ptr<IAudioBackend> backend, AudioEngineConfig config)
    : _config(config),
      _backend(std::move(backend)) {
    if (!_backend) {
        KIN_LOG_WARN("audio", "audio backend missing, using null backend");
        _backend = create_null_audio_backend(config.sample_rate, config.channels);
    }
    KIN_LOG_INFO_F("audio",
                   "audio engine initialized",
                   (LogFields{
                       {.name = "backend", .value = _backend->available() ? "custom" : "null"},
                       {.name = "sample_rate", .value = std::to_string(config.sample_rate)},
                       {.name = "channels", .value = std::to_string(config.channels)},
                   }));
}

void AudioEngine::update(f32 dt) {
    for (Voice& voice : _voices) {
        voice.age += std::max(0.0f, dt);
    }

    if (!_backend) {
        return;
    }

    const i32 queued = _backend->queued_frames();
    const i32 wanted = std::max(0, _config.queue_target_frames - queued);
    if (wanted > 0) {
        mix_frames(_catalog, wanted);
    }
}

const AudioClip* AudioEngine::load_clip(const AudioCatalog& catalog, std::string_view clip_id) {
    const std::filesystem::path path = catalog.resolve_clip_path(clip_id);
    if (path.empty()) {
        KIN_LOG_WARN_F("audio",
                       "audio clip missing from catalog",
                       (LogFields{{.name = "clip", .value = std::string{clip_id}}}));
        return nullptr;
    }
    const std::string key = path.generic_string();
    if (auto found = _clip_cache.find(key); found != _clip_cache.end()) {
        KIN_LOG_DEBUG_F("audio",
                        "audio clip cache hit",
                        (LogFields{
                            {.name = "clip", .value = std::string{clip_id}},
                            {.name = "path", .value = key},
                        }));
        return found->second.valid() ? &found->second : nullptr;
    }
    AudioClip clip;
    try {
        clip = load_audio_clip(path);
    } catch (const std::exception& error) {
        KIN_LOG_ERROR_F("audio",
                        "audio clip load failed",
                        (LogFields{
                            {.name = "clip", .value = std::string{clip_id}},
                            {.name = "path", .value = key},
                            {.name = "error", .value = error.what()},
                        }));
        throw;
    }
    auto [it, inserted] = _clip_cache.emplace(key, std::move(clip));
    (void)inserted;
    KIN_LOG_INFO_F("audio",
                   "audio clip loaded",
                   (LogFields{
                       {.name = "clip", .value = std::string{clip_id}},
                       {.name = "path", .value = key},
                       {.name = "frames", .value = std::to_string(it->second.frame_count())},
                       {.name = "channels", .value = std::to_string(it->second.channels())},
                   }));
    return it->second.valid() ? &it->second : nullptr;
}

AudioHandle AudioEngine::play(const AudioCatalog& catalog, const AudioPlayRequest& request) {
    _catalog = &catalog;
    const AudioCue* cue = catalog.cue(request.cue);
    if (!cue || cue->clips.empty()) {
        ++_stats.culled_requests;
        KIN_LOG_WARN_F("audio",
                       "audio play rejected",
                       (LogFields{
                           {.name = "cue", .value = std::string{request.cue}},
                           {.name = "reason", .value = cue ? "cue has no clips" : "missing cue"},
                       }));
        return {};
    }

    i32 index = -1;
    if (!allocate_voice(*cue, request, index)) {
        ++_stats.culled_requests;
        KIN_LOG_WARN_F("audio",
                       "audio play rejected",
                       (LogFields{
                           {.name = "cue", .value = std::string{request.cue}},
                           {.name = "category", .value = std::string{audio_category_name(cue->category)}},
                           {.name = "reason", .value = "voice limit or priority"},
                       }));
        return {};
    }

    const AudioClip* clip = load_clip(catalog, cue->clips.front());
    if (!clip) {
        ++_stats.culled_requests;
        KIN_LOG_WARN_F("audio",
                       "audio play rejected",
                       (LogFields{
                           {.name = "cue", .value = cue->id},
                           {.name = "clip", .value = cue->clips.front()},
                           {.name = "reason", .value = "clip unavailable"},
                       }));
        return {};
    }

    Voice voice{
        .handle = AudioHandle{_next_handle++},
        .cue_id = cue->id,
        .bus = cue->bus,
        .category = cue->category,
        .clip = clip,
        .volume = cue->volume * request.volume,
        .pitch = std::max(0.01f, cue->pitch * request.pitch),
        .priority = cue->priority + request.priority_boost,
        .loop = cue->loop,
        .spatial = cue->spatial,
        .min_distance = cue->min_distance,
        .max_distance = cue->max_distance,
        .position = request.position,
        .has_position = request.has_position,
    };

    if (index == static_cast<i32>(_voices.size())) {
        _voices.push_back(std::move(voice));
    } else {
        _voices[static_cast<std::size_t>(index)] = std::move(voice);
    }
    ++_stats.played_requests;
    KIN_LOG_DEBUG_F("audio",
                    "audio cue playing",
                    (LogFields{
                        {.name = "cue", .value = cue->id},
                        {.name = "clip", .value = cue->clips.front()},
                        {.name = "bus", .value = cue->bus},
                        {.name = "category", .value = std::string{audio_category_name(cue->category)}},
                    }));
    return _voices[static_cast<std::size_t>(index)].handle;
}

void AudioEngine::stop(AudioHandle handle, f32) {
    if (!handle) {
        return;
    }
    std::erase_if(_voices, [handle](const Voice& voice) {
        return voice.handle == handle;
    });
}

void AudioEngine::stop_bus(std::string_view bus, f32) {
    std::erase_if(_voices, [bus](const Voice& voice) {
        return voice.bus == bus;
    });
}

void AudioEngine::set_bus_volume(std::string_view bus, f32 volume) {
    _bus_overrides[std::string{bus}] = std::max(0.0f, volume);
    KIN_LOG_INFO_F("audio",
                   "audio bus volume set",
                   (LogFields{
                       {.name = "bus", .value = std::string{bus}},
                       {.name = "volume", .value = std::to_string(std::max(0.0f, volume))},
                   }));
}

void AudioEngine::set_listener(Vec2f position) {
    _listener = position;
}

void AudioEngine::set_position(AudioHandle handle, Vec2f position) {
    for (Voice& voice : _voices) {
        if (voice.handle == handle) {
            voice.position = position;
            voice.has_position = true;
            return;
        }
    }
}

bool AudioEngine::playing(AudioHandle handle) const {
    return std::ranges::any_of(_voices, [handle](const Voice& voice) {
        return voice.handle == handle;
    });
}

i32 AudioEngine::active_voice_count() const {
    return static_cast<i32>(_voices.size());
}

i32 AudioEngine::active_voice_count(AudioCategory category) const {
    return static_cast<i32>(std::ranges::count_if(_voices, [category](const Voice& voice) {
        return same_voice_bucket(category, voice.category);
    }));
}

f32 AudioEngine::bus_volume(std::string_view bus) const {
    if (const auto found = _bus_overrides.find(std::string{bus}); found != _bus_overrides.end()) {
        return found->second;
    }
    return 1.0f;
}

f32 AudioEngine::effective_bus_volume(const AudioCatalog& catalog, std::string_view bus) const {
    return catalog.effective_bus_volume(bus) * bus_volume(bus);
}

void AudioEngine::reset_stats() {
    _stats = {};
}

i32 AudioEngine::category_cap(AudioCategory category) const {
    switch (category) {
    case AudioCategory::Music: return _config.music_voices;
    case AudioCategory::Ambient: return _config.ambient_voices;
    case AudioCategory::Sound:
    case AudioCategory::Effect: return _config.sound_effect_voices;
    case AudioCategory::Ui: return _config.ui_voices;
    }
    return _config.sound_effect_voices;
}

i32 AudioEngine::category_bias(AudioCategory category) const {
    switch (category) {
    case AudioCategory::Music: return 10000;
    case AudioCategory::Ui: return 5000;
    case AudioCategory::Effect: return 1000;
    case AudioCategory::Sound: return 0;
    case AudioCategory::Ambient: return -500;
    }
    return 0;
}

f32 AudioEngine::distance_penalty(const AudioCue& cue, const AudioPlayRequest& request) const {
    if (!cue.spatial || !request.has_position) {
        return 0.0f;
    }
    const f32 dx = request.position.x - _listener.x;
    const f32 dy = request.position.y - _listener.y;
    const f32 distance = std::sqrt(dx * dx + dy * dy);
    if (distance <= cue.max_distance) {
        return distance * 0.25f;
    }
    return 5000.0f + (distance - cue.max_distance);
}

f32 AudioEngine::voice_score(const Voice& voice) const {
    f32 score = static_cast<f32>(voice.priority + category_bias(voice.category)) - voice.age * 2.0f;
    if (voice.spatial && voice.has_position) {
        const f32 dx = voice.position.x - _listener.x;
        const f32 dy = voice.position.y - _listener.y;
        const f32 distance = std::sqrt(dx * dx + dy * dy);
        score -= distance <= voice.max_distance ? distance * 0.25f : 5000.0f + (distance - voice.max_distance);
    }
    return score;
}

bool AudioEngine::allocate_voice(const AudioCue& cue, const AudioPlayRequest& request, i32& out_index) {
    const f32 request_score = static_cast<f32>(cue.priority + request.priority_boost + category_bias(cue.category))
        - distance_penalty(cue, request);
    if (request_score <= -1000.0f) {
        return false;
    }

    if (cue.max_instances > 0) {
        std::vector<i32> matching;
        for (i32 i = 0; i < static_cast<i32>(_voices.size()); ++i) {
            if (_voices[static_cast<std::size_t>(i)].cue_id == cue.id) {
                matching.push_back(i);
            }
        }
        if (static_cast<i32>(matching.size()) >= cue.max_instances) {
            i32 steal = matching.front();
            f32 lowest = voice_score(_voices[static_cast<std::size_t>(steal)]);
            for (const i32 index : matching) {
                const f32 score = voice_score(_voices[static_cast<std::size_t>(index)]);
                if (score < lowest) {
                    lowest = score;
                    steal = index;
                }
            }
            if (request_score <= lowest) {
                return false;
            }
            out_index = steal;
            ++_stats.stolen_voices;
            return true;
        }
    }

    const i32 cap = category_cap(cue.category);
    if (active_voice_count(cue.category) < cap) {
        out_index = static_cast<i32>(_voices.size());
        return true;
    }

    i32 steal = -1;
    f32 lowest = 0.0f;
    for (i32 i = 0; i < static_cast<i32>(_voices.size()); ++i) {
        const Voice& voice = _voices[static_cast<std::size_t>(i)];
        if (!same_voice_bucket(cue.category, voice.category)) {
            continue;
        }
        const f32 score = voice_score(voice);
        if (steal < 0 || score < lowest) {
            steal = i;
            lowest = score;
        }
    }
    if (steal < 0 || request_score <= lowest) {
        return false;
    }
    out_index = steal;
    ++_stats.stolen_voices;
    return true;
}

void AudioEngine::mix_frames(const AudioCatalog* catalog, i32 frames) {
    if (frames <= 0 || !_backend || _config.channels <= 0) {
        return;
    }

    std::vector<f32> mix(static_cast<std::size_t>(frames * _config.channels), 0.0f);
    for (Voice& voice : _voices) {
        if (!voice.clip || !voice.clip->valid()) {
            continue;
        }

        f32 gain = voice.volume;
        f32 pan = 0.0f;
        if (catalog) {
            gain *= effective_bus_volume(*catalog, voice.bus);
        }
        if (voice.spatial && voice.has_position) {
            const SpatialAudioResult spatial = calculate_spatial_audio(_listener, voice.position, {
                .min_distance = voice.min_distance,
                .max_distance = voice.max_distance,
            });
            gain *= spatial.gain;
            pan = spatial.pan;
        }
        gain = clamp_gain(gain);
        if (gain <= 0.0f) {
            continue;
        }

        const std::span<const f32> samples = voice.clip->samples();
        const i32 clip_channels = voice.clip->channels();
        const i32 clip_frames = voice.clip->frame_count();
        const f32 left_gain = pan <= 0.0f ? 1.0f : 1.0f - pan;
        const f32 right_gain = pan >= 0.0f ? 1.0f : 1.0f + pan;
        for (i32 frame = 0; frame < frames; ++frame) {
            i32 src_frame = static_cast<i32>(voice.cursor);
            if (src_frame >= clip_frames) {
                if (!voice.loop) {
                    break;
                }
                voice.cursor = std::fmod(voice.cursor, static_cast<f64>(clip_frames));
                src_frame = static_cast<i32>(voice.cursor);
            }

            const std::size_t src = static_cast<std::size_t>(src_frame * clip_channels);
            const f32 left = samples[src];
            const f32 right = clip_channels > 1 ? samples[src + 1] : left;
            const std::size_t dst = static_cast<std::size_t>(frame * _config.channels);
            mix[dst] += left * gain * left_gain;
            if (_config.channels > 1) {
                mix[dst + 1] += right * gain * right_gain;
            }
            voice.cursor += std::max(0.01f, voice.pitch);
        }
    }

    for (f32& sample : mix) {
        sample = std::clamp(sample, -1.0f, 1.0f);
    }
    _backend->queue_interleaved(mix);
    _stats.mixed_frames += frames;

    std::erase_if(_voices, [](const Voice& voice) {
        return voice.clip && !voice.loop && voice.cursor >= voice.clip->frame_count();
    });
    _stats.active_voices = static_cast<i32>(_voices.size());
}

} // namespace kin
