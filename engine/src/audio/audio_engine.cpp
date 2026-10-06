#include <kin/audio/audio_engine.hpp>

#include <kin/audio/audio_decoder.hpp>

#include <kin/core/jobs.hpp>
#include <kin/core/json.hpp>
#include <kin/core/rng.hpp>
#include <kin/platform/log.hpp>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <numbers>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kin {

namespace {

constexpr i32 no_bus = -1;
// Shortest fade a stop or a steal gets, so cutting a voice off never clicks.
constexpr f32 declick_seconds = 0.005f;
// How fast the master limiter lets the level back up after a peak.
constexpr f32 limiter_release_seconds = 0.05f;
// Frames update() mixes at once for a backend that is not real time.
constexpr i32 render_chunk_frames = 1024;
// Frames a streamed voice decodes at a time.
constexpr i32 stream_buffer_frames = 1024;

bool same_voice_bucket(AudioCategory a, AudioCategory b) {
    if ((a == AudioCategory::Sound || a == AudioCategory::Effect) &&
        (b == AudioCategory::Sound || b == AudioCategory::Effect)) {
        return true;
    }
    return a == b;
}

struct PanGains {
    f32 left = 1.0f;
    f32 right = 1.0f;
};

// Equal-power pan law, scaled so a centred sound keeps unity gain: a sound
// moving across the listener keeps its loudness instead of dipping in the middle.
PanGains pan_gains(f32 pan) {
    const f32 angle = (std::clamp(pan, -1.0f, 1.0f) + 1.0f) * std::numbers::pi_v<f32> * 0.25f;
    return {
        .left = std::numbers::sqrt2_v<f32> * std::cos(angle),
        .right = std::numbers::sqrt2_v<f32> * std::sin(angle),
    };
}

} // namespace

struct AudioEngine::State {
    struct Bus {
        std::string id;
        i32 parent = no_bus;
        f32 authored = 1.0f; // the catalog's volume
        bool authored_muted = false;
        f32 volume = 1.0f; // set_bus_volume, fading toward target
        f32 target = 1.0f;
        f32 step = 0.0f; // volume change per output frame while fading
        bool muted = false;
        bool paused = false;
        // Resolved through the parents at the start of each mixed block.
        f32 duck = 1.0f; // from the duck rules aimed at this bus
        f32 gain = 1.0f;
        bool paused_now = false;
    };

    struct Duck {
        i32 bus = no_bus;
        i32 when = no_bus;
        f32 volume = 0.5f;
        i32 attack_frames = 0;
        i32 release_frames = 0;
        f32 level = 1.0f; // 1 untouched, `volume` fully ducked
    };

    struct Voice {
        AudioHandle handle;
        std::string cue_id;
        i32 bus = no_bus;
        AudioCategory category = AudioCategory::Sound;
        std::shared_ptr<const AudioClip> clip;
        // Read state: the two source frames the output falls between, `frac` of
        // the way from f0 to f1, and the next source frame to read.
        i64 next_frame = 0;
        f32 f0[2]{};
        f32 f1[2]{};
        f64 frac = 0.0;
        f64 rate_ratio = 1.0; // clip rate / output rate
        // Loop points from the catalog's clip, in source frames.
        i64 loop_start = 0;
        i64 loop_end = 0;
        // A streamed clip's decoder, and the frames it decoded last.
        std::unique_ptr<AudioDecoder> stream;
        std::vector<f32> stream_buffer;
        i64 stream_pos = 0;
        i64 stream_count = 0;
        bool primed = false;
        bool at_end = false; // f1 is past the end of the clip
        bool done = false;
        f32 volume = 1.0f;
        f32 cue_volume = 1.0f; // set_volume is relative to it
        f32 volume_target = 1.0f; // set_volume fades by block toward it
        f32 volume_step = 0.0f;
        i32 volume_frames = 0;
        f32 base_pitch = 1.0f; // the cue's pitch and variation; set_pitch scales it
        f32 pitch = 1.0f;
        i32 priority = 0;
        f32 age = 0.0f;
        bool loop = false;
        bool spatial = false;
        f32 min_distance = 32.0f;
        f32 max_distance = 512.0f;
        Vec2f position{};
        bool has_position = false;
        f32 fade = 1.0f;
        f32 fade_target = 1.0f;
        f32 fade_step = 0.0f;
        i32 fade_frames = 0; // left until fade reaches fade_target
        bool stopping = false;
        // Gains reached at the end of the last block; the next block ramps from
        // them, so moving sources and volume changes do not zipper.
        f32 gain_left = 0.0f;
        f32 gain_right = 0.0f;
        bool fresh = true;
    };

    State(AudioEngineConfig config_value, i32 rate, i32 channel_count)
        : config(config_value),
          sample_rate(std::max(1, rate)),
          channels(std::max(1, channel_count)),
          rng(make_key(config_value.seed)),
          limiter_release(std::exp(-1.0f / (limiter_release_seconds * static_cast<f32>(sample_rate)))) {
    }

    AudioEngineConfig config;
    i32 sample_rate = 48000;
    i32 channels = 2;

    // Shared with the audio thread: only touched with `mutex` held.
    mutable std::mutex mutex;
    std::vector<Voice> voices;
    std::vector<Bus> buses;
    std::unordered_map<std::string, i32> bus_index;
    std::vector<AudioDuck> duck_source; // the catalog rules `ducks` was built from
    std::vector<Duck> ducks;
    Vec2f listener{};
    AudioEngineStats stats;
    f32 limiter_env = 0.0f;
    f32 limiter_release = 0.0f;

