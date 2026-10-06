#include <kin/audio/audio_engine.hpp>

#include <kin/audio/audio_decoder.hpp>
#include <kin/assets/file_watcher.hpp>

#include "audio_analysis.hpp"
#include "audio_effects.hpp"

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
// How quickly level meters fall back after a peak, and average loudness.
constexpr f32 meter_seconds = 0.3f;
// The mixer works in blocks of at most this many frames, so each bus's buffer
// is allocated once and parameter changes land within ~20 ms.
constexpr i32 block_frames = 1024;
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
    // A level meter, and (when enabled) the recent frames for a spectrum.
    struct Meter {
        f32 peak = 0.0f;
        f32 mean_square = 0.0f;
        std::vector<f32> history; // mono; empty when analysis is off
        std::size_t history_pos = 0;
    };

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
        // Resolved at the start of each mixed block.
        f32 duck = 1.0f; // from the duck rules aimed at this bus
        f32 own_gain = 1.0f; // this bus's level alone, applied as it adds into its parent
        f32 gain = 1.0f; // through all its parents (for reports)
        bool paused_now = false;
        i32 depth = 0;
        // The bus's submix: its voices and child buses, then its effects.
        std::vector<f32> buffer;
        bool active = false; // anything reached the buffer this block
        f32 applied_gain = -1.0f; // own_gain at the end of the last block; <0 before the first
        std::vector<std::unique_ptr<AudioEffectProcessor>> effects;
        std::vector<AudioEffect> effect_source; // the catalog chain `effects` came from
        bool effects_from_game = false; // set_bus_effects wins over the catalog
        Meter meter;
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
        AudioRolloff rolloff = AudioRolloff::Smooth;
        f32 rolloff_power = 1.0f;
        f32 pan_strength = 1.0f;
        Vec2f position{};
        bool has_position = false;
        bool paused = false;
        // Output frames to start and stop on (scheduled music and stingers):
        // start_at in the past means now; stop_at -1 means not scheduled.
        i64 start_at = 0;
        i64 stop_at = -1;
        f32 stop_fade = 0.0f;
        // A seek waiting for the voice to fade out (-1: none), and the fade to
        // return to after the jump.
        i64 seek_frame = -1;
        f32 seek_restore = 1.0f;
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
        retired_clips.reserve(retired_capacity);
        retired_streams.reserve(retired_capacity);
    }

    AudioEngineConfig config;
    i32 sample_rate = 48000;
    i32 channels = 2;

    // Shared with the audio thread: only touched with `mutex` held.
    mutable std::mutex mutex;
    std::vector<Voice> voices;
    std::vector<Bus> buses;
    std::vector<i32> bus_order; // deepest buses first, so children mix before parents
    std::unordered_map<std::string, i32> bus_index;
    std::vector<AudioDuck> duck_source; // the catalog rules `ducks` was built from
    std::vector<Duck> ducks;
    Vec2f listener{};
    AudioEngineStats stats;
    f32 limiter_env = 0.0f;
    f32 limiter_release = 0.0f;
    Meter output_meter;
    i64 output_frame = 0; // frames mixed so far: the clock scheduled voices start on
    // What finished voices held, handed back for the game thread to free (in
    // update()), so the audio thread never frees a clip's samples. Reserved up
    // front; past capacity the audio thread frees them itself.
    static constexpr std::size_t retired_capacity = 256;
    std::vector<std::shared_ptr<const AudioClip>> retired_clips;
    std::vector<std::unique_ptr<AudioDecoder>> retired_streams;

    // Game thread only.
    u64 next_handle = 1;
    RngKey rng;
    std::unordered_map<std::string, std::shared_ptr<const AudioClip>> clip_cache;
    std::unordered_map<std::string, std::shared_ptr<const AudioClip>> memory_clips;
    std::unordered_map<std::string, Job<AudioClip>> loading;
    std::unordered_map<std::string, std::size_t> last_clip; // per cue, so variations do not repeat
    f64 pending_frames = 0.0;
    std::vector<f32> scratch;
    // The music (play_music): its cue, the voice of each layer (one for a cue
    // without layers; for a playlist, the clip playing now) and, for a
    // playlist, the next clip, scheduled to start where this one ends.
    struct MusicLayer {
        std::string name; // the layer's clip id; "" for a cue without layers
        AudioHandle handle;
    };
    struct Music {
        const AudioCatalog* catalog = nullptr;
        AudioCue cue;
        std::vector<MusicLayer> layers;
        std::size_t index = 0; // playlist: the clip playing now
        AudioHandle next;
        std::size_t next_index = 0;
        bool finished = false; // playlist: reached the end of a list that does not loop
    };
    Music music;
    std::unordered_map<std::string, f32> layer_volumes; // set_music_layer, kept across cues
    // Files watched for changes (watch(), watch_catalog()).
    struct WatchRef {
        FileWatcher* files = nullptr;
        std::weak_ptr<const void> alive;
        u32 id = 0;
    };
    FileWatcher* clip_files = nullptr; // set by watch(): clip files are watched as they load
    std::weak_ptr<const void> clip_files_alive;
    std::unordered_map<std::string, WatchRef> clip_watches; // by cache key
    std::vector<WatchRef> catalog_watches;
    f32 device_poll = 0.0f; // seconds since the backend last checked its device

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
        buses.back().buffer.assign(static_cast<std::size_t>(block_frames * channels), 0.0f);
        bus_order.push_back(index);
        bus_index.emplace(std::string{id}, index);
        return index;
    }

    std::vector<std::unique_ptr<AudioEffectProcessor>> make_effects(const std::vector<AudioEffect>& chain) const {
        std::vector<std::unique_ptr<AudioEffectProcessor>> made;
        for (const AudioEffect& effect : chain) {
            if (auto processor = make_audio_effect(effect, sample_rate)) {
                made.push_back(std::move(processor));
            }
        }
        return made;
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
            if (!bus.effects_from_game && bus.effect_source != authored->effects) {
                bus.effects = make_effects(authored->effects);
                bus.effect_source = authored->effects;
            }
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
            bus.depth = 0;
            const Bus* level = &bus;
            for (i32 guard = 0; level && guard < 32; ++guard) {
                bus.paused_now = bus.paused_now || level->paused;
                level = level->parent == no_bus ? nullptr : &buses[static_cast<std::size_t>(level->parent)];
                bus.depth += level ? 1 : 0;
            }
        }
        std::ranges::sort(bus_order, [&](i32 a, i32 b) {
            return buses[static_cast<std::size_t>(a)].depth > buses[static_cast<std::size_t>(b)].depth;
        });

        for (Duck& duck : ducks) {
            const bool active = std::ranges::any_of(voices, [&](const Voice& voice) {
                return !voice.stopping && !voice.done && !voice.paused && voice.start_at <= output_frame &&
                       voice.bus != no_bus &&
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
            bus.own_gain = bus.muted || bus.authored_muted ? 0.0f : bus.authored * bus.volume * bus.duck;
        }
        for (Bus& bus : buses) {
            f32 gain = 1.0f;
            const Bus* level = &bus;
            for (i32 guard = 0; level && guard < 32; ++guard) {
                gain *= level->own_gain;
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

    static void prime(Voice& voice) {
        voice.primed = true;
        if (!read_frame(voice, voice.f0)) {
            voice.done = true;
            return;
        }
        if (!read_frame(voice, voice.f1)) {
            voice.f1[0] = voice.f1[1] = 0.0f;
            voice.at_end = true;
        }
    }

    // Moves a voice's read position to `frame`; it primes on its next frame.
    static bool jump(Voice& voice, i64 frame) {
        if (voice.stream) {
            if (!voice.stream->seek(frame)) {
                return false;
            }
            voice.stream_pos = voice.stream_count = 0;
        }
        voice.next_frame = frame;
        voice.frac = 0.0;
        voice.primed = false;
        voice.at_end = false;
        return true;
    }

    // Mixes `frames` frames of a voice, starting at frame `first` of the block
    // (a scheduled start), and stops it at `stop_offset` if that is in the block.
    void mix_voice(Voice& voice, std::span<f32> out, i32 frames, PanGains target, i32 first = 0, i32 stop_offset = -1) {
        if (!voice.primed) {
            prime(voice);
            if (voice.done) {
                return;
            }
        }
        if (voice.fresh) {
            voice.fresh = false;
            voice.gain_left = target.left;
            voice.gain_right = target.right;
        }

        const f32 ramp_left = (target.left - voice.gain_left) / static_cast<f32>(frames - first);
        const f32 ramp_right = (target.right - voice.gain_right) / static_cast<f32>(frames - first);
        const f64 step = static_cast<f64>(std::max(0.01f, voice.pitch)) * voice.rate_ratio;
        const std::size_t stride = static_cast<std::size_t>(channels);
        for (i32 frame = first; frame < frames && !voice.done; ++frame) {
            if (frame == stop_offset) {
                voice.stop_at = -1;
                stop_voice(voice, voice.stop_fade);
            }
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
            if (voice.seek_frame >= 0 && voice.fade_frames == 0) {
                // Faded out: jump, and fade back in from the new place.
                jump(voice, voice.seek_frame);
                voice.seek_frame = -1;
                start_fade(voice, voice.seek_restore, declick_seconds);
                prime(voice);
                continue;
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

    // Folds a block (what a bus sent to its parent, or the output) into a meter;
    // `block` null measures silence.
    void measure(Meter& meter, const f32* block, i32 frames) {
        const std::size_t stride = static_cast<std::size_t>(channels);
        f32 peak = 0.0f;
        f32 sum = 0.0f;
        const bool record = !meter.history.empty();
        for (i32 frame = 0; frame < frames; ++frame) {
            f32 mono = 0.0f;
            for (std::size_t c = 0; c < stride && block; ++c) {
                const f32 v = block[static_cast<std::size_t>(frame) * stride + c];
                peak = std::max(peak, std::fabs(v));
                sum += v * v;
                mono += v;
            }
            if (record) {
                meter.history[meter.history_pos] = mono / static_cast<f32>(stride);
                meter.history_pos = (meter.history_pos + 1) % meter.history.size();
            }
        }
        const f32 keep = std::exp(-static_cast<f32>(frames) / (meter_seconds * static_cast<f32>(sample_rate)));
        const f32 mean_square = sum / static_cast<f32>(static_cast<std::size_t>(frames) * stride);
        meter.peak = std::max(peak, meter.peak * keep);
        meter.mean_square = mean_square + (meter.mean_square - mean_square) * keep;
    }

    void mix(std::span<f32> out) {
        const std::size_t stride = static_cast<std::size_t>(channels);
        const i32 total = static_cast<i32>(out.size() / stride);
        for (i32 at = 0; at < total; at += block_frames) {
            const i32 frames = std::min(block_frames, total - at);
            mix_block(out.subspan(static_cast<std::size_t>(at) * stride, static_cast<std::size_t>(frames) * stride), frames);
        }
        for (Voice& voice : voices) {
            if (!voice.done) {
                continue;
            }
            if (voice.clip && retired_clips.size() < retired_capacity) {
                retired_clips.push_back(std::move(voice.clip));
            }
            if (voice.stream && retired_streams.size() < retired_capacity) {
                retired_streams.push_back(std::move(voice.stream));
            }
        }
        std::erase_if(voices, [](const Voice& voice) { return voice.done; });
        stats.active_voices = static_cast<i32>(std::ranges::count_if(voices, [](const Voice& voice) {
            return !voice.stopping;
        }));
    }

    void mix_block(std::span<f32> out, i32 frames) {
        const std::size_t samples = static_cast<std::size_t>(frames * channels);
        std::ranges::fill(out, 0.0f);
        for (Bus& bus : buses) {
            std::fill_n(bus.buffer.begin(), samples, 0.0f);
            bus.active = !bus.effects.empty(); // effects can ring on after their input stops
        }
        advance_buses(frames);

        const i64 block_end = output_frame + frames;
        for (Voice& voice : voices) {
            if (voice.done) {
                continue;
            }
            if (!voice.clip || !voice.clip->valid()) {
                voice.done = true;
                continue;
            }
            if (voice.start_at >= block_end) {
                if (voice.stopping) {
                    voice.done = true; // stopped before it started
                }
                continue; // scheduled for a later block
            }
            const i32 first = static_cast<i32>(std::max<i64>(0, voice.start_at - output_frame));
            i32 stop_offset = -1;
            if (voice.stop_at >= 0 && !voice.stopping && voice.stop_at < block_end) {
                stop_offset = static_cast<i32>(std::max<i64>(first, voice.stop_at - output_frame));
            }
            Bus* bus = voice.bus == no_bus ? nullptr : &buses[static_cast<std::size_t>(voice.bus)];
            const bool paused = voice.paused || (bus && bus->paused_now);
            if (paused && (voice.fresh || (voice.gain_left == 0.0f && voice.gain_right == 0.0f))) {
                continue; // silent and holding its place
            }

            if (voice.volume_frames > 0) {
                const i32 n = std::min(frames, voice.volume_frames);
                voice.volume_frames -= n;
                voice.volume = voice.volume_frames == 0 ? voice.volume_target
                                                        : voice.volume + voice.volume_step * static_cast<f32>(n);
            }
            f32 gain = paused ? 0.0f : voice.volume;
            PanGains pan{};
            if (voice.spatial && voice.has_position) {
                const SpatialAudioResult spatial = calculate_spatial_audio(listener, voice.position, {
                    .min_distance = voice.min_distance,
                    .max_distance = voice.max_distance,
                    .pan_strength = voice.pan_strength,
                    .rolloff = voice.rolloff,
                    .rolloff_power = voice.rolloff_power,
                });
                gain *= spatial.gain;
                pan = pan_gains(spatial.pan);
            }
            gain = std::max(0.0f, gain);
            std::span<f32> dest = out;
            if (bus) {
                bus->active = true;
                dest = std::span<f32>{bus->buffer.data(), samples};
            }
            mix_voice(voice, dest, frames, {.left = gain * pan.left, .right = gain * pan.right}, first, stop_offset);
        }

        // Each bus runs its effects over its submix, then adds it into its
        // parent at its own volume, ramped across the block.
        for (const i32 index : bus_order) {
            Bus& bus = buses[static_cast<std::size_t>(index)];
            const f32 from = bus.applied_gain < 0.0f ? bus.own_gain : bus.applied_gain;
            bus.applied_gain = bus.own_gain;
            if (!bus.active) {
                measure(bus.meter, nullptr, frames);
                continue;
            }
            const std::span<f32> block{bus.buffer.data(), samples};
            for (const auto& effect : bus.effects) {
                effect->process(block, channels);
            }
            f32* dest = out.data();
            if (bus.parent != no_bus) {
                Bus& parent = buses[static_cast<std::size_t>(bus.parent)];
                parent.active = true;
                dest = parent.buffer.data();
            }
            const f32 ramp = (bus.own_gain - from) / static_cast<f32>(frames);
            const std::size_t stride = static_cast<std::size_t>(channels);
            for (i32 frame = 0; frame < frames; ++frame) {
                const f32 gain = from + ramp * static_cast<f32>(frame);
                for (std::size_t c = 0; c < stride; ++c) {
                    const std::size_t i = static_cast<std::size_t>(frame) * stride + c;
                    block[i] *= gain; // what the bus sends, for its meter
                    dest[i] += block[i];
                }
            }
            measure(bus.meter, block.data(), frames);
        }

        limit(out, frames);
        measure(output_meter, out.data(), frames);
        output_frame += frames;
        stats.mixed_frames += frames;
    }

    // --- music timing (mutex held) ---

    Voice* find_voice(AudioHandle handle) {
        for (Voice& voice : voices) {
            if (voice.handle == handle && !voice.done) {
                return &voice;
            }
        }
        return nullptr;
    }

    // The voice that keeps the music's time: its first layer still playing.
    const Voice* music_voice() {
        for (const MusicLayer& layer : music.layers) {
            if (const Voice* voice = find_voice(layer.handle); voice && !voice->stopping) {
                return voice;
            }
        }
        return nullptr;
    }

    // Where in its clip (in source frames) a voice is at the start of the next
    // block, or will start.
    static f64 source_position(const Voice& voice) {
        if (!voice.primed) {
            return static_cast<f64>(voice.next_frame);
        }
        return static_cast<f64>(voice.next_frame - (voice.at_end ? 1 : 2)) + voice.frac;
    }

    static f64 source_end(const Voice& voice) {
        const f64 frames = voice.clip->frame_count();
        return voice.loop && voice.loop_end > 0 ? std::min(static_cast<f64>(voice.loop_end), frames) : frames;
    }

    // The output frame of the music's next beat, bar or clip end (now, for
    // AudioSync::Now, without music, or for a beat without a tempo).
    i64 sync_frame(AudioSync sync) {
        const Voice* voice = sync == AudioSync::Now ? nullptr : music_voice();
        if (!voice) {
            return output_frame;
        }
        const i64 base = std::max(output_frame, voice->start_at);
        const f64 position = source_position(*voice);
        const f64 rate = voice->clip->sample_rate();
        f64 target = source_end(*voice);
        if (sync == AudioSync::Beat || sync == AudioSync::Bar) {
            if (music.cue.bpm <= 0.0f) {
                return output_frame;
            }
            const f64 unit = 60.0 / music.cue.bpm * rate * (sync == AudioSync::Bar ? std::max(1, music.cue.beats_per_bar) : 1);
            const f64 origin = music.cue.beat_offset * rate;
            const f64 next = std::floor((position - origin) / unit + 1e-9) + 1.0;
            target = std::min(target, origin + next * unit);
        }
        const f64 speed = voice->rate_ratio * std::max(0.01f, voice->pitch);
        return base + static_cast<i64>(std::ceil(std::max(0.0, target - position) / speed));
    }

    // Seconds into its clip the music will be at output frame `at`.
    f64 music_seconds_at(i64 at) {
        const Voice* voice = music_voice();
        if (!voice) {
            return 0.0;
        }
        const f64 speed = voice->rate_ratio * std::max(0.01f, voice->pitch);
        f64 position = source_position(*voice) + static_cast<f64>(std::max<i64>(0, at - std::max(output_frame, voice->start_at))) * speed;
        const f64 end = source_end(*voice);
        if (position >= end) {
            position = voice->loop ? static_cast<f64>(voice->loop_start) + (position - end) : end;
        }
        return position / voice->clip->sample_rate();
    }

    void stop_music_voices(i64 at, f32 fade) {
        std::vector<AudioHandle> handles;
        for (const MusicLayer& layer : music.layers) {
            handles.push_back(layer.handle);
        }
        handles.push_back(music.next);
        for (const AudioHandle handle : handles) {
            if (Voice* voice = find_voice(handle)) {
                if (at <= output_frame) {
                    stop_voice(*voice, fade);
                } else {
                    voice->stop_at = at;
                    voice->stop_fade = fade;
                }
            }
        }
    }

    f32 layer_volume(const std::string& name) const {
        const auto found = layer_volumes.find(name);
        return found == layer_volumes.end() ? 1.0f : found->second;
    }

    // Schedules a playlist's next clip where the current one ends, once that
    // is less than two seconds away, and moves on when it starts.
    void advance_playlist() {
        if (!music.catalog || !music.cue.playlist || music.finished || music.layers.empty()) {
            return;
        }
        {
            const std::scoped_lock lock{mutex};
            const Voice* current = find_voice(music.layers.front().handle);
            if (!current || current->stopping) {
                if (!music.next || !find_voice(music.next)) {
                    music.finished = true; // stopped, or the list ran out
                    return;
                }
                music.layers.front().handle = music.next;
                music.index = music.next_index;
                music.next = {};
            }
            if (music.next || sync_frame(AudioSync::End) - output_frame > 2 * sample_rate) {
                return;
            }
        }
        const std::size_t count = music.cue.clips.size();
        std::size_t next = music.index + 1;
        if (music.cue.shuffle && count > 1) {
            next = static_cast<std::size_t>(rng_i32(next_key(), 0, static_cast<i32>(count) - 2));
            next += next >= music.index ? 1 : 0;
        } else if (next >= count) {
            if (!music.cue.loop) {
                music.finished = true;
                return;
            }
            next = 0;
        }
        Prepared part;
        if (!prepare(*music.catalog, music.cue, music.cue.clips[next], part)) {
            music.finished = true;
            return;
        }
        part.cue = &music.cue;
        part.loop = false;
        const std::scoped_lock lock{mutex};
        const i64 start = sync_frame(AudioSync::End);
        music.next = commit(*music.catalog, std::move(part), {.cue = music.cue.id, .volume = layer_volume("")}, start, 0.0);
        music.next_index = next;
    }

    // --- playing (game thread) ---

    const AudioCue* find_cue(const AudioCatalog& catalog, std::string_view id) {
        const AudioCue* cue = catalog.cue(id);
        if (!cue || cue->clips.empty()) {
            {
                const std::scoped_lock lock{mutex};
                ++stats.culled_requests;
            }
            KIN_LOG_WARN_F("audio",
                           "audio play rejected",
                           (LogFields{
                               {.name = "cue", .value = std::string{id}},
                               {.name = "reason", .value = cue ? "cue has no clips" : "missing cue"},
                           }));
            return nullptr;
        }
        return cue;
    }

    // A voice about to be added: its clip loaded and its stream opened, which
    // happens before taking the lock so the audio thread never waits on it.
    struct Prepared {
        const AudioCue* cue = nullptr;
        std::string clip_id;
        std::shared_ptr<const AudioClip> clip;
        std::unique_ptr<AudioDecoder> stream;
        f32 variation = 0.0f;
        bool loop = false;
    };

    bool prepare(const AudioCatalog& catalog, const AudioCue& cue, const std::string& clip_id, Prepared& out) {
        // Variation is drawn for every request, played or not, so which requests
        // get culled never changes what the others sound like.
        out.cue = &cue;
        out.clip_id = clip_id;
        out.loop = cue.loop;
        out.variation = cue.pitch_variance > 0.0f ? rng_f32(next_key(), -cue.pitch_variance, cue.pitch_variance) : 0.0f;
        out.clip = load_clip(catalog, clip_id);
        if (out.clip && out.clip->streamed()) {
            try {
                out.stream = out.clip->open_stream();
            } catch (const std::exception& error) {
                KIN_LOG_ERROR_F("audio",
                                "audio stream open failed",
                                (LogFields{
                                    {.name = "clip", .value = clip_id},
                                    {.name = "error", .value = error.what()},
                                }));
            }
        }
        if (!out.clip || (out.clip->streamed() && !out.stream)) {
            const std::scoped_lock lock{mutex};
            ++stats.culled_requests;
            KIN_LOG_WARN_F("audio",
                           "audio play rejected",
                           (LogFields{
                               {.name = "cue", .value = cue.id},
                               {.name = "clip", .value = clip_id},
                               {.name = "reason", .value = "clip unavailable"},
                           }));
            return false;
        }
        return true;
    }

    // Adds a prepared voice (mutex held). It starts on output frame
    // `start_at` (now if that has passed), `start_seconds` into its clip.
    AudioHandle commit(const AudioCatalog& catalog, Prepared part, const AudioPlayRequest& request, i64 start_at,
                       f64 start_seconds) {
        const AudioCue& cue = *part.cue;
        sync_ducks(catalog);
        i32 steal = -1;
        if (!allocate_voice(cue, request, steal)) {
            // Culling is the voice limits doing their job, and a busy game does it
            // every frame: count it (stats().culled_requests), log only at debug.
            ++stats.culled_requests;
            KIN_LOG_DEBUG_F("audio",
                            "audio play rejected",
                            (LogFields{
                                {.name = "cue", .value = cue.id},
                                {.name = "category", .value = std::string{audio_category_name(cue.category)}},
                                {.name = "reason", .value = "voice limit or priority"},
                            }));
            return {};
        }
        if (steal >= 0) {
            // The stolen voice fades out over a few milliseconds beside the new one.
            stop_voice(voices[static_cast<std::size_t>(steal)], 0.0f);
            ++stats.stolen_voices;
        }

        const AudioClip& clip = *part.clip;
        Voice voice{
            .handle = AudioHandle{next_handle++},
            .cue_id = cue.id,
            .bus = sync_bus(catalog, cue.bus),
            .category = cue.category,
            .clip = part.clip,
            .rate_ratio = static_cast<f64>(clip.sample_rate()) / static_cast<f64>(sample_rate),
            .stream = std::move(part.stream),
            .volume = std::max(0.0f, cue.volume * request.volume),
            .cue_volume = std::max(0.0f, cue.volume),
            .volume_target = std::max(0.0f, cue.volume * request.volume),
            .base_pitch = std::max(0.01f, cue.pitch * (1.0f + part.variation)),
            .pitch = std::max(0.01f, cue.pitch * request.pitch * (1.0f + part.variation)),
            .priority = cue.priority + request.priority_boost,
            .loop = part.loop,
            .spatial = cue.spatial,
            .min_distance = cue.min_distance,
            .max_distance = cue.max_distance,
            .rolloff = cue.rolloff,
            .rolloff_power = cue.rolloff_power,
            .pan_strength = cue.pan_strength,
            .position = request.position,
            .has_position = request.has_position,
            .start_at = start_at,
        };
        if (const AudioClipRef* ref = catalog.clip(part.clip_id)) {
            voice.loop_start = ref->loop_start;
            voice.loop_end = ref->loop_end;
        }
        if (voice.stream) {
            voice.stream_buffer.resize(static_cast<std::size_t>(stream_buffer_frames * clip.channels()));
        }
        if (start_seconds > 0.0) {
            const i64 frame = std::clamp<i64>(std::llround(start_seconds * clip.sample_rate()), 0,
                                              std::max(0, clip.frame_count() - 1));
            jump(voice, frame);
        }
        if (request.fade_in > 0.0f) {
            voice.fade = 0.0f;
            start_fade(voice, 1.0f, request.fade_in);
        }
        const AudioHandle handle = voice.handle;
        voices.push_back(std::move(voice));
        ++stats.played_requests;
        stats.active_voices = static_cast<i32>(std::ranges::count_if(voices, [](const Voice& v) { return !v.stopping; }));
        KIN_LOG_DEBUG_F("audio",
                        "audio cue playing",
                        (LogFields{
                            {.name = "cue", .value = cue.id},
                            {.name = "clip", .value = part.clip_id},
                            {.name = "bus", .value = cue.bus},
                            {.name = "category", .value = std::string{audio_category_name(cue.category)}},
                        }));
        return handle;
    }

    // --- clips (game thread) ---

    // Frees, on this thread, what finished voices handed back.
    void free_retired() {
        std::vector<std::shared_ptr<const AudioClip>> clips;
        std::vector<std::unique_ptr<AudioDecoder>> streams;
        {
            const std::scoped_lock lock{mutex};
            // Move them out; clear() keeps the capacity reserved for the audio thread.
            clips.assign(std::make_move_iterator(retired_clips.begin()), std::make_move_iterator(retired_clips.end()));
            streams.assign(std::make_move_iterator(retired_streams.begin()), std::make_move_iterator(retired_streams.end()));
            retired_clips.clear();
            retired_streams.clear();
        }
    }

    std::shared_ptr<const AudioClip> take_loaded(const std::string& key, Job<AudioClip>& job) {
        auto clip = std::make_shared<const AudioClip>(std::move(job.get()));
        clip_cache.insert_or_assign(key, clip);
        watch_clip(key);
        return clip;
    }

    static void unwatch(const WatchRef& ref) {
        if (ref.files && !ref.alive.expired()) {
            ref.files->unwatch(ref.id);
        }
    }

    // Reloads a cached clip's file when it changes, if watch() was called. The
    // cache key is the file's path, with "|stream" for a streamed copy.
    void watch_clip(const std::string& key) {
        if (!clip_files || clip_files_alive.expired() || clip_watches.contains(key)) {
            return;
        }
        const bool stream = key.ends_with("|stream");
        const std::filesystem::path path{stream ? key.substr(0, key.size() - 7) : key};
        const u32 id = clip_files->watch(path, [this, key, path, stream](const std::filesystem::path&) {
            if (!clip_cache.contains(key)) {
                return;
            }
            try {
                // Voices playing the old version keep it; new plays get this one.
                clip_cache.insert_or_assign(key, std::make_shared<const AudioClip>(load_file(path, stream)));
                KIN_LOG_INFO_F("audio", "audio clip reloaded", (LogFields{{.name = "path", .value = path.string()}}));
            } catch (const std::exception& error) {
                KIN_LOG_WARN_F("audio",
                               "audio clip reload failed, keeping the last version",
                               (LogFields{
                                   {.name = "path", .value = path.string()},
                                   {.name = "error", .value = error.what()},
                               }));
            }
        });
        clip_watches.emplace(key, WatchRef{clip_files, clip_files_alive, id});
    }

    void forget_clip(const std::string& key) {
        if (const auto found = clip_watches.find(key); found != clip_watches.end()) {
            unwatch(found->second);
            clip_watches.erase(found);
        }
        clip_cache.erase(key);
    }

    void unwatch_all() {
        for (const auto& [key, ref] : clip_watches) {
            unwatch(ref);
        }
        for (const WatchRef& ref : catalog_watches) {
            unwatch(ref);
        }
        clip_watches.clear();
        catalog_watches.clear();
    }

    // Registers every bus and duck rule of the catalog now (play() otherwise
    // does it for the buses a cue uses).
    void apply_catalog(const AudioCatalog& catalog) {
        const std::scoped_lock lock{mutex};
        for (const auto& [id, bus] : catalog.buses()) {
            sync_bus(catalog, id);
        }
        sync_ducks(catalog);
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
                watch_clip(key);
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
    if (_state) {
        _state->unwatch_all();
    }
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
        if (_state) {
            _state->unwatch_all();
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
    s.advance_playlist();
    s.free_retired();

    s.device_poll += std::max(0.0f, dt);
    if (s.device_poll >= 1.0f) {
        s.device_poll = 0.0f;
        _backend->poll(); // e.g. fall back to the default device if ours was unplugged
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
    const AudioCue* cue = s.find_cue(catalog, request.cue);
    if (!cue) {
        return {};
    }
    State::Prepared part;
    if (!s.prepare(catalog, *cue, s.choose_clip(*cue), part)) {
        return {};
    }
    const std::scoped_lock lock{s.mutex};
    return s.commit(catalog, std::move(part), request, 0, 0.0);
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

void AudioEngine::set_bus_effects(std::string_view bus, std::vector<AudioEffect> effects) {
    State& s = *_state;
    auto made = s.make_effects(effects); // allocate before taking the lock
    {
        const std::scoped_lock lock{s.mutex};
        State::Bus& state = s.buses[static_cast<std::size_t>(s.ensure_bus(bus))];
        std::swap(state.effects, made); // the old chain is freed after unlocking
        state.effect_source = std::move(effects);
        state.effects_from_game = true;
    }
}

void AudioEngine::set_bus_effect(std::string_view bus, std::size_t index, const AudioEffect& effect) {
    State& s = *_state;
    {
        const std::scoped_lock lock{s.mutex};
        const auto found = s.bus_index.find(std::string{bus});
        if (found == s.bus_index.end()) {
            return;
        }
        State::Bus& state = s.buses[static_cast<std::size_t>(found->second)];
        if (index >= state.effects.size()) {
            return;
        }
        if (state.effects[index]->effect().type == effect.type) {
            state.effects[index]->set(effect); // keeps its state: no click
            state.effect_source[index] = effect;
            return;
        }
    }
    std::unique_ptr<AudioEffectProcessor> made = make_audio_effect(effect, s.sample_rate);
    const std::scoped_lock lock{s.mutex};
    State::Bus& state = s.buses[static_cast<std::size_t>(s.bus_index.at(std::string{bus}))];
    if (index < state.effects.size()) {
        std::swap(state.effects[index], made);
        state.effect_source[index] = effect;
    }
}

std::vector<AudioEffect> AudioEngine::bus_effects(std::string_view bus) const {
    const std::scoped_lock lock{_state->mutex};
    const State::Bus* state = _state->find_bus(bus);
    return state ? state->effect_source : std::vector<AudioEffect>{};
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
    return play_music(catalog, cue, AudioMusicTransition{.crossfade = crossfade});
}

AudioHandle AudioEngine::play_music(const AudioCatalog& catalog, std::string_view cue_id, const AudioMusicTransition& transition) {
    State& s = *_state;
    if (s.music.catalog && s.music.cue.id == cue_id && music()) {
        return music(); // already playing: keep it going
    }
    const AudioCue* cue = s.find_cue(catalog, cue_id);
    if (!cue) {
        return {};
    }

    // Load every layer before taking the lock, so they all start on one frame.
    std::vector<State::Prepared> parts;
    std::vector<std::string> names;
    std::size_t index = 0;
    if (cue->layers) {
        for (const std::string& clip : cue->clips) {
            State::Prepared part;
            if (s.prepare(catalog, *cue, clip, part)) {
                parts.push_back(std::move(part));
                names.push_back(clip);
            }
        }
    } else {
        std::string clip_id;
        if (cue->playlist) {
            index = cue->shuffle ? static_cast<std::size_t>(rng_i32(s.next_key(), 0, static_cast<i32>(cue->clips.size()) - 1)) : 0;
            clip_id = cue->clips[index];
        } else {
            clip_id = s.choose_clip(*cue);
        }
        State::Prepared part;
        if (s.prepare(catalog, *cue, clip_id, part)) {
            part.loop = cue->loop && !cue->playlist; // a playlist loops the list, not each clip
            parts.push_back(std::move(part));
            names.emplace_back();
        }
    }
    if (parts.empty()) {
        return {};
    }

    State::Music next{.catalog = &catalog, .cue = *cue, .index = index};
    const std::scoped_lock lock{s.mutex};
    const i64 start = s.sync_frame(transition.sync);
    const f64 start_seconds = transition.match_position ? s.music_seconds_at(start) : 0.0;
    s.stop_music_voices(start, transition.crossfade);
    for (std::size_t i = 0; i < parts.size(); ++i) {
        parts[i].cue = &next.cue;
        const AudioHandle handle = s.commit(catalog, std::move(parts[i]), {
            .cue = cue->id,
            .volume = s.layer_volume(names[i]),
            .fade_in = transition.crossfade,
        }, start, start_seconds);
        if (handle) {
            next.layers.push_back({.name = names[i], .handle = handle});
        }
    }
    s.music = std::move(next);
    return s.music.layers.empty() ? AudioHandle{} : s.music.layers.front().handle;
}

void AudioEngine::stop_music(f32 fade) {
    State& s = *_state;
    const std::scoped_lock lock{s.mutex};
    s.stop_music_voices(s.output_frame, fade);
    s.music = {};
}

AudioHandle AudioEngine::music() const {
    const std::scoped_lock lock{_state->mutex};
    const State::Voice* voice = _state->music_voice();
    return voice ? voice->handle : AudioHandle{};
}

void AudioEngine::set_music_layer(std::string_view layer, f32 volume, f32 fade) {
    State& s = *_state;
    s.layer_volumes[std::string{layer}] = std::max(0.0f, volume);
    std::vector<AudioHandle> handles;
    for (const State::MusicLayer& entry : s.music.layers) {
        if (entry.name == layer) {
            handles.push_back(entry.handle);
        }
    }
    for (const AudioHandle handle : handles) {
        set_volume(handle, volume, fade);
    }
}

AudioMusicPosition AudioEngine::music_position() const {
    const std::scoped_lock lock{_state->mutex};
    State& s = *_state;
    const State::Voice* voice = s.music_voice();
    if (!voice || voice->start_at > s.output_frame) {
        return {};
    }
    AudioMusicPosition position{.playing = true};
    position.seconds = static_cast<f32>(State::source_position(*voice) / voice->clip->sample_rate());
    if (s.music.cue.bpm > 0.0f) {
        const i32 per_bar = std::max(1, s.music.cue.beats_per_bar);
        position.beat = (position.seconds - s.music.cue.beat_offset) * s.music.cue.bpm / 60.0f;
        position.bar = static_cast<i32>(std::floor(position.beat / static_cast<f32>(per_bar)));
        position.beat_in_bar = position.beat - static_cast<f32>(position.bar * per_bar);
    }
    return position;
}

AudioHandle AudioEngine::play_synced(const AudioCatalog& catalog, const AudioPlayRequest& request, AudioSync sync) {
    State& s = *_state;
    const AudioCue* cue = s.find_cue(catalog, request.cue);
    if (!cue) {
        return {};
    }
    State::Prepared part;
    if (!s.prepare(catalog, *cue, s.choose_clip(*cue), part)) {
        return {};
    }
    const std::scoped_lock lock{s.mutex};
    return s.commit(catalog, std::move(part), request, s.sync_frame(sync), 0.0);
}

bool AudioEngine::seek(AudioHandle handle, f32 seconds) {
    const std::scoped_lock lock{_state->mutex};
    for (State::Voice& voice : _state->voices) {
        if (voice.handle != handle || voice.stopping || !voice.clip) {
            continue;
        }
        const i64 frame = std::clamp<i64>(std::llround(static_cast<f64>(std::max(0.0f, seconds)) * voice.clip->sample_rate()),
                                          0, std::max(0, voice.clip->frame_count() - 1));
        if (!voice.primed || voice.fresh || (voice.gain_left == 0.0f && voice.gain_right == 0.0f)) {
            voice.seek_frame = -1;
            return State::jump(voice, frame); // nothing audible to fade
        }
        if (voice.seek_frame < 0) {
            voice.seek_restore = voice.fade_target;
        }
        voice.seek_frame = frame;
        _state->start_fade(voice, 0.0f, declick_seconds);
        return true;
    }
    return false;
}

void AudioEngine::set_paused(AudioHandle handle, bool paused) {
    const std::scoped_lock lock{_state->mutex};
    for (State::Voice& voice : _state->voices) {
        if (voice.handle == handle) {
            voice.paused = paused;
        }
    }
}

bool AudioEngine::paused(AudioHandle handle) const {
    const std::scoped_lock lock{_state->mutex};
    return std::ranges::any_of(_state->voices, [handle](const State::Voice& voice) {
        return voice.handle == handle && voice.paused && !voice.stopping;
    });
}

void AudioEngine::remove_clip(std::string_view clip_id) {
    _state->memory_clips.erase(std::string{clip_id});
}

i32 AudioEngine::unload_unused() {
    State& s = *_state;
    s.free_retired(); // finished voices let go of their clips
    // Only the cache holds these: no voice is playing them.
    std::vector<std::string> unused;
    for (const auto& [key, clip] : s.clip_cache) {
        if (clip.use_count() == 1) {
            unused.push_back(key);
        }
    }
    for (const std::string& key : unused) {
        s.forget_clip(key);
    }
    return static_cast<i32>(unused.size());
}

void AudioEngine::unload(const AudioCatalog& catalog) {
    State& s = *_state;
    for (const auto& [id, clip] : catalog.clips()) {
        const std::filesystem::path path = catalog.resolve_clip_path(id);
        for (const bool stream : {false, true}) {
            s.forget_clip(State::cache_key(path, stream));
            s.loading.erase(State::cache_key(path, stream));
        }
    }
}

void AudioEngine::watch(FileWatcher& files) {
    State& s = *_state;
    s.clip_files = &files;
    s.clip_files_alive = files.lifetime();
    for (const auto& [key, clip] : s.clip_cache) {
        s.watch_clip(key);
    }
}

void AudioEngine::watch_catalog(FileWatcher& files, std::filesystem::path path, AudioCatalog& catalog) {
    State& s = *_state;
    const u32 id = files.watch(path, [state = &s, &catalog, path](const std::filesystem::path&) {
        try {
            AudioCatalog loaded = load_audio_catalog(path);
            catalog = std::move(loaded);
            state->apply_catalog(catalog);
            KIN_LOG_INFO_F("audio", "audio catalog reloaded", (LogFields{{.name = "path", .value = path.string()}}));
        } catch (const std::exception& error) {
            KIN_LOG_WARN_F("audio",
                           "audio catalog reload failed, keeping the last version",
                           (LogFields{
                               {.name = "path", .value = path.string()},
                               {.name = "error", .value = error.what()},
                           }));
        }
    });
    s.catalog_watches.push_back({&files, files.lifetime(), id});
}

void AudioEngine::apply_catalog(const AudioCatalog& catalog) {
    _state->apply_catalog(catalog);
}

namespace {

AudioLevel level_of(f32 peak, f32 mean_square) {
    return {.peak = peak, .rms = std::sqrt(std::max(0.0f, mean_square))};
}

} // namespace

AudioLevel AudioEngine::bus_level(std::string_view bus) const {
    const std::scoped_lock lock{_state->mutex};
    const State::Bus* state = _state->find_bus(bus);
    return state ? level_of(state->meter.peak, state->meter.mean_square) : AudioLevel{};
}

AudioLevel AudioEngine::output_level() const {
    const std::scoped_lock lock{_state->mutex};
    return level_of(_state->output_meter.peak, _state->output_meter.mean_square);
}

void AudioEngine::enable_analysis(std::string_view bus, bool enabled) {
    State& s = *_state;
    std::vector<f32> history(enabled ? static_cast<std::size_t>(audio_analysis_frames) : 0, 0.0f);
    const std::scoped_lock lock{s.mutex};
    State::Meter& meter = bus.empty() ? s.output_meter : s.buses[static_cast<std::size_t>(s.ensure_bus(bus))].meter;
    if (meter.history.empty() != enabled) {
        return; // already as asked
    }
    std::swap(meter.history, history); // the old buffer is freed after unlocking
    meter.history_pos = 0;
}

std::vector<f32> AudioEngine::spectrum(std::string_view bus, i32 bands, f32 min_hz, f32 max_hz) const {
    std::vector<f32> recent;
    {
        const std::scoped_lock lock{_state->mutex};
        const State::Meter* meter = &_state->output_meter;
        if (!bus.empty()) {
            const State::Bus* state = _state->find_bus(bus);
            meter = state ? &state->meter : nullptr;
        }
        if (!meter || meter->history.empty()) {
            return {};
        }
        // Oldest first.
        recent.reserve(meter->history.size());
        recent.insert(recent.end(), meter->history.begin() + static_cast<std::ptrdiff_t>(meter->history_pos), meter->history.end());
        recent.insert(recent.end(), meter->history.begin(), meter->history.begin() + static_cast<std::ptrdiff_t>(meter->history_pos));
    }
    const std::vector<f32> bins = audio_spectrum_bins(recent);
    const f32 rate = static_cast<f32>(_state->sample_rate);
    const f32 low = std::max(1.0f, min_hz);
    const f32 high = std::max(low * 1.001f, std::min(max_hz, rate * 0.5f));
    std::vector<f32> result(static_cast<std::size_t>(std::max(0, bands)));
    for (std::size_t band = 0; band < result.size(); ++band) {
        const f32 from = low * std::pow(high / low, static_cast<f32>(band) / static_cast<f32>(result.size()));
        const f32 to = low * std::pow(high / low, static_cast<f32>(band + 1) / static_cast<f32>(result.size()));
        result[band] = audio_band_magnitude(bins, rate, from, to);
    }
    return result;
}

f32 AudioEngine::magnitude(std::string_view bus, f32 from_hz, f32 to_hz) const {
    const std::vector<f32> band = spectrum(bus, 1, from_hz, to_hz);
    return band.empty() ? 0.0f : band.front();
}

i32 AudioEngine::loaded_clip_count() const {
    return static_cast<i32>(_state->clip_cache.size() + _state->memory_clips.size());
}

std::size_t AudioEngine::loaded_clip_bytes() const {
    std::size_t bytes = 0;
    for (const auto* clips : {&_state->clip_cache, &_state->memory_clips}) {
        for (const auto& [key, clip] : *clips) {
            bytes += clip->memory_bytes();
        }
    }
    return bytes;
}

bool AudioEngine::set_output_device(std::string_view name) {
    const bool switched = _backend->set_device(name);
    KIN_LOG_INFO_F("audio",
                   switched ? "audio output device set" : "audio output device unavailable",
                   (LogFields{{.name = "device", .value = name.empty() ? std::string{"default"} : std::string{name}}}));
    return switched;
}

std::string AudioEngine::output_device() const {
    return _backend->device();
}

f32 AudioEngine::playback_position(AudioHandle handle) const {
    const std::scoped_lock lock{_state->mutex};
    for (const State::Voice& voice : _state->voices) {
        if (voice.handle == handle && !voice.stopping && voice.clip) {
            // f0, the frame the output is leaving, is two behind the next read.
            const f64 frame = voice.seek_frame >= 0 ? static_cast<f64>(voice.seek_frame)
                : voice.primed ? static_cast<f64>(voice.next_frame - (voice.at_end ? 1 : 2)) + voice.frac
                               : static_cast<f64>(voice.next_frame);
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
    json.field("output_device", _backend->device());
    json.field("loaded_clips", static_cast<i32>(s.clip_cache.size() + s.memory_clips.size()));
    json.field("peak", static_cast<f64>(s.output_meter.peak));
    json.field("rms", static_cast<f64>(std::sqrt(s.output_meter.mean_square)));
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
        json.begin_object();
        json.field("cue", s.music.cue.id);
        json.key("layers").begin_array();
        for (const State::MusicLayer& layer : s.music.layers) {
            json.value(layer.name);
        }
        json.end_array();
        json.end_object();
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
        json.field("peak", static_cast<f64>(bus.meter.peak));
        json.field("rms", static_cast<f64>(std::sqrt(bus.meter.mean_square)));
        json.key("effects").begin_array();
        for (const auto& effect : bus.effects) {
            json.value(audio_effect_type_name(effect->effect().type));
        }
        json.end_array();
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
        json.field("paused", voice.paused);
        json.field("stopping", voice.stopping);
        json.end_object();
    }
    json.end_array();
    json.end_object();
}

} // namespace kin
