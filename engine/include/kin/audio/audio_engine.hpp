#pragma once

#include <kin/audio/audio_catalog.hpp>
#include <kin/audio/backend.hpp>
#include <kin/audio/spatial_audio.hpp>

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kin {

class FileWatcher;
class JobSystem;
class JsonWriter;

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
    // Seconds to fade in from silence; 0 starts at full volume.
    f32 fade_in = 0.0f;
};

struct AudioEngineConfig {
    i32 sample_rate = 48000;
    i32 channels = 2; // 1 or 2
    i32 music_voices = 2;
    i32 ambient_voices = 12;
    i32 sound_effect_voices = 64;
    i32 ui_voices = 8;
    // Seeds which clip a cue with several clips plays and its pitch variation,
    // so a run with the same requests sounds the same.
    u64 seed = 0x6b696e2d617564ULL;
};

struct AudioEngineStats {
    i32 active_voices = 0;
    i32 played_requests = 0;
    i32 culled_requests = 0;
    i32 stolen_voices = 0;
    i64 mixed_frames = 0;
    // Output frames the master limiter turned down to keep the mix from clipping.
    i64 limited_frames = 0;
};

// How loud something is playing, in linear amplitude (1 is full scale): the
// peak falls back over ~0.3 s, and rms is the average over about as long.
struct AudioLevel {
    f32 peak = 0.0f;
    f32 rms = 0.0f;
};

// A bus's runtime state: what the game set on it, on top of the catalog.
struct AudioBusState {
    std::string id;
    f32 volume = 1.0f;
    bool muted = false;
    bool paused = false;
};

// Plays cues from an AudioCatalog. Call it from one thread (the game thread);
// with a device backend, mixing happens on the device's audio thread.
class AudioEngine {
public:
    explicit AudioEngine(AudioEngineConfig config = {});
    AudioEngine(std::unique_ptr<IAudioBackend> backend, AudioEngineConfig config = {});
    ~AudioEngine();
    AudioEngine(AudioEngine&& other) noexcept;
    AudioEngine& operator=(AudioEngine&& other) noexcept;
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // Ages voices and picks up clips preload_async finished. With a backend that
    // is not real time, it also mixes `dt` seconds, so voices finish on time.
    void update(f32 dt);
    // Mixes the next frames into `interleaved` (frames * channels()) on the
    // calling thread: offline rendering and tests. With a device backend that is
    // pulling, the frames rendered here are never heard.
    void render(std::span<f32> interleaved);

    // Registers a clip made in memory (e.g. synthesized, or from make_memory_audio_clip)
    // under a clip id, which cues then name like a file-backed clip; it needs no
    // catalog entry and takes precedence over one with the same id.
    void add_clip(std::string_view clip_id, AudioClip clip);
    // Loads every clip the catalog names now, so play() never stops to decode.
    void preload(const AudioCatalog& catalog);
    // Decodes the catalog's clips on `jobs`. A play() that needs a clip still
    // loading waits for that job instead of decoding the file again.
    void preload_async(const AudioCatalog& catalog, JobSystem& jobs);
    // Frees cached clips no voice is playing and returns how many; a later
    // play() loads them again. Call between levels, or after unload().
    i32 unload_unused();
    // Drops the catalog's clips from the cache. Voices playing them keep them
    // until they finish; then they are freed.
    void unload(const AudioCatalog& catalog);
    // Forgets a clip add_clip registered.
    void remove_clip(std::string_view clip_id);
    // Reloads clip files the engine loaded when they change on disk (polling
    // `files` runs the reloads). New plays use the new version; voices already
    // playing finish with the old. A file that fails to decode keeps the last
    // good version. `files` may be destroyed first.
    void watch(FileWatcher& files);
    // Reloads the catalog file at `path` into `catalog` when it changes, and
    // applies its buses, effects and duck rules at once; a broken edit keeps
    // the last good catalog. `catalog` must outlive the watch (or the engine).
    void watch_catalog(FileWatcher& files, std::filesystem::path path, AudioCatalog& catalog);
    // Registers all of a catalog's buses, effects and duck rules now, rather
    // than as cues first play on them: after editing a catalog in code.
    void apply_catalog(const AudioCatalog& catalog);
    i32 loaded_clip_count() const;
    // Memory the loaded clips take: decoded samples, or a streamed file's bytes.
    std::size_t loaded_clip_bytes() const;