    // Game thread only.
    u64 next_handle = 1;
    RngKey rng;
    std::unordered_map<std::string, std::shared_ptr<const AudioClip>> clip_cache;
    std::unordered_map<std::string, std::shared_ptr<const AudioClip>> memory_clips;
    std::unordered_map<std::string, Job<AudioClip>> loading;
    std::unordered_map<std::string, std::size_t> last_clip; // per cue, so variations do not repeat
    f64 pending_frames = 0.0;
    std::vector<f32> scratch;
    AudioHandle music;
    std::string music_cue;

    RngKey next_key() {
        auto [next, sub] = split(rng);
        rng = next;
        return sub;
    }

    i32 frames_for(f32 seconds) const {
        return std::max(1, static_cast<i32>(std::lround(std::max(0.0f, seconds) * static_cast<f32>(sample_rate))));
    }

    // --- buses (mutex held) ---

    i32 ensure_bus(std::string_view id) {
        if (const auto found = bus_index.find(std::string{id}); found != bus_index.end()) {
            return found->second;
        }
        const i32 index = static_cast<i32>(buses.size());
        buses.push_back({.id = std::string{id}});
        bus_index.emplace(std::string{id}, index);
        return index;
    }

    // Registers `id` and its parents, refreshing what the catalog says about them.
    i32 sync_bus(const AudioCatalog& catalog, std::string_view id) {
        const i32 index = ensure_bus(id);
        i32 child = index;
        const AudioBus* authored = catalog.bus(id);
        for (i32 guard = 0; authored && guard < 32; ++guard) {
            Bus& bus = buses[static_cast<std::size_t>(child)];
            bus.authored = authored->volume;
            bus.authored_muted = authored->muted;
            if (authored->parent.empty()) {
                bus.parent = no_bus;
                break;
            }
            const i32 parent = ensure_bus(authored->parent);
            buses[static_cast<std::size_t>(child)].parent = parent;
            child = parent;
            authored = catalog.bus(authored->parent);
        }
        return index;
    }

    bool bus_under(i32 bus, i32 ancestor) const {
        for (i32 guard = 0; bus != no_bus && guard < 32; ++guard) {
            if (bus == ancestor) {
                return true;
            }
            bus = buses[static_cast<std::size_t>(bus)].parent;
        }
        return false;
    }

    const Bus* find_bus(std::string_view id) const {
        const auto found = bus_index.find(std::string{id});
        return found == bus_index.end() ? nullptr : &buses[static_cast<std::size_t>(found->second)];
    }

    // Rebuilds the duck rules when the catalog's changed, keeping how far each
    // surviving rule had ducked.
    void sync_ducks(const AudioCatalog& catalog) {
        if (catalog.ducks() == duck_source) {
            return;
        }
        std::vector<Duck> next;
        for (const AudioDuck& authored : catalog.ducks()) {
            Duck duck{
                .bus = sync_bus(catalog, authored.bus),
                .when = sync_bus(catalog, authored.when),
                .volume = std::max(0.0f, authored.volume),
                .attack_frames = authored.attack > 0.0f ? frames_for(authored.attack) : 0,
                .release_frames = authored.release > 0.0f ? frames_for(authored.release) : 0,
            };
            for (const Duck& old : ducks) {
                if (old.bus == duck.bus && old.when == duck.when) {
                    duck.level = old.level;
                }
            }
            next.push_back(duck);
        }
        ducks = std::move(next);
        duck_source = catalog.ducks();
    }

    void advance_buses(i32 frames) {
        for (Bus& bus : buses) {
            if (bus.step != 0.0f) {
                bus.volume += bus.step * static_cast<f32>(frames);
                if ((bus.step > 0.0f && bus.volume >= bus.target) || (bus.step < 0.0f && bus.volume <= bus.target)) {
                    bus.volume = bus.target;
                    bus.step = 0.0f;
                }
            }
            bus.duck = 1.0f;
            bus.paused_now = false;
            const Bus* level = &bus;
            for (i32 guard = 0; level && guard < 32; ++guard) {
                bus.paused_now = bus.paused_now || level->paused;
                level = level->parent == no_bus ? nullptr : &buses[static_cast<std::size_t>(level->parent)];
            }
        }

        for (Duck& duck : ducks) {
            const bool active = std::ranges::any_of(voices, [&](const Voice& voice) {
                return !voice.stopping && !voice.done && voice.bus != no_bus &&
                       !buses[static_cast<std::size_t>(voice.bus)].paused_now && bus_under(voice.bus, duck.when);
            });
            const f32 target = active ? duck.volume : 1.0f;
            const i32 ramp = active ? duck.attack_frames : duck.release_frames;
            const f32 step = ramp > 0 ? std::fabs(1.0f - duck.volume) / static_cast<f32>(ramp) * static_cast<f32>(frames)
                                      : std::fabs(target - duck.level);
            duck.level = duck.level < target ? std::min(target, duck.level + step) : std::max(target, duck.level - step);
            buses[static_cast<std::size_t>(duck.bus)].duck *= duck.level;
        }

        for (Bus& bus : buses) {
            f32 gain = 1.0f;
            const Bus* level = &bus;
            for (i32 guard = 0; level && guard < 32; ++guard) {
                if (level->muted || level->authored_muted) {
                    gain = 0.0f;
                }
                gain *= level->authored * level->volume * level->duck;
                level = level->parent == no_bus ? nullptr : &buses[static_cast<std::size_t>(level->parent)];
            }
            bus.gain = gain;
        }
    }