    AudioHandle play(const AudioCatalog& catalog, const AudioPlayRequest& request);
    // Stops a voice, fading out over `fade` seconds (a few milliseconds at least,
    // so nothing clicks). playing() is false from the call on.
    void stop(AudioHandle handle, f32 fade = 0.0f);
    // Stops every voice on `bus` and on the buses under it.
    void stop_bus(std::string_view bus, f32 fade = 0.0f);
    // The bus's own volume, on top of the catalog's; children follow their
    // parents, so set_bus_volume("master", 0.5f) halves everything under master.
    void set_bus_volume(std::string_view bus, f32 volume, f32 fade = 0.0f);
    // Replaces the bus's effect chain; until a game sets one, the bus uses its
    // catalog's. Effects run on the bus's mix before its volume.
    void set_bus_effects(std::string_view bus, std::vector<AudioEffect> effects);
    // Changes one effect while it plays, e.g. sweeping a low-pass cutoff for a
    // muffled pause menu; filter memory and reverb tails carry on. An effect
    // of another type replaces it.
    void set_bus_effect(std::string_view bus, std::size_t index, const AudioEffect& effect);
    std::vector<AudioEffect> bus_effects(std::string_view bus) const;
    void set_bus_muted(std::string_view bus, bool muted);
    // A paused bus's voices fade out briefly and hold their place until resumed:
    // pause "sfx" with the game while "ui" keeps playing.
    void set_bus_paused(std::string_view bus, bool paused);
    void set_listener(Vec2f position);
    void set_position(AudioHandle handle, Vec2f position);
    // A voice's volume on top of its cue's, and its pitch on top of the cue's
    // (with the cue's variation kept).
    void set_volume(AudioHandle handle, f32 volume, f32 fade = 0.0f);
    void set_pitch(AudioHandle handle, f32 pitch);
    // Jumps to `seconds` into the clip, with a few milliseconds' fade out and
    // back in. False if the voice is not playing (or a stream cannot seek).
    bool seek(AudioHandle handle, f32 seconds);
    // Pauses one voice: it fades out and holds its place until resumed, and
    // still counts as playing.
    void set_paused(AudioHandle handle, bool paused);
    bool paused(AudioHandle handle) const;

    // The music: one cue at a time. Playing another crossfades to it over
    // `crossfade` seconds; playing the cue already playing keeps it going, so
    // each scene can name its music without restarting it.
    AudioHandle play_music(const AudioCatalog& catalog, std::string_view cue, f32 crossfade = 1.0f);
    void stop_music(f32 fade = 1.0f);
    AudioHandle music() const;

    bool playing(AudioHandle handle) const;
    // Seconds into the clip the voice has played (looping wraps); 0 if it is
    // not playing.
    f32 playback_position(AudioHandle handle) const;
    i32 active_voice_count() const;
    i32 active_voice_count(AudioCategory category) const;
    // The volume set_bus_volume set (or is fading to); 1 by default.
    f32 bus_volume(std::string_view bus) const;
    bool bus_muted(std::string_view bus) const;
    bool bus_paused(std::string_view bus) const;
    // Catalog and runtime volumes multiplied up the bus hierarchy; 0 if any is muted.
    f32 effective_bus_volume(const AudioCatalog& catalog, std::string_view bus) const;
    // Every bus the engine knows, in the order it met them.
    std::vector<AudioBusState> bus_states() const;
    // Level meters: what a bus sends to its parent (after its effects and
    // volume), and the final output. Always on.
    AudioLevel bus_level(std::string_view bus) const;
    AudioLevel output_level() const;
    // Starts (or stops) recording the last 2048 frames a bus plays, "" for the
    // final output, so spectrum() can analyse them. Off by default.
    void enable_analysis(std::string_view bus, bool enabled = true);
    // The amplitude in each of `bands` frequency bands, spaced evenly in pitch
    // from min_hz to max_hz: a sine of amplitude A reads about A in its band.
    // Computed on the calling thread from the last ~43 ms; empty if analysis
    // is off for the bus.
    std::vector<f32> spectrum(std::string_view bus, i32 bands, f32 min_hz = 20.0f, f32 max_hz = 20000.0f) const;
    // The strongest amplitude between two frequencies (one band of spectrum()).
    f32 magnitude(std::string_view bus, f32 from_hz, f32 to_hz) const;
    i32 sample_rate() const;
    i32 channels() const;
    bool device_available() const;
    // Plays on the named output device (a name from list_audio_output_devices);
    // "" is the system default, which follows the system's choice. False if
    // the device cannot be opened (playback stays where it was). If the
    // device is unplugged, playback moves to the default and back when it
    // returns.
    bool set_output_device(std::string_view name);
    std::string output_device() const;
    AudioEngineStats stats() const;
    void reset_stats();
    // Stats, buses and voices as a JSON object, for a scene's write_report.
    void write_report(JsonWriter& json) const;

private:
    struct State;
    std::unique_ptr<IAudioBackend> _backend;
    std::unique_ptr<State> _state;
};

} // namespace kin