    // --- voices (mutex held) ---

    void start_fade(Voice& voice, f32 target, f32 seconds) {
        voice.fade_target = target;
        if (seconds <= 0.0f) {
            voice.fade = target;
            voice.fade_step = 0.0f;
            voice.fade_frames = 0;
            return;
        }
        voice.fade_frames = frames_for(seconds);
        voice.fade_step = (target - voice.fade) / static_cast<f32>(voice.fade_frames);
    }

    void stop_voice(Voice& voice, f32 fade) {
        if (voice.stopping && voice.fade_frames <= frames_for(std::max(fade, declick_seconds))) {
            return; // already fading out at least as quickly
        }
        voice.stopping = true;
        start_fade(voice, 0.0f, std::max(fade, declick_seconds));
    }

    i32 counted_voices(AudioCategory category) const {
        return static_cast<i32>(std::ranges::count_if(voices, [category](const Voice& voice) {
            return !voice.stopping && same_voice_bucket(category, voice.category);
        }));
    }

    i32 category_cap(AudioCategory category) const {
        switch (category) {
        case AudioCategory::Music: return config.music_voices;
        case AudioCategory::Ambient: return config.ambient_voices;
        case AudioCategory::Sound:
        case AudioCategory::Effect: return config.sound_effect_voices;
        case AudioCategory::Ui: return config.ui_voices;
        }
        return config.sound_effect_voices;
    }

    static i32 category_bias(AudioCategory category) {
        switch (category) {
        case AudioCategory::Music: return 10000;
        case AudioCategory::Ui: return 5000;
        case AudioCategory::Effect: return 1000;
        case AudioCategory::Sound: return 0;
        case AudioCategory::Ambient: return -500;
        }
        return 0;
    }

    f32 distance_penalty(bool spatial, bool has_position, Vec2f position, f32 max_distance) const {
        if (!spatial || !has_position) {
            return 0.0f;
        }
        const f32 dx = position.x - listener.x;
        const f32 dy = position.y - listener.y;
        const f32 distance = std::sqrt(dx * dx + dy * dy);
        return distance <= max_distance ? distance * 0.25f : 5000.0f + (distance - max_distance);
    }

    f32 voice_score(const Voice& voice) const {
        return static_cast<f32>(voice.priority + category_bias(voice.category)) - voice.age * 2.0f
            - distance_penalty(voice.spatial, voice.has_position, voice.position, voice.max_distance);
    }

    // Finds room for a new voice: true with `steal` = -1 when there is a free
    // slot, or the index of the voice to replace; false when the request loses.
    bool allocate_voice(const AudioCue& cue, const AudioPlayRequest& request, i32& steal) {
        steal = -1;
        const f32 request_score = static_cast<f32>(cue.priority + request.priority_boost + category_bias(cue.category))
            - distance_penalty(cue.spatial, request.has_position, request.position, cue.max_distance);
        if (request_score <= -1000.0f) {
            return false;
        }

        const auto lowest_of = [&](auto&& eligible) {
            i32 index = -1;
            f32 lowest = 0.0f;
            for (i32 i = 0; i < static_cast<i32>(voices.size()); ++i) {
                const Voice& voice = voices[static_cast<std::size_t>(i)];
                if (voice.stopping || !eligible(voice)) {
                    continue;
                }
                const f32 score = voice_score(voice);
                if (index < 0 || score < lowest) {
                    index = i;
                    lowest = score;
                }
            }
            return std::pair{index, lowest};
        };

        if (cue.max_instances > 0) {
            const auto same_cue = [&](const Voice& voice) { return voice.cue_id == cue.id; };
            const i32 instances = static_cast<i32>(std::ranges::count_if(voices, [&](const Voice& voice) {
                return !voice.stopping && same_cue(voice);
            }));
            if (instances >= cue.max_instances) {
                const auto [index, lowest] = lowest_of(same_cue);
                if (index < 0 || request_score <= lowest) {
                    return false;
                }
                steal = index;
                return true;
            }
        }

        if (counted_voices(cue.category) < category_cap(cue.category)) {
            return true;
        }
        const auto [index, lowest] = lowest_of([&](const Voice& voice) {
            return same_voice_bucket(cue.category, voice.category);
        });
        if (index < 0 || request_score <= lowest) {
            return false;
        }
        steal = index;
        return true;
    }

    // --- mixing (mutex held) ---

    // Jumps back to the loop start.
    static bool rewind(Voice& voice) {
        const i64 start = std::clamp<i64>(voice.loop_start, 0, std::max(0, voice.clip->frame_count() - 1));
        if (voice.stream) {
            if (!voice.stream->seek(start)) {
                return false;
            }
            voice.stream_pos = voice.stream_count = 0;
        }
        voice.next_frame = start;
        return true;
    }

    static bool refill(Voice& voice, i64 end) {
        const i64 channels = voice.clip->channels();
        const i64 want = std::min<i64>(static_cast<i64>(voice.stream_buffer.size()) / channels, end - voice.next_frame);
        voice.stream_pos = 0;
        voice.stream_count = want > 0
            ? voice.stream->read(std::span<f32>{voice.stream_buffer.data(), static_cast<std::size_t>(want * channels)})
            : 0;
        return voice.stream_count > 0;
    }

    static bool read_frame(Voice& voice, f32 (&out)[2]) {
        const AudioClip& clip = *voice.clip;
        const i64 frames = clip.frame_count();
        const i64 end = voice.loop && voice.loop_end > 0 ? std::min(voice.loop_end, frames) : frames;
        if (voice.next_frame >= end && (!voice.loop || !rewind(voice))) {
            return false;
        }
        const std::size_t channels = static_cast<std::size_t>(clip.channels());
        const f32* frame = nullptr;
        if (voice.stream) {
            if (voice.stream_pos >= voice.stream_count && !refill(voice, end)) {
                // The file ended before its header said it would.
                if (!voice.loop || !rewind(voice) || !refill(voice, end)) {
                    return false;
                }
            }
            frame = voice.stream_buffer.data() + static_cast<std::size_t>(voice.stream_pos++) * channels;
        } else {
            frame = clip.samples().data() + static_cast<std::size_t>(voice.next_frame) * channels;
        }
        out[0] = frame[0];
        out[1] = channels > 1 ? frame[1] : frame[0];
        ++voice.next_frame;
        return true;
    }

    static void advance_source(Voice& voice) {
        if (voice.at_end) {
            voice.done = true;
            return;
        }
        voice.f0[0] = voice.f1[0];
        voice.f0[1] = voice.f1[1];
        if (!read_frame(voice, voice.f1)) {
            voice.f1[0] = voice.f1[1] = 0.0f;
            voice.at_end = true;
        }
    }

    void mix_voice(Voice& voice, std::span<f32> out, i32 frames, PanGains target) {
        if (!voice.primed) {
            voice.primed = true;
            if (!read_frame(voice, voice.f0)) {
                voice.done = true;
                return;
            }
            if (!read_frame(voice, voice.f1)) {
                voice.at_end = true;
            }
        }
        if (voice.fresh) {
            voice.fresh = false;
            voice.gain_left = target.left;
            voice.gain_right = target.right;
        }

        const f32 ramp_left = (target.left - voice.gain_left) / static_cast<f32>(frames);
        const f32 ramp_right = (target.right - voice.gain_right) / static_cast<f32>(frames);
        const f64 step = static_cast<f64>(std::max(0.01f, voice.pitch)) * voice.rate_ratio;
        const std::size_t stride = static_cast<std::size_t>(channels);
        for (i32 frame = 0; frame < frames && !voice.done; ++frame) {
            const f32 t = static_cast<f32>(voice.frac);
            const f32 left = (voice.f0[0] + (voice.f1[0] - voice.f0[0]) * t) * voice.gain_left * voice.fade;
            const f32 right = (voice.f0[1] + (voice.f1[1] - voice.f0[1]) * t) * voice.gain_right * voice.fade;
            f32* dst = out.data() + static_cast<std::size_t>(frame) * stride;
            if (channels == 1) {
                dst[0] += 0.5f * (left + right);
            } else {
                dst[0] += left;
                dst[1] += right;
            }

            voice.gain_left += ramp_left;
            voice.gain_right += ramp_right;
            if (voice.fade_frames > 0) {
                voice.fade = --voice.fade_frames == 0 ? voice.fade_target : voice.fade + voice.fade_step;
            }
            if (voice.stopping && voice.fade <= 0.0f) {
                voice.done = true;
                break;
            }

            voice.frac += step;
            while (voice.frac >= 1.0 && !voice.done) {
                voice.frac -= 1.0;
                advance_source(voice);
            }
        }
        voice.gain_left = target.left;
        voice.gain_right = target.right;
    }

    void limit(std::span<f32> out, i32 frames) {
        const std::size_t stride = static_cast<std::size_t>(channels);
        for (i32 frame = 0; frame < frames; ++frame) {
            f32* sample = out.data() + static_cast<std::size_t>(frame) * stride;
            f32 peak = 0.0f;
            for (std::size_t c = 0; c < stride; ++c) {
                peak = std::max(peak, std::fabs(sample[c]));
            }
            limiter_env = std::max(peak, limiter_env * limiter_release);
            if (limiter_env > 1.0f) {
                const f32 gain = 1.0f / limiter_env;
                for (std::size_t c = 0; c < stride; ++c) {
                    sample[c] = std::clamp(sample[c] * gain, -1.0f, 1.0f);
                }
                ++stats.limited_frames;
            }
        }
    }

    void mix(std::span<f32> out) {
        std::ranges::fill(out, 0.0f);
        const i32 frames = static_cast<i32>(out.size() / static_cast<std::size_t>(channels));
        if (frames <= 0) {
            return;
        }
        advance_buses(frames);

        for (Voice& voice : voices) {
            if (!voice.clip || !voice.clip->valid()) {
                voice.done = true;
                continue;
            }
            const Bus* bus = voice.bus == no_bus ? nullptr : &buses[static_cast<std::size_t>(voice.bus)];
            const bool paused = bus && bus->paused_now;
            if (paused && (voice.fresh || (voice.gain_left == 0.0f && voice.gain_right == 0.0f))) {
                continue; // silent and holding its place
            }

            if (voice.volume_frames > 0) {
                const i32 n = std::min(frames, voice.volume_frames);
                voice.volume_frames -= n;
                voice.volume = voice.volume_frames == 0 ? voice.volume_target
                                                        : voice.volume + voice.volume_step * static_cast<f32>(n);
            }
            f32 gain = paused ? 0.0f : voice.volume * (bus ? bus->gain : 1.0f);
            PanGains pan{};
            if (voice.spatial && voice.has_position) {
                const SpatialAudioResult spatial = calculate_spatial_audio(listener, voice.position, {
                    .min_distance = voice.min_distance,
                    .max_distance = voice.max_distance,
                });
                gain *= spatial.gain;
                pan = pan_gains(spatial.pan);
            }
            gain = std::max(0.0f, gain);
            mix_voice(voice, out, frames, {.left = gain * pan.left, .right = gain * pan.right});
        }

        limit(out, frames);
        stats.mixed_frames += frames;
        std::erase_if(voices, [](const Voice& voice) { return voice.done; });
        stats.active_voices = static_cast<i32>(std::ranges::count_if(voices, [](const Voice& voice) {
            return !voice.stopping;
        }));
    }

    // --- clips (game thread) ---

    std::shared_ptr<const AudioClip> take_loaded(const std::string& key, Job<AudioClip>& job) {
        auto clip = std::make_shared<const AudioClip>(std::move(job.get()));
        clip_cache.insert_or_assign(key, clip);
        return clip;
    }

    void poll_loading() {
        for (auto it = loading.begin(); it != loading.end();) {
            if (!it->second.ready()) {
                ++it;
                continue;
            }
            try {
                take_loaded(it->first, it->second);
            } catch (const std::exception& error) {
                KIN_LOG_ERROR_F("audio",
                                "audio clip preload failed",
                                (LogFields{
                                    {.name = "path", .value = it->first},
                                    {.name = "error", .value = error.what()},
                                }));
            }
            it = loading.erase(it);
        }
    }

    // Streamed and decoded copies of one file are cached apart.
    static std::string cache_key(const std::filesystem::path& path, bool stream) {
        return path.generic_string() + (stream ? "|stream" : "");
    }

    static AudioClip load_file(const std::filesystem::path& path, bool stream) {
        return stream ? load_audio_stream(path) : load_audio_clip(path);
    }

    std::shared_ptr<const AudioClip> load_clip(const AudioCatalog& catalog, std::string_view clip_id) {
        if (const auto found = memory_clips.find(std::string{clip_id}); found != memory_clips.end()) {
            return found->second->valid() ? found->second : nullptr;
        }
        const AudioClipRef* ref = catalog.clip(clip_id);
        const std::filesystem::path path = catalog.resolve_clip_path(clip_id);
        if (!ref || path.empty()) {
            KIN_LOG_WARN_F("audio",
                           "audio clip missing from catalog",
                           (LogFields{{.name = "clip", .value = std::string{clip_id}}}));
            return nullptr;
        }
        const std::string key = cache_key(path, ref->stream);
        if (const auto found = clip_cache.find(key); found != clip_cache.end()) {
            return found->second->valid() ? found->second : nullptr;
        }

        std::shared_ptr<const AudioClip> clip;
        try {
            if (const auto pending = loading.find(key); pending != loading.end()) {
                // Waits for the preload instead of decoding the file a second time.
                auto job = std::move(pending->second);
                loading.erase(pending);
                clip = take_loaded(key, job);
            } else {
                clip = std::make_shared<const AudioClip>(load_file(path, ref->stream));
                clip_cache.emplace(key, clip);
            }
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
        return clip->valid() ? clip : nullptr;
    }

    // A cue with several clips plays a random one, never the same twice in a row.
    const std::string& choose_clip(const AudioCue& cue) {
        if (cue.clips.size() == 1) {
            return cue.clips.front();
        }
        const i32 options = static_cast<i32>(cue.clips.size());
        const auto [last, first] = last_clip.try_emplace(cue.id, 0);
        std::size_t pick = 0;
        if (first || last->second >= cue.clips.size()) {
            pick = static_cast<std::size_t>(rng_i32(next_key(), 0, options - 1));
        } else {
            // Draw from the others: skip over the last one.
            pick = static_cast<std::size_t>(rng_i32(next_key(), 0, options - 2));
            pick += pick >= last->second ? 1 : 0;
        }
        last->second = pick;
        return cue.clips[pick];
    }
};

AudioEngine::AudioEngine(AudioEngineConfig config) {
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
    _state = std::make_unique<State>(config, _backend->sample_rate(), _backend->channels());
    _backend->start([state = _state.get()](std::span<f32> out) {
        const std::scoped_lock lock{state->mutex};
        state->mix(out);
    });
}

AudioEngine::AudioEngine(std::unique_ptr<IAudioBackend> backend, AudioEngineConfig config)
    : _backend(std::move(backend)) {
    if (!_backend) {
        KIN_LOG_WARN("audio", "audio backend missing, using null backend");
        _backend = create_null_audio_backend(config.sample_rate, config.channels);
    }
    KIN_LOG_INFO_F("audio",
                   "audio engine initialized",
                   (LogFields{
                       {.name = "backend", .value = _backend->available() ? "custom" : "null"},
                       {.name = "sample_rate", .value = std::to_string(_backend->sample_rate())},
                       {.name = "channels", .value = std::to_string(_backend->channels())},
                   }));
    _state = std::make_unique<State>(config, _backend->sample_rate(), _backend->channels());
    _backend->start([state = _state.get()](std::span<f32> out) {
        const std::scoped_lock lock{state->mutex};
        state->mix(out);
    });
}

AudioEngine::~AudioEngine() {
    if (_backend) {
        _backend->stop();
    }
}

AudioEngine::AudioEngine(AudioEngine&& other) noexcept = default;

AudioEngine& AudioEngine::operator=(AudioEngine&& other) noexcept {
    if (this != &other) {
        if (_backend) {
            _backend->stop(); // before the state it renders from goes away
        }
        _backend = std::move(other._backend);
        _state = std::move(other._state);
    }
    return *this;
}

void AudioEngine::update(f32 dt) {
    State& s = *_state;
    s.poll_loading();
    {
        const std::scoped_lock lock{s.mutex};
        for (State::Voice& voice : s.voices) {
            voice.age += std::max(0.0f, dt);
        }
    }
    if (_backend->available()) {
        return;
    }

    // Nothing consumes audio in real time, so mix the elapsed time here;
    // otherwise voices would never finish, and one-shots would pile up until
    // every voice and instance limit was taken.
    s.pending_frames += static_cast<f64>(std::max(0.0f, dt)) * static_cast<f64>(s.sample_rate);
    i64 frames = static_cast<i64>(s.pending_frames);
    s.pending_frames -= static_cast<f64>(frames);
    s.scratch.resize(static_cast<std::size_t>(render_chunk_frames * s.channels));
    while (frames > 0) {
        const i32 chunk = static_cast<i32>(std::min<i64>(frames, render_chunk_frames));
        render(std::span<f32>{s.scratch.data(), static_cast<std::size_t>(chunk * s.channels)});
        frames -= chunk;
    }
}

void AudioEngine::render(std::span<f32> interleaved) {
    const std::scoped_lock lock{_state->mutex};
    _state->mix(interleaved);
}

void AudioEngine::add_clip(std::string_view clip_id, AudioClip clip) {
    _state->memory_clips.insert_or_assign(std::string{clip_id}, std::make_shared<const AudioClip>(std::move(clip)));
}

void AudioEngine::preload(const AudioCatalog& catalog) {
    for (const auto& [id, clip] : catalog.clips()) {
        _state->load_clip(catalog, id);
    }
}

void AudioEngine::preload_async(const AudioCatalog& catalog, JobSystem& jobs) {
    State& s = *_state;
    for (const auto& [id, clip] : catalog.clips()) {
        if (s.memory_clips.contains(id)) {
            continue;
        }
        std::filesystem::path path = catalog.resolve_clip_path(id);
        const std::string key = State::cache_key(path, clip.stream);
        if (s.clip_cache.contains(key) || s.loading.contains(key)) {
            continue;
        }
        s.loading.emplace(key, jobs.run([path = std::move(path), stream = clip.stream] {
            return State::load_file(path, stream);
        }));
    }
}

AudioHandle AudioEngine::play(const AudioCatalog& catalog, const AudioPlayRequest& request) {
    State& s = *_state;
    const AudioCue* cue = catalog.cue(request.cue);
    if (!cue || cue->clips.empty()) {
        {
            const std::scoped_lock lock{s.mutex};
            ++s.stats.culled_requests;
        }
        KIN_LOG_WARN_F("audio",
                       "audio play rejected",
                       (LogFields{
                           {.name = "cue", .value = std::string{request.cue}},
                           {.name = "reason", .value = cue ? "cue has no clips" : "missing cue"},
                       }));
        return {};
    }

    // Variation is drawn for every request, played or not, so which requests
    // get culled never changes what the others sound like.
    const std::string& clip_id = s.choose_clip(*cue);
    const f32 variation = cue->pitch_variance > 0.0f
        ? rng_f32(s.next_key(), -cue->pitch_variance, cue->pitch_variance)
        : 0.0f;

    // Decoding (when the clip is not loaded yet) happens before taking the lock,
    // so the audio thread never waits on it.
    const std::shared_ptr<const AudioClip> clip = s.load_clip(catalog, clip_id);
    std::unique_ptr<AudioDecoder> stream;
    if (clip && clip->streamed()) {
        try {
            stream = clip->open_stream();
        } catch (const std::exception& error) {
            KIN_LOG_ERROR_F("audio",
                            "audio stream open failed",
                            (LogFields{
                                {.name = "clip", .value = clip_id},
                                {.name = "error", .value = error.what()},
                            }));
        }
    }
    const std::scoped_lock lock{s.mutex};
    if (!clip || (clip->streamed() && !stream)) {
        ++s.stats.culled_requests;
        KIN_LOG_WARN_F("audio",
                       "audio play rejected",
                       (LogFields{
                           {.name = "cue", .value = cue->id},
                           {.name = "clip", .value = clip_id},
                           {.name = "reason", .value = "clip unavailable"},
                       }));
        return {};
    }

    s.sync_ducks(catalog);
    i32 steal = -1;
    if (!s.allocate_voice(*cue, request, steal)) {
        // Culling is the voice limits doing their job, and a busy game does it
        // every frame: count it (stats().culled_requests), log only at debug.
        ++s.stats.culled_requests;
        KIN_LOG_DEBUG_F("audio",
                        "audio play rejected",
                        (LogFields{
                            {.name = "cue", .value = std::string{request.cue}},
                            {.name = "category", .value = std::string{audio_category_name(cue->category)}},
                            {.name = "reason", .value = "voice limit or priority"},
                        }));
        return {};
    }
    if (steal >= 0) {
        // The stolen voice fades out over a few milliseconds beside the new one.
        s.stop_voice(s.voices[static_cast<std::size_t>(steal)], 0.0f);
        ++s.stats.stolen_voices;
    }

    State::Voice voice{
        .handle = AudioHandle{s.next_handle++},
        .cue_id = cue->id,
        .bus = s.sync_bus(catalog, cue->bus),
        .category = cue->category,
        .clip = clip,
        .rate_ratio = static_cast<f64>(clip->sample_rate()) / static_cast<f64>(s.sample_rate),
        .stream = std::move(stream),
        .volume = std::max(0.0f, cue->volume * request.volume),
        .cue_volume = std::max(0.0f, cue->volume),
        .volume_target = std::max(0.0f, cue->volume * request.volume),
        .base_pitch = std::max(0.01f, cue->pitch * (1.0f + variation)),
        .pitch = std::max(0.01f, cue->pitch * request.pitch * (1.0f + variation)),
        .priority = cue->priority + request.priority_boost,
        .loop = cue->loop,
        .spatial = cue->spatial,
        .min_distance = cue->min_distance,
        .max_distance = cue->max_distance,
        .position = request.position,
        .has_position = request.has_position,
    };
    if (const AudioClipRef* ref = catalog.clip(clip_id)) {
        voice.loop_start = ref->loop_start;
        voice.loop_end = ref->loop_end;
    }
    if (voice.stream) {
        voice.stream_buffer.resize(static_cast<std::size_t>(stream_buffer_frames * clip->channels()));
    }
    if (request.fade_in > 0.0f) {
        voice.fade = 0.0f;
        s.start_fade(voice, 1.0f, request.fade_in);
    }
    const AudioHandle handle = voice.handle;
    s.voices.push_back(std::move(voice));
    ++s.stats.played_requests;
    s.stats.active_voices = static_cast<i32>(std::ranges::count_if(s.voices, [](const State::Voice& v) {
        return !v.stopping;
    }));
    KIN_LOG_DEBUG_F("audio",
                    "audio cue playing",
                    (LogFields{
                        {.name = "cue", .value = cue->id},
                        {.name = "clip", .value = clip_id},
                        {.name = "bus", .value = cue->bus},
                        {.name = "category", .value = std::string{audio_category_name(cue->category)}},
                    }));
    return handle;
}

void AudioEngine::stop(AudioHandle handle, f32 fade) {
    if (!handle) {
        return;
    }
    const std::scoped_lock lock{_state->mutex};
    for (State::Voice& voice : _state->voices) {
        if (voice.handle == handle) {
            _state->stop_voice(voice, fade);
        }
    }
}

void AudioEngine::stop_bus(std::string_view bus, f32 fade) {
    State& s = *_state;
    const std::scoped_lock lock{s.mutex};
    const auto found = s.bus_index.find(std::string{bus});
    if (found == s.bus_index.end()) {
        return;
    }
    for (State::Voice& voice : s.voices) {
        if (s.bus_under(voice.bus, found->second)) {
            s.stop_voice(voice, fade);
        }
    }
}

void AudioEngine::set_bus_volume(std::string_view bus, f32 volume, f32 fade) {
    State& s = *_state;
    const f32 target = std::max(0.0f, volume);
    {
        const std::scoped_lock lock{s.mutex};
        State::Bus& state = s.buses[static_cast<std::size_t>(s.ensure_bus(bus))];
        state.target = target;
        state.step = fade > 0.0f ? (target - state.volume) / static_cast<f32>(s.frames_for(fade)) : 0.0f;
        if (state.step == 0.0f) {
            state.volume = target;
        }
    }
    KIN_LOG_INFO_F("audio",
                   "audio bus volume set",
                   (LogFields{
                       {.name = "bus", .value = std::string{bus}},
                       {.name = "volume", .value = std::to_string(target)},
                       {.name = "fade", .value = std::to_string(std::max(0.0f, fade))},
                   }));
}

void AudioEngine::set_bus_muted(std::string_view bus, bool muted) {
    const std::scoped_lock lock{_state->mutex};
    _state->buses[static_cast<std::size_t>(_state->ensure_bus(bus))].muted = muted;
}

void AudioEngine::set_bus_paused(std::string_view bus, bool paused) {
    const std::scoped_lock lock{_state->mutex};
    _state->buses[static_cast<std::size_t>(_state->ensure_bus(bus))].paused = paused;
}

void AudioEngine::set_listener(Vec2f position) {
    const std::scoped_lock lock{_state->mutex};
    _state->listener = position;
}

void AudioEngine::set_position(AudioHandle handle, Vec2f position) {
    const std::scoped_lock lock{_state->mutex};
    for (State::Voice& voice : _state->voices) {
        if (voice.handle == handle) {
            voice.position = position;
            voice.has_position = true;
            return;
        }
    }
}

void AudioEngine::set_volume(AudioHandle handle, f32 volume, f32 fade) {
    const std::scoped_lock lock{_state->mutex};
    for (State::Voice& voice : _state->voices) {
        if (voice.handle != handle) {
            continue;
        }
        // Relative to the cue's volume, as AudioPlayRequest::volume is.
        voice.volume_target = std::max(0.0f, voice.cue_volume * volume);
        if (fade > 0.0f) {
            voice.volume_frames = _state->frames_for(fade);
            voice.volume_step = (voice.volume_target - voice.volume) / static_cast<f32>(voice.volume_frames);
        } else {
            voice.volume = voice.volume_target;
            voice.volume_frames = 0;
        }
    }
}

void AudioEngine::set_pitch(AudioHandle handle, f32 pitch) {
    const std::scoped_lock lock{_state->mutex};
    for (State::Voice& voice : _state->voices) {
        if (voice.handle == handle) {
            voice.pitch = std::max(0.01f, voice.base_pitch * pitch);
        }
    }
}

AudioHandle AudioEngine::play_music(const AudioCatalog& catalog, std::string_view cue, f32 crossfade) {
    State& s = *_state;
    if (s.music && s.music_cue == cue && playing(s.music)) {
        return s.music;
    }
    if (s.music) {
        stop(s.music, crossfade);
    }
    s.music = play(catalog, {.cue = std::string{cue}, .fade_in = crossfade});
    s.music_cue = s.music ? std::string{cue} : std::string{};
    return s.music;
}

void AudioEngine::stop_music(f32 fade) {
    State& s = *_state;
    stop(s.music, fade);
    s.music = {};
    s.music_cue.clear();
}

AudioHandle AudioEngine::music() const {
    return playing(_state->music) ? _state->music : AudioHandle{};
}

f32 AudioEngine::playback_position(AudioHandle handle) const {
    const std::scoped_lock lock{_state->mutex};
    for (const State::Voice& voice : _state->voices) {
        if (voice.handle == handle && !voice.stopping && voice.clip) {
            // f0, the frame the output is leaving, is two behind the next read.
            const f64 frame = voice.primed ? static_cast<f64>(voice.next_frame - (voice.at_end ? 1 : 2)) + voice.frac : 0.0;
            return static_cast<f32>(std::max(0.0, frame) / static_cast<f64>(voice.clip->sample_rate()));
        }
    }
    return 0.0f;
}

bool AudioEngine::playing(AudioHandle handle) const {
    const std::scoped_lock lock{_state->mutex};
    return std::ranges::any_of(_state->voices, [handle](const State::Voice& voice) {
        return voice.handle == handle && !voice.stopping;
    });
}

i32 AudioEngine::active_voice_count() const {
    const std::scoped_lock lock{_state->mutex};
    return static_cast<i32>(std::ranges::count_if(_state->voices, [](const State::Voice& voice) {
        return !voice.stopping;
    }));
}

i32 AudioEngine::active_voice_count(AudioCategory category) const {
    const std::scoped_lock lock{_state->mutex};
    return _state->counted_voices(category);
}

f32 AudioEngine::bus_volume(std::string_view bus) const {
    const std::scoped_lock lock{_state->mutex};
    const State::Bus* state = _state->find_bus(bus);
    return state ? state->target : 1.0f;
}

bool AudioEngine::bus_muted(std::string_view bus) const {
    const std::scoped_lock lock{_state->mutex};
    const State::Bus* state = _state->find_bus(bus);
    return state && state->muted;
}

bool AudioEngine::bus_paused(std::string_view bus) const {
    const std::scoped_lock lock{_state->mutex};
    const State::Bus* state = _state->find_bus(bus);
    return state && state->paused;
}

f32 AudioEngine::effective_bus_volume(const AudioCatalog& catalog, std::string_view bus) const {
    const std::scoped_lock lock{_state->mutex};
    f32 volume = 1.0f;
    std::string current{bus};
    for (i32 guard = 0; !current.empty() && guard < 32; ++guard) {
        const AudioBus* authored = catalog.bus(current);
        const State::Bus* state = _state->find_bus(current);
        if ((authored && authored->muted) || (state && state->muted)) {
            return 0.0f;
        }
        volume *= (authored ? authored->volume : 1.0f) * (state ? state->target : 1.0f);
        if (!authored) {
            break;
        }
        current = authored->parent;
    }
    return volume;
}

std::vector<AudioBusState> AudioEngine::bus_states() const {
    const std::scoped_lock lock{_state->mutex};
    std::vector<AudioBusState> states;
    states.reserve(_state->buses.size());
    for (const State::Bus& bus : _state->buses) {
        states.push_back({.id = bus.id, .volume = bus.target, .muted = bus.muted, .paused = bus.paused});
    }
    return states;
}

i32 AudioEngine::sample_rate() const {
    return _state->sample_rate;
}

i32 AudioEngine::channels() const {
    return _state->channels;
}

bool AudioEngine::device_available() const {
    return _backend->available();
}

AudioEngineStats AudioEngine::stats() const {
    const std::scoped_lock lock{_state->mutex};
    return _state->stats;
}

void AudioEngine::reset_stats() {
    const std::scoped_lock lock{_state->mutex};
    _state->stats = {};
}

void AudioEngine::write_report(JsonWriter& json) const {
    const AudioHandle current_music = music();
    const std::scoped_lock lock{_state->mutex};
    const State& s = *_state;
    json.begin_object();
    json.field("device", _backend->available());
    json.field("sample_rate", s.sample_rate);
    json.key("stats").begin_object();
    json.field("active_voices", s.stats.active_voices);
    json.field("played_requests", s.stats.played_requests);
    json.field("culled_requests", s.stats.culled_requests);
    json.field("stolen_voices", s.stats.stolen_voices);
    json.field("mixed_frames", s.stats.mixed_frames);
    json.field("limited_frames", s.stats.limited_frames);
    json.end_object();
    json.key("music");
    if (current_music) {
        json.value(s.music_cue);
    } else {
        json.value_null();
    }
    json.key("buses").begin_array();
    for (const State::Bus& bus : s.buses) {
        json.begin_object();
        json.field("id", bus.id);
        json.field("volume", static_cast<f64>(bus.target));
        json.field("gain", static_cast<f64>(bus.gain));
        json.field("duck", static_cast<f64>(bus.duck));
        json.field("muted", bus.muted);
        json.field("paused", bus.paused);
        json.end_object();
    }
    json.end_array();
    json.key("voices").begin_array();
    for (const State::Voice& voice : s.voices) {
        json.begin_object();
        json.field("handle", voice.handle.id);
        json.field("cue", voice.cue_id);
        json.field("bus", voice.bus == no_bus ? std::string_view{} : std::string_view{s.buses[static_cast<std::size_t>(voice.bus)].id});
        json.field("category", audio_category_name(voice.category));
        json.field("volume", static_cast<f64>(voice.volume));
        json.field("stopping", voice.stopping);
        json.end_object();
    }
    json.end_array();
    json.end_object();
}

} // namespace kin
