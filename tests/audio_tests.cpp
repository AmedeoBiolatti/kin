#include <kin/audio/audio.hpp>
#include <kin/anim/player.hpp>
#include <kin/assets/file_watcher.hpp>
#include <kin/core/jobs.hpp>
#include <kin/core/json.hpp>
#include <kin/core/json_value.hpp>
#include <kin/scripting/lua_audio.hpp>
#include <kin/scripting/lua_script.hpp>
#include <kin/scripting/script_engine.hpp>
#include <kin/ecs/audio.hpp>
#include <kin/platform/log.hpp>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace {

bool has_log_event(const std::vector<kin::LogEvent>& events,
                   std::string_view category,
                   std::string_view message) {
    return std::ranges::any_of(events, [&](const kin::LogEvent& event) {
        return event.category == category && event.message == message;
    });
}

void write_u16(std::ofstream& out, kin::u16 value) {
    out.put(static_cast<char>(value & 0xff));
    out.put(static_cast<char>((value >> 8) & 0xff));
}

void write_u32(std::ofstream& out, kin::u32 value) {
    out.put(static_cast<char>(value & 0xff));
    out.put(static_cast<char>((value >> 8) & 0xff));
    out.put(static_cast<char>((value >> 16) & 0xff));
    out.put(static_cast<char>((value >> 24) & 0xff));
}

void write_wav(const std::filesystem::path& path, kin::i16 sample = 8000, kin::i32 frames = 64) {
    std::ofstream out(path, std::ios::binary);
    const kin::u16 channels = 1;
    const kin::u32 sample_rate = 48000;
    const kin::u16 bits = 16;
    const kin::u32 data_bytes = static_cast<kin::u32>(frames * channels * (bits / 8));

    out.write("RIFF", 4);
    write_u32(out, 36 + data_bytes);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    write_u32(out, 16);
    write_u16(out, 1);
    write_u16(out, channels);
    write_u32(out, sample_rate);
    write_u32(out, sample_rate * channels * (bits / 8));
    write_u16(out, channels * (bits / 8));
    write_u16(out, bits);
    out.write("data", 4);
    write_u32(out, data_bytes);
    for (kin::i32 i = 0; i < frames; ++i) {
        write_u16(out, static_cast<kin::u16>(sample));
    }
}

kin::AudioCatalog make_catalog(const std::filesystem::path& dir) {
    kin::AudioCatalog catalog;
    catalog.set_root(dir);
    catalog.add_bus({.id = "master", .volume = 0.5f});
    catalog.add_bus({.id = "sfx", .parent = "master", .volume = 0.5f});
    catalog.add_bus({.id = "music", .parent = "master", .volume = 1.0f});
    catalog.add_clip({.id = "beep", .path = "beep.wav"});
    catalog.add_clip({.id = "boop", .path = "boop.wav"});
    catalog.add_cue({
        .id = "step",
        .clips = {"beep"},
        .category = kin::AudioCategory::Sound,
        .bus = "sfx",
        .volume = 1.0f,
        .priority = 10,
        .max_instances = 1,
        .spatial = true,
        .min_distance = 8.0f,
        .max_distance = 64.0f,
    });
    catalog.add_cue({
        .id = "impact",
        .clips = {"boop"},
        .category = kin::AudioCategory::Effect,
        .bus = "sfx",
        .priority = 100,
    });
    catalog.add_cue({
        .id = "theme",
        .clips = {"beep"},
        .category = kin::AudioCategory::Music,
        .bus = "music",
        .priority = 1,
        .loop = true,
    });
    return catalog;
}

kin::AudioEngine make_test_engine(kin::AudioEngineConfig config = {}) {
    return kin::AudioEngine{kin::create_null_audio_backend(config.sample_rate, config.channels), config};
}

void test_catalog_roundtrip_and_loader() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "kin-audio-tests";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    write_wav(dir / "beep.wav");
    write_wav(dir / "boop.wav", 12000);

    kin::AudioCatalog catalog = make_catalog(dir);
    const std::filesystem::path path = dir / "audio.kinaudio";
    assert(kin::save_audio_catalog(catalog, path));

    const kin::AudioCatalog loaded = kin::load_audio_catalog(path);
    assert(loaded.root() == dir);
    assert(loaded.clip("beep") != nullptr);
    assert(loaded.cue("step") != nullptr);
    assert(loaded.cue("step")->spatial);
    assert(loaded.cue("theme")->loop);
    assert(std::fabs(loaded.effective_bus_volume("sfx") - 0.25f) < 0.001f);

    kin::AssetManager assets{dir};
    assets.discover();
    const kin::AssetMetadata* meta = assets.metadata("audio.kinaudio");
    assert(meta != nullptr);
    assert(meta->type == kin::AssetType::AudioCatalog);
    const auto asset = assets.load<kin::AudioCatalog>("audio.kinaudio");
    assert(asset);
    assert(asset->cue("impact") != nullptr);
}

void test_spatial_audio() {
    const kin::SpatialAudioResult near = kin::calculate_spatial_audio({0.0f, 0.0f}, {4.0f, 0.0f}, {
        .min_distance = 8.0f,
        .max_distance = 64.0f,
    });
    assert(near.gain > 0.99f);
    assert(near.pan > 0.0f);

    const kin::SpatialAudioResult far = kin::calculate_spatial_audio({0.0f, 0.0f}, {128.0f, 0.0f}, {
        .min_distance = 8.0f,
        .max_distance = 64.0f,
    });
    assert(far.gain == 0.0f);
    assert(far.pan == 1.0f);
}

void test_voice_priority_caps_and_instances() {
    std::vector<kin::LogEvent> log_events;
    kin::set_logger_config({
        .min_level = kin::LogLevel::Debug,
        .format = kin::LogFormat::Text,
        .sdl_sink = false,
        .memory_events = &log_events,
    });

    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "kin-audio-tests-priority";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    write_wav(dir / "beep.wav");
    write_wav(dir / "boop.wav");
    const kin::AudioCatalog catalog = make_catalog(dir);

    kin::AudioEngineConfig config;
    config.sound_effect_voices = 1;
    kin::AudioEngine audio = make_test_engine(config);

    const kin::AudioHandle low = audio.play(catalog, {.cue = "step", .position = {0.0f, 0.0f}, .has_position = true});
    assert(low);
    assert(audio.active_voice_count(kin::AudioCategory::Sound) == 1);

    const kin::AudioHandle rejected = audio.play(catalog, {.cue = "step", .position = {0.0f, 0.0f}, .has_position = true});
    assert(!rejected);
    assert(audio.active_voice_count() == 1);
    assert(has_log_event(log_events, "audio", "audio play rejected"));
    // Culling is routine, so it stays out of the default (info) log.
    assert(std::ranges::all_of(log_events, [](const kin::LogEvent& event) {
        return event.message != "audio play rejected" || event.level == kin::LogLevel::Debug;
    }));
    assert(audio.stats().culled_requests == 1);

    const kin::AudioHandle high = audio.play(catalog, {.cue = "impact"});
    assert(high);
    assert(!audio.playing(low));
    assert(audio.playing(high));
    assert(audio.stats().stolen_voices == 1);
    assert(has_log_event(log_events, "audio", "audio cue playing"));
    kin::set_logger_config({
        .min_level = kin::LogLevel::Debug,
        .format = kin::LogFormat::Text,
        .sdl_sink = false,
    });
}

void test_music_has_dedicated_cap() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "kin-audio-tests-music";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    write_wav(dir / "beep.wav");
    write_wav(dir / "boop.wav");
    const kin::AudioCatalog catalog = make_catalog(dir);

    kin::AudioEngineConfig config;
    config.sound_effect_voices = 1;
    config.music_voices = 1;
    kin::AudioEngine audio = make_test_engine(config);
    const kin::AudioHandle sfx = audio.play(catalog, {.cue = "impact"});
    const kin::AudioHandle music = audio.play(catalog, {.cue = "theme"});
    assert(sfx);
    assert(music);
    assert(audio.playing(sfx));
    assert(audio.playing(music));
    assert(audio.active_voice_count() == 2);
}

void test_ecs_audio_bridge_and_animation_events() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "kin-audio-tests-ecs";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    write_wav(dir / "beep.wav");
    write_wav(dir / "boop.wav");
    const kin::AudioCatalog catalog = make_catalog(dir);
    kin::AudioEngine audio = make_test_engine();

    kin::EcsWorld world;
    kin::EcsEntity listener = world.entity("listener");
    listener.set(kin::Transform2D{.pos = {16.0f, 0.0f}});
    listener.set(kin::AudioListener{.priority = 5});
    kin::update_audio_listeners(world, audio);

    kin::EcsEntity emitter = world.entity("emitter");
    emitter.set(kin::Transform2D{.pos = {16.0f, 0.0f}});
    emitter.set(kin::AudioEmitter{.cue = "theme"});
    kin::update_audio_emitters(world, audio, catalog);
    auto* stored_emitter = emitter.raw().get_mut<kin::AudioEmitter>();
    assert(stored_emitter != nullptr);
    assert(stored_emitter->playing);
    assert(stored_emitter->handle);

    kin::EcsEntity one_shot = world.entity("one-shot");
    one_shot.set(kin::Transform2D{.pos = {16.0f, 0.0f}});
    one_shot.set(kin::AudioOneShot{.cue = "impact", .offset = {1.0f, 0.0f}, .priority_boost = 1});
    const kin::i32 before_one_shot = audio.active_voice_count();
    kin::consume_audio_one_shots(world, audio, catalog);
    assert(audio.active_voice_count() == before_one_shot + 1);
    assert(!one_shot.raw().has<kin::AudioOneShot>());

    kin::Clip clip{
        .duration = 0.1f,
        .events = {{
            .keys = {{
                .time = 0.1f,
                .event = {.channel = "sound", .name = "impact", .value = "impact", .offset = {2.0f, 0.0f}},
            }},
        }},
    };
    kin::AnimationRegistry animations;
    animations.add(kin::Animation{.name = "impact.event", .root = kin::clip_node(std::move(clip))});
    kin::AnimationPlayer player{.registry = &animations};
    kin::set_base(player, "impact.event");

    kin::EcsEntity animated = world.entity("animated");
    animated.set(kin::Transform2D{.pos = {16.0f, 0.0f}});
    animated.set(std::move(player));
    kin::advance_animation_players(world, 0.1f);
    const kin::i32 before_animation = audio.active_voice_count();
    kin::play_animation_audio_events(world, audio, catalog);
    assert(audio.active_voice_count() == before_animation + 1);
}

} // namespace

// A synthesized clip registered with add_clip plays through a cue with no
// catalog clip entry, mixes into the backend, and loops when the cue does.
void test_memory_clips() {
    kin::AudioEngine audio = make_test_engine();
    std::vector<kin::f32> samples(480 * 2);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        samples[i] = 0.25f * std::sin(static_cast<float>(i / 2) * 0.1f);
    }
    audio.add_clip("tone", kin::make_memory_audio_clip("tone", samples, 2, 48000));

    kin::AudioCatalog catalog;
    catalog.add_bus({.id = "sfx"});
    catalog.add_cue({.id = "blip", .clips = {"tone"}, .bus = "sfx"});
    catalog.add_cue({.id = "hum", .clips = {"tone"}, .category = kin::AudioCategory::Ambient, .bus = "sfx", .loop = true});
    catalog.add_cue({.id = "silence", .clips = {"nothing"}, .bus = "sfx"});

    const kin::AudioHandle blip = audio.play(catalog, {.cue = "blip"});
    const kin::AudioHandle hum = audio.play(catalog, {.cue = "hum"});
    assert(blip && hum);
    assert(!audio.play(catalog, {.cue = "silence"}));
    for (int i = 0; i < 10; ++i) {
        audio.update(1.0f / 60.0f); // 0.17 s: longer than the 10 ms clip
    }
    assert(audio.stats().mixed_frames > 480);
    assert(!audio.playing(blip));
    assert(audio.playing(hum));
}

namespace {

// A clip holding `value` in every sample: what the mixer outputs is then the
// gain it applied.
kin::AudioClip dc_clip(float value, kin::i32 frames = 48000, kin::i32 channels = 1, kin::i32 rate = 48000) {
    return kin::make_memory_audio_clip("dc", std::vector<kin::f32>(static_cast<std::size_t>(frames * channels), value),
                                       channels, rate);
}

// A mono ramp, sample i = i * step: the output's slope is the playback rate.
kin::AudioClip ramp_clip(kin::i32 frames, float step, kin::i32 rate = 48000) {
    std::vector<kin::f32> samples(static_cast<std::size_t>(frames));
    for (kin::i32 i = 0; i < frames; ++i) {
        samples[static_cast<std::size_t>(i)] = static_cast<float>(i) * step;
    }
    return kin::make_memory_audio_clip("ramp", std::move(samples), 1, rate);
}

std::vector<kin::f32> render(kin::AudioEngine& audio, kin::i32 frames) {
    std::vector<kin::f32> out(static_cast<std::size_t>(frames * audio.channels()));
    audio.render(out);
    return out;
}

bool near(float a, float b, float tolerance = 1e-4f) {
    return std::fabs(a - b) <= tolerance;
}

kin::AudioCatalog bus_catalog() {
    kin::AudioCatalog catalog;
    catalog.add_bus({.id = "master"});
    catalog.add_bus({.id = "sfx", .parent = "master"});
    catalog.add_bus({.id = "ui", .parent = "master"});
    catalog.add_cue({.id = "hum", .clips = {"dc"}, .bus = "sfx", .loop = true});
    catalog.add_cue({.id = "click", .clips = {"dc"}, .category = kin::AudioCategory::Ui, .bus = "ui", .loop = true});
    return catalog;
}

kin::AudioEngine null_engine(kin::AudioEngineConfig config = {}) {
    return kin::AudioEngine{kin::create_null_audio_backend(config.sample_rate, config.channels), config};
}

// set_bus_volume on a parent reaches the voices on its children, and volume
// changes ramp over a block instead of jumping.
void test_bus_volume_follows_hierarchy() {
    kin::AudioEngine audio = null_engine();
    audio.add_clip("dc", dc_clip(0.5f));
    const kin::AudioCatalog catalog = bus_catalog();
    assert(audio.play(catalog, {.cue = "hum"}));
    assert(near(render(audio, 64)[0], 0.5f));

    audio.set_bus_volume("master", 0.5f);
    assert(near(audio.effective_bus_volume(catalog, "sfx"), 0.5f));
    const std::vector<kin::f32> ramp = render(audio, 64);
    assert(ramp[2 * 32] < 0.4f && ramp[2 * 32] > 0.35f); // ramping down, no step
    assert(near(ramp[126], 0.25f, 0.01f));
    assert(near(render(audio, 64)[0], 0.25f));

    // Fades over time.
    audio.set_bus_volume("master", 1.0f, 0.1f);
    assert(near(audio.bus_volume("master"), 1.0f));
    const std::vector<kin::f32> halfway = render(audio, 2400);
    assert(near(halfway[halfway.size() - 2], 0.375f, 0.01f));
    render(audio, 2400);
    assert(near(render(audio, 64)[0], 0.5f));

    audio.set_bus_muted("master", true);
    render(audio, 64);
    assert(near(render(audio, 64)[0], 0.0f));
    assert(audio.effective_bus_volume(catalog, "sfx") == 0.0f);
}

// stop() fades out instead of cutting, and playing() is false straight away.
void test_stop_fades() {
    kin::AudioEngine audio = null_engine();
    audio.add_clip("dc", dc_clip(0.5f));
    const kin::AudioCatalog catalog = bus_catalog();

    const kin::AudioHandle hum = audio.play(catalog, {.cue = "hum"});
    render(audio, 64);
    audio.stop(hum, 0.1f);
    assert(!audio.playing(hum));
    assert(audio.active_voice_count() == 0);
    const std::vector<kin::f32> tail = render(audio, 2400);
    assert(tail[0] > 0.49f && near(tail[tail.size() - 2], 0.25f, 0.01f));
    render(audio, 2400);
    assert(audio.stats().active_voices == 0);
    assert(render(audio, 64)[0] == 0.0f);

    // Even an immediate stop takes a few milliseconds, so it does not click.
    audio.play(catalog, {.cue = "hum"});
    render(audio, 64);
    audio.stop_bus("master");
    const std::vector<kin::f32> cut = render(audio, 480);
    assert(cut[0] > 0.4f);
    assert(cut[2 * 120] > 0.0f && cut[2 * 120] < 0.5f);
    assert(cut[2 * 300] == 0.0f);

    // A fade-in starts from silence.
    audio.play(catalog, {.cue = "hum", .fade_in = 0.01f});
    const std::vector<kin::f32> in = render(audio, 960);
    assert(in[0] < 0.01f && near(in[2 * 240], 0.25f, 0.01f) && near(in[2 * 900], 0.5f));
}

// A paused bus goes quiet and holds its voices' place; the other buses play on.
void test_bus_pause() {
    kin::AudioEngine audio = null_engine();
    audio.add_clip("dc", ramp_clip(48000, 1.0f / 48000.0f));
    kin::AudioCatalog catalog = bus_catalog();
    const kin::AudioHandle hum = audio.play(catalog, {.cue = "hum"});
    render(audio, 1000);

    audio.set_bus_paused("sfx", true);
    assert(audio.bus_paused("sfx"));
    render(audio, 100); // fades out within this block
    const std::vector<kin::f32> paused = render(audio, 1000);
    assert(std::ranges::all_of(paused, [](kin::f32 v) { return v == 0.0f; }));
    assert(audio.playing(hum));

    audio.set_bus_paused("sfx", false);
    render(audio, 100); // fades back in
    const std::vector<kin::f32> resumed = render(audio, 1);
    // Paused for 1000 frames, so the ramp is at frame 1200, not 2200.
    assert(near(resumed[0], 1200.0f / 48000.0f, 1e-4f));
}

// Clips play at their own sample rate, interpolated, and pitch scales the rate.
void test_resampling_and_pitch() {
    kin::AudioEngine audio = null_engine();
    audio.add_clip("slow", ramp_clip(100, 0.01f, 24000));
    kin::AudioCatalog catalog;
    catalog.add_cue({.id = "slow", .clips = {"slow"}, .bus = "sfx"});
    catalog.add_cue({.id = "fast", .clips = {"slow"}, .bus = "sfx", .pitch = 2.0f});

    audio.play(catalog, {.cue = "slow"});
    const std::vector<kin::f32> out = render(audio, 300);
    for (int frame = 0; frame < 198; ++frame) {
        assert(near(out[static_cast<std::size_t>(frame) * 2], 0.005f * static_cast<float>(frame)));
    }
    assert(out[2 * 210] == 0.0f);
    assert(audio.active_voice_count() == 0);

    // Twice the pitch of a 24 kHz clip on a 48 kHz output: one source frame per output frame.
    audio.play(catalog, {.cue = "fast"});
    const std::vector<kin::f32> fast = render(audio, 50);
    assert(near(fast[2 * 40], 0.40f));
}

// Cue variations: a random clip, never the same one twice running, and a random
// pitch, both the same for the same seed.
void test_variation_is_seeded() {
    const auto run = [](kin::u64 seed) {
        kin::AudioEngine audio = null_engine({.seed = seed});
        audio.add_clip("a", dc_clip(0.1f, 64));
        audio.add_clip("b", dc_clip(0.2f, 64));
        audio.add_clip("c", dc_clip(0.3f, 64));
        audio.add_clip("ramp", ramp_clip(64, 0.001f));
        kin::AudioCatalog catalog;
        catalog.add_cue({.id = "step", .clips = {"a", "b", "c"}, .bus = "sfx"});
        catalog.add_cue({.id = "zap", .clips = {"ramp"}, .bus = "sfx", .pitch_variance = 0.2f});
        std::vector<float> picks;
        std::vector<float> pitches;
        for (int i = 0; i < 24; ++i) {
            audio.play(catalog, {.cue = "step"});
            picks.push_back(render(audio, 64)[0]);
            audio.play(catalog, {.cue = "zap"});
            pitches.push_back(render(audio, 64)[2 * 10] / 0.010f);
        }
        return std::pair{picks, pitches};
    };

    const auto [picks, pitches] = run(7);
    assert(run(7) == std::pair(picks, pitches));
    assert(run(8).first != picks);
    for (std::size_t i = 1; i < picks.size(); ++i) {
        assert(picks[i] != picks[i - 1]);
    }
    for (const float clip : {0.1f, 0.2f, 0.3f}) {
        assert(std::ranges::any_of(picks, [&](float v) { return near(v, clip); }));
    }
    assert(std::ranges::all_of(pitches, [](float p) { return p >= 0.79f && p <= 1.21f; }));
    assert(std::ranges::any_of(pitches, [](float p) { return p < 0.95f; }));
    assert(std::ranges::any_of(pitches, [](float p) { return p > 1.05f; }));
}

// Many loud voices are turned down to fit instead of clipping.
void test_limiter() {
    kin::AudioEngine audio = null_engine();
    audio.add_clip("dc", dc_clip(0.5f));
    const kin::AudioCatalog catalog = bus_catalog();
    for (int i = 0; i < 6; ++i) {
        audio.play(catalog, {.cue = "hum"});
    }
    const std::vector<kin::f32> out = render(audio, 4800);
    assert(std::ranges::all_of(out, [](kin::f32 v) { return v <= 1.0f && v >= -1.0f; }));
    assert(audio.stats().limited_frames > 0);
}

// Equal-power panning: the power of a panned sound matches a centred one.
void test_equal_power_pan() {
    kin::AudioEngine audio = null_engine();
    audio.add_clip("dc", dc_clip(0.5f));
    kin::AudioCatalog catalog;
    catalog.add_cue({.id = "near", .clips = {"dc"}, .bus = "sfx", .loop = true, .spatial = true,
                     .min_distance = 1000.0f, .max_distance = 2000.0f});
    audio.set_listener({0.0f, 0.0f});
    audio.play(catalog, {.cue = "near", .position = {1000.0f, 0.0f}, .has_position = true});
    const std::vector<kin::f32> out = render(audio, 16);
    const float left = out[0];
    const float right = out[1];
    assert(right > left);
    assert(near(left * left + right * right, 2.0f * 0.25f, 1e-3f));
}

// Mono output averages the two sides.
void test_mono_output() {
    kin::AudioEngine audio = null_engine({.channels = 1});
    assert(audio.channels() == 1);
    audio.add_clip("dc", dc_clip(0.5f, 48000, 2));
    const kin::AudioCatalog catalog = bus_catalog();
    audio.play(catalog, {.cue = "hum"});
    const std::vector<kin::f32> out = render(audio, 16);
    assert(out.size() == 16 && near(out[8], 0.5f));
}

// preload_async decodes on workers; play() waits for a clip still loading
// rather than decoding it again.
void test_preload_async() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "kin-audio-tests-preload";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    write_wav(dir / "beep.wav");
    write_wav(dir / "boop.wav", 12000);
    const kin::AudioCatalog catalog = make_catalog(dir);

    std::vector<kin::LogEvent> log_events;
    kin::set_logger_config({.min_level = kin::LogLevel::Debug, .format = kin::LogFormat::Text, .sdl_sink = false,
                            .memory_events = &log_events});
    kin::JobSystem jobs{{.workers = 2}};
    kin::AudioEngine audio = null_engine();
    audio.preload_async(catalog, jobs);
    assert(audio.play(catalog, {.cue = "impact"}));
    audio.update(0.0f);
    jobs.drain();
    audio.update(0.0f);
    assert(audio.play(catalog, {.cue = "step", .has_position = true}));
    const auto loads = std::ranges::count_if(log_events, [](const kin::LogEvent& event) {
        return event.message == "audio clip loaded";
    });
    kin::set_logger_config({.min_level = kin::LogLevel::Debug, .format = kin::LogFormat::Text, .sdl_sink = false});
    // Each file decoded once: impact's clip was waited for, beep picked up by update.
    assert(loads == 2);

    kin::AudioEngine moved = std::move(audio);
    assert(moved.active_voice_count() == 2);
}

// With a device backend the device's audio thread pulls the mix: voices finish
// with no update() calls, while the game thread keeps changing them. SDL's
// dummy driver pulls in real time on its own thread, like a sound card.
void test_device_thread_mixes() {
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    std::unique_ptr<kin::IAudioBackend> backend;
    try {
        backend = kin::create_sdl_audio_backend();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "device test skipped: %s\n", error.what());
        return; // no dummy driver in this SDL build
    }
    kin::AudioEngine audio{std::move(backend)};
    assert(audio.device_available());
    audio.add_clip("dc", dc_clip(0.2f, 2400));
    kin::AudioCatalog catalog = bus_catalog();
    catalog.add_cue({.id = "blip", .clips = {"dc"}, .bus = "sfx", .spatial = true});

    const kin::AudioHandle blip = audio.play(catalog, {.cue = "blip"});
    for (int i = 0; i < 2000; ++i) {
        const kin::AudioHandle h = audio.play(catalog, {.cue = "hum"});
        audio.set_position(blip, {static_cast<float>(i), 0.0f});
        audio.set_bus_volume("master", static_cast<float>(i % 3) * 0.5f, 0.01f);
        audio.stop(h, 0.001f * static_cast<float>(i % 5));
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while ((audio.playing(blip) || audio.stats().active_voices > 0) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    assert(!audio.playing(blip));
    assert(audio.stats().mixed_frames > 2400);
}

const std::filesystem::path fixtures{KIN_AUDIO_FIXTURES};

float rms(const kin::AudioClip& clip, int channel) {
    double sum = 0.0;
    const auto samples = clip.samples();
    for (kin::i32 frame = 0; frame < clip.frame_count(); ++frame) {
        const double v = samples[static_cast<std::size_t>(frame * clip.channels() + channel)];
        sum += v * v;
    }
    return static_cast<float>(std::sqrt(sum / clip.frame_count()));
}

// Upward zero crossings per second on one channel: its frequency.
float frequency(const kin::AudioClip& clip, int channel) {
    const auto samples = clip.samples();
    int crossings = 0;
    for (kin::i32 frame = 1; frame < clip.frame_count(); ++frame) {
        const float a = samples[static_cast<std::size_t>((frame - 1) * clip.channels() + channel)];
        const float b = samples[static_cast<std::size_t>(frame * clip.channels() + channel)];
        crossings += a < 0.0f && b >= 0.0f ? 1 : 0;
    }
    return static_cast<float>(crossings) * static_cast<float>(clip.sample_rate()) / static_cast<float>(clip.frame_count());
}

// Each format decodes to the tone it holds: 0.5 s at 44.1 kHz, a 440 Hz sine at
// 0.5 on the left and a 660 Hz sine at 0.25 on the right.
void test_decodes_every_format() {
    for (const char* name : {"tone.ogg", "tone.mp3", "tone.flac"}) {
        const kin::AudioClip clip = kin::load_audio_clip(fixtures / name);
        assert(clip.valid() && !clip.streamed());
        assert(clip.channels() == 2);
        assert(clip.sample_rate() == 44100);
        assert(std::abs(clip.frame_count() - 22050) <= 1152); // an MP3 may pad a frame
        assert(near(rms(clip, 0), 0.5f / std::sqrt(2.0f), 0.02f));
        assert(near(rms(clip, 1), 0.25f / std::sqrt(2.0f), 0.02f));
        assert(near(frequency(clip, 0), 440.0f, 10.0f));
        assert(near(frequency(clip, 1), 660.0f, 10.0f));
    }

    // Streamed, the file stays compressed and a decoder reads it.
    const kin::AudioClip stream = kin::load_audio_stream(fixtures / "tone.ogg");
    assert(stream.valid() && stream.streamed() && stream.samples().empty());
    assert(stream.frame_count() == kin::load_audio_clip(fixtures / "tone.ogg").frame_count());
    auto decoder = stream.open_stream();
    assert(decoder && decoder->channels() == 2 && decoder->sample_rate() == 44100);

    // Formats kin cannot read fail with a reason.
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "kin-audio-tests-formats";
    std::filesystem::create_directories(dir);
    {
        std::ofstream opus(dir / "voice.opus", std::ios::binary);
        std::string page(64, '\0');
        page.replace(0, 4, "OggS");
        page.replace(28, 8, "OpusHead");
        opus << page;
        std::ofstream text(dir / "notes.wav");
        text << "not audio";
    }
    for (const char* name : {"voice.opus", "notes.wav", "missing.ogg"}) {
        bool threw = false;
        try {
            kin::load_audio_clip(dir / name);
        } catch (const std::runtime_error& error) {
            threw = std::string_view{error.what()}.find(name) != std::string_view::npos;
        }
        assert(threw);
    }
}

// A 48 kHz mono WAV whose sample i is (i % 4096) / 4096, so the output names
// the source frame it came from.
std::filesystem::path write_ramp_wav(const std::filesystem::path& path, kin::i32 frames) {
    std::ofstream out(path, std::ios::binary);
    const kin::u32 data_bytes = static_cast<kin::u32>(frames * 2);
    out.write("RIFF", 4);
    write_u32(out, 36 + data_bytes);
    out.write("WAVEfmt ", 8);
    write_u32(out, 16);
    write_u16(out, 1);
    write_u16(out, 1);
    write_u32(out, 48000);
    write_u32(out, 96000);
    write_u16(out, 2);
    write_u16(out, 16);
    out.write("data", 4);
    write_u32(out, data_bytes);
    for (kin::i32 i = 0; i < frames; ++i) {
        write_u16(out, static_cast<kin::u16>((i % 4096) * 8));
    }
    return path;
}

int source_frame(float sample) {
    return static_cast<int>(std::lround(sample * 4096.0f));
}

// Loop points: a looping cue plays its intro once, then repeats from loop_start
// to loop_end; a streamed clip loops the same as a decoded one.
void test_loop_points_and_streaming() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "kin-audio-tests-loops";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    write_ramp_wav(dir / "ramp.wav", 3000);

    for (const bool stream : {false, true}) {
        kin::AudioCatalog catalog;
        catalog.set_root(dir);
        catalog.add_clip({.id = "ramp", .path = "ramp.wav", .stream = stream, .loop_start = 1000, .loop_end = 2000});
        catalog.add_cue({.id = "theme", .clips = {"ramp"}, .category = kin::AudioCategory::Music, .bus = "music",
                         .loop = true});
        catalog.add_cue({.id = "once", .clips = {"ramp"}, .bus = "sfx"});
        kin::AudioEngine audio = null_engine();
        assert(audio.play(catalog, {.cue = "theme"}));
        const std::vector<kin::f32> out = render(audio, 5000);
        for (int frame = 0; frame < 5000; ++frame) {
            const int expected = frame < 2000 ? frame : 1000 + (frame - 2000) % 1000;
            assert(source_frame(out[static_cast<std::size_t>(frame) * 2]) == expected);
        }

        // Without loop, the loop points do not cut the clip short.
        audio.stop_bus("music");
        render(audio, 480);
        const kin::AudioHandle once = audio.play(catalog, {.cue = "once"});
        const std::vector<kin::f32> whole = render(audio, 3100);
        assert(source_frame(whole[2 * 2999]) == 2999);
        assert(whole[2 * 3050] == 0.0f);
        assert(!audio.playing(once));
    }
}

// A streamed and a decoded copy of a file sound the same, sample for sample.
void test_stream_matches_decoded() {
    for (const char* name : {"tone.ogg", "tone.flac", "tone.mp3"}) {
        std::vector<std::vector<kin::f32>> outputs;
        for (const bool stream : {false, true}) {
            kin::AudioCatalog catalog;
            catalog.set_root(fixtures);
            catalog.add_clip({.id = "tone", .path = name, .stream = stream});
            catalog.add_cue({.id = "tone", .clips = {"tone"}, .bus = "sfx"});
            kin::AudioEngine audio = null_engine();
            assert(audio.play(catalog, {.cue = "tone"}));
            outputs.push_back(render(audio, 30000));
        }
        assert(outputs[0] == outputs[1]);
        assert(std::ranges::any_of(outputs[0], [](kin::f32 v) { return v > 0.4f; }));
    }
}

void test_catalog_clip_options() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "kin-audio-tests-clip-options";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    kin::AudioCatalog catalog;
    catalog.add_bus({.id = "music"});
    catalog.add_clip({.id = "theme", .path = "theme.ogg", .stream = true, .loop_start = 44100, .loop_end = 441000});
    catalog.add_clip({.id = "hit", .path = "hit.wav"});
    catalog.add_cue({.id = "theme", .clips = {"theme"}, .category = kin::AudioCategory::Music, .bus = "music"});
    assert(kin::save_audio_catalog(catalog, dir / "audio.kinaudio"));
    const kin::AudioCatalog loaded = kin::load_audio_catalog(dir / "audio.kinaudio");
    const kin::AudioClipRef* theme = loaded.clip("theme");
    assert(theme && theme->stream && theme->loop_start == 44100 && theme->loop_end == 441000);
    assert(!loaded.clip("hit")->stream && loaded.clip("hit")->loop_end == 0);

    std::ofstream(dir / "bad.kinaudio") << "clip theme theme.ogg loop_start=10 loop_end=5\n";
    bool threw = false;
    try {
        kin::load_audio_catalog(dir / "bad.kinaudio");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);
}

// A duck rule turns its bus down while its trigger bus plays, and back up after.
void test_ducking() {
    kin::AudioEngine audio = null_engine();
    audio.add_clip("music", dc_clip(0.5f));
    audio.add_clip("silent", dc_clip(0.0f, 4800));
    kin::AudioCatalog catalog;
    catalog.add_bus({.id = "master"});
    catalog.add_bus({.id = "music", .parent = "master"});
    catalog.add_bus({.id = "dialogue", .parent = "master"});
    catalog.add_bus({.id = "lines", .parent = "dialogue"});
    catalog.add_cue({.id = "theme", .clips = {"music"}, .category = kin::AudioCategory::Music, .bus = "music", .loop = true});
    catalog.add_cue({.id = "line", .clips = {"silent"}, .bus = "lines"});
    catalog.add_duck({.bus = "music", .when = "dialogue", .volume = 0.25f, .attack = 0.01f, .release = 0.02f});

    audio.play(catalog, {.cue = "theme"});
    assert(near(render(audio, 64)[0], 0.5f));
    const kin::AudioHandle line = audio.play(catalog, {.cue = "line"}); // on a bus under "dialogue"
    render(audio, 960); // attack: 10 ms
    assert(near(render(audio, 64)[0], 0.125f, 1e-3f));

    audio.stop(line);
    render(audio, 240); // the stop's declick
    const std::vector<kin::f32> releasing = render(audio, 480);
    assert(releasing[0] > 0.125f && releasing[0] < 0.5f);
    render(audio, 960);
    assert(near(render(audio, 64)[0], 0.5f, 1e-3f));
}

// One music cue at a time: the same cue keeps playing, another crossfades.
void test_music() {
    kin::AudioEngine audio = null_engine();
    audio.add_clip("a", dc_clip(0.2f));
    audio.add_clip("b", dc_clip(0.4f));
    kin::AudioCatalog catalog;
    catalog.add_bus({.id = "music"});
    catalog.add_cue({.id = "town", .clips = {"a"}, .category = kin::AudioCategory::Music, .bus = "music", .loop = true});
    catalog.add_cue({.id = "cave", .clips = {"b"}, .category = kin::AudioCategory::Music, .bus = "music", .loop = true});

    const kin::AudioHandle town = audio.play_music(catalog, "town", 0.0f);
    assert(town && audio.music() == town);
    assert(near(render(audio, 64)[0], 0.2f));
    assert(audio.play_music(catalog, "town") == town);

    const kin::AudioHandle cave = audio.play_music(catalog, "cave", 0.1f);
    assert(cave != town && audio.music() == cave && !audio.playing(town));
    const std::vector<kin::f32> crossfade = render(audio, 2400);
    assert(near(crossfade[2 * 2399], 0.1f + 0.2f, 0.01f)); // halfway: half of each
    render(audio, 2400);
    assert(near(render(audio, 64)[0], 0.4f));
    assert(audio.active_voice_count() == 1);

    audio.stop_music(0.0f);
    assert(!audio.music());
    render(audio, 480);
    assert(render(audio, 64)[0] == 0.0f);
}

// set_volume fades a voice, set_pitch changes its rate, and playback_position
// says how far it got.
void test_voice_controls() {
    kin::AudioEngine audio = null_engine();
    audio.add_clip("dc", dc_clip(0.5f));
    audio.add_clip("ramp", ramp_clip(48000, 0.00001f));
    kin::AudioCatalog catalog = bus_catalog();
    catalog.add_cue({.id = "half", .clips = {"dc"}, .bus = "sfx", .volume = 0.5f, .loop = true});
    catalog.add_cue({.id = "ramp", .clips = {"ramp"}, .bus = "sfx"});

    const kin::AudioHandle hum = audio.play(catalog, {.cue = "half"});
    assert(near(render(audio, 64)[0], 0.25f));
    audio.set_volume(hum, 2.0f, 0.1f); // relative to the cue's 0.5
    const std::vector<kin::f32> fading = render(audio, 2400);
    assert(fading[2 * 2399] > 0.3f && fading[2 * 2399] < 0.45f);
    render(audio, 2400);
    assert(near(render(audio, 64)[0], 0.5f));
    audio.stop(hum, 0.0f);
    render(audio, 480);

    const kin::AudioHandle ramp = audio.play(catalog, {.cue = "ramp"});
    render(audio, 4800);
    assert(near(audio.playback_position(ramp), 0.1f, 0.001f));
    audio.set_pitch(ramp, 2.0f);
    const std::vector<kin::f32> fast = render(audio, 101);
    assert(near(fast[2 * 100] - fast[0], 200 * 0.00001f, 1e-5f));
    assert(near(audio.playback_position(ramp), (4800.0f + 202.0f) / 48000.0f, 0.001f));
    assert(audio.playback_position(kin::AudioHandle{999}) == 0.0f);
}

// Bus volumes and mutes go into a settings file and come back.
void test_settings_and_report() {
    kin::AudioEngine audio = null_engine();
    audio.add_clip("dc", dc_clip(0.5f));
    const kin::AudioCatalog catalog = bus_catalog();
    audio.play(catalog, {.cue = "hum"});
    audio.set_bus_volume("master", 0.5f);
    audio.set_bus_muted("ui", true);
    audio.set_bus_paused("sfx", true);

    std::ostringstream text;
    {
        kin::JsonWriter json{text, false};
        json.begin_object();
        json.key("audio");
        kin::write_audio_settings(json, audio);
        json.end_object();
    }
    const kin::JsonParseResult parsed = kin::parse_json(text.str());
    assert(parsed.ok());
    const kin::JsonValue* settings = parsed.value->find("audio");
    assert(settings && settings->is_object());
    assert(!settings->contains("device")); // the default device is not written
    const kin::JsonValue* saved_buses = settings->find("buses");
    assert(saved_buses && saved_buses->contains("master") && saved_buses->contains("ui"));
    assert(!saved_buses->contains("sfx")); // a pause is not a setting

    kin::AudioEngine restored = null_engine();
    kin::apply_audio_settings(restored, *settings);
    assert(near(restored.bus_volume("master"), 0.5f));
    assert(restored.bus_muted("ui") && !restored.bus_paused("sfx"));
    assert(near(restored.effective_bus_volume(catalog, "sfx"), 0.5f));
    assert(restored.effective_bus_volume(catalog, "ui") == 0.0f);

    std::ostringstream report_text;
    {
        kin::JsonWriter json{report_text, false};
        audio.write_report(json);
    }
    const kin::JsonParseResult report = kin::parse_json(report_text.str());
    assert(report.ok());
    assert(report.value->find("stats")->int_at("played_requests") == 1);
    assert(report.value->find("voices")->items().size() == 1);
    assert(report.value->find("voices")->items()[0].string_at("cue") == "hum");
    assert(report.value->find("music")->is_null());
    const auto& buses = report.value->find("buses")->items();
    assert(std::ranges::any_of(buses, [](const kin::JsonValue& bus) {
        return bus.string_at("id") == "sfx" && bus.bool_at("paused");
    }));
}

// Lua scripts drive the engine through bind_lua_audio, in a LuaScript and in a
// ScriptScene's engine.
void test_lua_audio() {
    kin::AudioEngine audio = null_engine();
    audio.add_clip("dc", dc_clip(0.5f));
    kin::AudioCatalog catalog = bus_catalog();
    catalog.add_cue({.id = "theme", .clips = {"dc"}, .category = kin::AudioCategory::Music, .bus = "ui", .loop = true});

    kin::LuaScript script{{.setup = [&](sol::state& lua) { kin::bind_lua_audio(lua, audio, catalog); }}};
    assert(script.load_string(R"(
        function start()
            hum = audio.play("hum", {volume = 0.5, x = 10, y = 20, fade_in = 0})
            audio.play_music("theme", 0)
            audio.set_bus_volume("master", 0.8, 0.5)
            audio.set_bus_paused("sfx", true)
            return hum
        end
        function muffle()
            audio.set_bus_effects("sfx", {{type = "lowpass", cutoff = 800}, {type = "reverb", wet = 0.2}})
            audio.set_bus_effect("sfx", 1, {cutoff = 300})
        end
        function finish()
            audio.stop(hum)
            audio.stop_music(0)
            return audio.playing(hum)
        end
    )"));
    const auto hum = script.call_for<kin::i64>("start");
    assert(hum && *hum > 0);
    assert(audio.playing(kin::AudioHandle{static_cast<kin::u64>(*hum)}));
    assert(audio.music());
    assert(near(audio.bus_volume("master"), 0.8f) && audio.bus_paused("sfx"));
    assert(script.call("muffle"));
    const std::vector<kin::AudioEffect> effects = audio.bus_effects("sfx");
    assert(effects.size() == 2 && effects[0].type == kin::AudioEffectType::LowPass && near(effects[0].cutoff, 300.0f));
    assert(effects[1].type == kin::AudioEffectType::Reverb && near(effects[1].wet, 0.2f));
    assert(script.call_for<bool>("finish") == false);
    assert(!audio.music());

    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "kin-audio-tests-lua";
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "scene.lua") << "assert(audio and audio.play, 'audio not bound')\n";
    kin::ScriptEngine bound{[&](sol::state& lua) { kin::bind_lua_audio(lua, audio, catalog); }};
    assert(bound.load_file(dir / "scene.lua"));
    kin::ScriptEngine unbound;
    assert(!unbound.load_file(dir / "scene.lua"));
}

void test_catalog_ducks_roundtrip() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "kin-audio-tests-ducks";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    kin::AudioCatalog catalog;
    catalog.add_duck({.bus = "music", .when = "dialogue", .volume = 0.3f, .attack = 0.2f, .release = 1.0f});
    catalog.add_duck({.bus = "music", .when = "dialogue", .volume = 0.4f}); // replaces the first
    catalog.add_duck({.bus = "ambient", .when = "dialogue"});
    assert(catalog.ducks().size() == 2);
    assert(kin::save_audio_catalog(catalog, dir / "audio.kinaudio"));
    const kin::AudioCatalog loaded = kin::load_audio_catalog(dir / "audio.kinaudio");
    assert(loaded.ducks() == catalog.ducks());

    std::ofstream(dir / "bad.kinaudio") << "duck music volume=0.5\n";
    bool threw = false;
    try {
        kin::load_audio_catalog(dir / "bad.kinaudio");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);
}

kin::AudioClip sine_clip(float hz, float amplitude = 0.5f, kin::i32 frames = 48000) {
    std::vector<kin::f32> samples(static_cast<std::size_t>(frames));
    for (kin::i32 i = 0; i < frames; ++i) {
        samples[static_cast<std::size_t>(i)] = amplitude * std::sin(6.2831853f * hz * static_cast<float>(i) / 48000.0f);
    }
    return kin::make_memory_audio_clip("sine", std::move(samples), 1, 48000);
}

// Root-mean-square of the left channel over [from, to) frames.
float rms_left(const std::vector<kin::f32>& out, int from, int to) {
    double sum = 0.0;
    for (int frame = from; frame < to; ++frame) {
        sum += static_cast<double>(out[static_cast<std::size_t>(frame) * 2]) * out[static_cast<std::size_t>(frame) * 2];
    }
    return static_cast<float>(std::sqrt(sum / (to - from)));
}

// RMS of `hz` played on "sfx" with `effects` on `bus`.
float filtered_rms(float hz, std::vector<kin::AudioEffect> effects, std::string_view bus = "sfx") {
    kin::AudioEngine audio = null_engine();
    audio.add_clip("dc", sine_clip(hz));
    const kin::AudioCatalog catalog = bus_catalog();
    audio.play(catalog, {.cue = "hum"});
    audio.set_bus_effects(bus, std::move(effects));
    return rms_left(render(audio, 9600), 4800, 9600);
}

// Low- and high-pass filters keep their band and cut the other, on the bus
// they are on and on everything under it.
void test_filters() {
    const float full = 0.5f / std::sqrt(2.0f);
    const kin::AudioEffect low{.type = kin::AudioEffectType::LowPass, .cutoff = 500.0f};
    const kin::AudioEffect high{.type = kin::AudioEffectType::HighPass, .cutoff = 2000.0f};
    assert(near(filtered_rms(100.0f, {low}), full, 0.02f));
    assert(filtered_rms(8000.0f, {low}) < 0.01f);
    assert(filtered_rms(8000.0f, {low}, "master") < 0.01f);
    assert(filtered_rms(100.0f, {high}) < 0.01f);
    assert(near(filtered_rms(8000.0f, {high}), full, 0.02f));
    assert(near(filtered_rms(8000.0f, {{.type = kin::AudioEffectType::LowPass, .enabled = false, .cutoff = 500.0f}}),
                full, 0.01f));
    assert(near(filtered_rms(8000.0f, {low}, "ui"), full, 0.01f)); // another branch
}

// A reverb rings on after its input stops, and dies away.
void test_reverb_tail() {
    kin::AudioEngine audio = null_engine();
    audio.add_clip("dc", dc_clip(0.5f, 480));
    kin::AudioCatalog catalog = bus_catalog();
    catalog.add_cue({.id = "click", .clips = {"dc"}, .bus = "sfx"});
    audio.set_bus_effects("sfx", {{.type = kin::AudioEffectType::Reverb, .room_size = 0.8f, .wet = 0.5f, .dry = 0.0f}});
    audio.play(catalog, {.cue = "click"});
    const std::vector<kin::f32> out = render(audio, 96000);
    assert(audio.active_voice_count() == 0);
    const float early = rms_left(out, 2000, 24000);
    const float late = rms_left(out, 72000, 96000);
    assert(early > 0.001f);
    assert(late < early * 0.5f);
    assert(std::ranges::all_of(out, [](kin::f32 v) { return std::isfinite(v) && std::fabs(v) <= 1.0f; }));
}

// A compressor brings a loud signal down by its ratio above the threshold.
void test_compressor() {
    kin::AudioEngine audio = null_engine();
    audio.add_clip("dc", dc_clip(0.9f));
    const kin::AudioCatalog catalog = bus_catalog();
    audio.play(catalog, {.cue = "hum"});
    audio.set_bus_effects("master", {{.type = kin::AudioEffectType::Compressor, .threshold = -12.0f, .ratio = 4.0f,
                                      .attack = 0.001f, .release = 0.1f}});
    const std::vector<kin::f32> out = render(audio, 4800);
    // 0.9 is -0.92 dB: 11.08 dB over, brought down to 2.77 dB over: -9.23 dB.
    assert(near(out[2 * 4799], 0.346f, 0.005f));
    assert(out[0] > out[2 * 4799]); // the attack takes a moment
}

// Effects change while playing, game chains outrank the catalog's, and the
// catalog file carries them.
void test_effect_changes_and_catalog() {
    kin::AudioEngine audio = null_engine();
    audio.add_clip("dc", sine_clip(4000.0f));
    kin::AudioCatalog catalog = bus_catalog();
    catalog.bus("sfx")->effects.push_back({.type = kin::AudioEffectType::LowPass, .cutoff = 200.0f});
    audio.play(catalog, {.cue = "hum"});
    assert(audio.bus_effects("sfx").size() == 1);
    assert(rms_left(render(audio, 4800), 2400, 4800) < 0.01f);

    // Sweep the cutoff open: the same filter, gliding.
    audio.set_bus_effect("sfx", 0, {.type = kin::AudioEffectType::LowPass, .cutoff = 20000.0f});
    render(audio, 9600);
    assert(rms_left(render(audio, 4800), 0, 4800) > 0.3f);
    assert(near(audio.bus_effects("sfx")[0].cutoff, 20000.0f));

    // Another type replaces it.
    audio.set_bus_effect("sfx", 0, {.type = kin::AudioEffectType::HighPass, .cutoff = 100.0f});
    assert(audio.bus_effects("sfx")[0].type == kin::AudioEffectType::HighPass);

    // The game's chain stays when the catalog is seen again.
    audio.set_bus_effects("sfx", {});
    audio.play(catalog, {.cue = "hum"});
    assert(audio.bus_effects("sfx").empty());

    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "kin-audio-tests-effects";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    catalog.bus("master")->effects.push_back({.type = kin::AudioEffectType::Compressor, .threshold = -6.0f});
    catalog.bus("ui")->effects.push_back({.type = kin::AudioEffectType::Reverb, .enabled = false, .room_size = 0.7f});
    assert(kin::save_audio_catalog(catalog, dir / "audio.kinaudio"));
    const kin::AudioCatalog loaded = kin::load_audio_catalog(dir / "audio.kinaudio");
    for (const char* bus : {"master", "sfx", "ui"}) {
        assert(loaded.bus(bus)->effects == catalog.bus(bus)->effects);
    }

    std::ofstream(dir / "bad.kinaudio") << "effect nowhere lowpass cutoff=100\n";
    bool threw = false;
    try {
        kin::load_audio_catalog(dir / "bad.kinaudio");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);
}

// seek() jumps within a clip, fading out and back in so the jump does not
// click; a voice that has not sounded yet jumps straight away, streamed too.
void test_seek() {
    kin::AudioEngine audio = null_engine();
    audio.add_clip("ramp", ramp_clip(48000, 1.0f / 48000.0f));
    kin::AudioCatalog catalog = bus_catalog();
    catalog.add_cue({.id = "ramp", .clips = {"ramp"}, .bus = "sfx"});
    const kin::AudioHandle ramp = audio.play(catalog, {.cue = "ramp"});
    render(audio, 1000);
    assert(audio.seek(ramp, 0.5f));
    assert(near(audio.playback_position(ramp), 0.5f, 1e-4f));
    const std::vector<kin::f32> jump = render(audio, 480);
    float largest_step = 0.0f;
    for (std::size_t frame = 1; frame < 480; ++frame) {
        largest_step = std::max(largest_step, std::fabs(jump[frame * 2] - jump[(frame - 1) * 2]));
    }
    assert(largest_step < 0.01f); // a cut from 0.02 to 0.5 would step by 0.48
    // Faded out over 240 frames, jumped, and played 240 frames from 0.5 s.
    assert(near(render(audio, 1)[0], (24000.0f + 240.0f) / 48000.0f, 5.0f / 48000.0f));
    assert(!audio.seek(kin::AudioHandle{12345}, 0.1f));

    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "kin-audio-tests-seek";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    write_ramp_wav(dir / "ramp.wav", 3000);
    for (const bool stream : {false, true}) {
        kin::AudioCatalog files;
        files.set_root(dir);
        files.add_clip({.id = "ramp", .path = "ramp.wav", .stream = stream});
        files.add_cue({.id = "ramp", .clips = {"ramp"}, .bus = "sfx"});
        kin::AudioEngine engine = null_engine();
        const kin::AudioHandle handle = engine.play(files, {.cue = "ramp"});
        assert(engine.seek(handle, 2000.0f / 48000.0f)); // not heard yet: no fade
        assert(source_frame(render(engine, 1)[0]) == 2000);
    }
}

// set_paused holds one voice where it is while others play on.
void test_pause_one_voice() {
    kin::AudioEngine audio = null_engine();
    audio.add_clip("ramp", ramp_clip(48000, 1.0f / 48000.0f));
    audio.add_clip("dc", dc_clip(0.25f));
    kin::AudioCatalog catalog = bus_catalog();
    catalog.add_cue({.id = "ramp", .clips = {"ramp"}, .bus = "sfx"});
    const kin::AudioHandle ramp = audio.play(catalog, {.cue = "ramp"});
    const kin::AudioHandle hum = audio.play(catalog, {.cue = "hum"});
    render(audio, 1000);

    audio.set_paused(ramp, true);
    assert(audio.paused(ramp) && audio.playing(ramp) && !audio.paused(hum));
    render(audio, 100);
    const std::vector<kin::f32> held = render(audio, 1000);
    assert(std::ranges::all_of(held, [](kin::f32 v) { return near(v, 0.25f); })); // just the hum

    audio.set_paused(ramp, false);
    render(audio, 100);
    // Held for 1000 frames: the ramp is at frame 1200, not 2200.
    assert(near(render(audio, 1)[0] - 0.25f, 1200.0f / 48000.0f, 1e-4f));
}

// A clip backend that records the thread it was freed on.
struct TrackedBackend final : kin::IAudioClipBackend {
    std::vector<kin::f32> data = std::vector<kin::f32>(480, 0.1f);
    std::thread::id* freed_on = nullptr;
    ~TrackedBackend() override { *freed_on = std::this_thread::get_id(); }
    std::span<const kin::f32> samples() const override { return data; }
};

// Cached clips are freed when asked, never while a voice plays them, and on
// the game thread rather than the audio thread.
void test_unloading() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "kin-audio-tests-unload";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    write_wav(dir / "beep.wav");
    write_wav(dir / "boop.wav");
    const kin::AudioCatalog catalog = make_catalog(dir);

    kin::AudioEngine audio = null_engine();
    audio.preload(catalog);
    assert(audio.loaded_clip_count() == 2);
    assert(audio.loaded_clip_bytes() == 2 * 64 * sizeof(kin::f32)); // two 64-frame mono clips
    const kin::AudioHandle theme = audio.play(catalog, {.cue = "theme"}); // loops "beep"
    assert(audio.unload_unused() == 1); // boop
    assert(audio.loaded_clip_count() == 1);
    audio.stop(theme);
    render(audio, 480);
    assert(audio.unload_unused() == 1);
    assert(audio.loaded_clip_count() == 0 && audio.loaded_clip_bytes() == 0);

    // unload() drops a catalog's clips at once; playing voices keep theirs.
    const kin::AudioHandle again = audio.play(catalog, {.cue = "theme"});
    audio.unload(catalog);
    assert(audio.loaded_clip_count() == 0 && audio.playing(again));
    assert(render(audio, 16)[0] != 0.0f);

    // The last owner of a clip lets go on the game thread.
    std::thread::id freed_on{};
    auto backend = std::make_shared<TrackedBackend>();
    backend->freed_on = &freed_on;
    audio.add_clip("tracked", kin::AudioClip{"tracked", kin::AudioFormat::F32, 1, 48000, 480, std::move(backend)});
    kin::AudioCatalog tracked = bus_catalog();
    tracked.add_cue({.id = "tracked", .clips = {"tracked"}, .bus = "sfx"});
    std::thread::id mixer_thread{};
    {
        assert(audio.play(tracked, {.cue = "tracked"}));
        audio.remove_clip("tracked");
        std::thread mixer{[&] {
            mixer_thread = std::this_thread::get_id();
            render(audio, 960); // the voice finishes on this "audio" thread
        }};
        mixer.join();
    }
    assert(freed_on == std::thread::id{});
    audio.update(0.0f);
    assert(freed_on == std::this_thread::get_id() && freed_on != mixer_thread);
}

// Distance curves: shape, power and pan strength, from the catalog.
void test_rolloff() {
    const auto gain = [](kin::AudioRolloff rolloff, float distance, float power = 1.0f) {
        return kin::calculate_spatial_audio({0.0f, 0.0f}, {distance, 0.0f},
                                            {.min_distance = 10.0f, .max_distance = 100.0f, .rolloff = rolloff,
                                             .rolloff_power = power}).gain;
    };
    using enum kin::AudioRolloff;
    for (const kin::AudioRolloff rolloff : {Smooth, Linear, Inverse}) {
        assert(gain(rolloff, 5.0f) == 1.0f && gain(rolloff, 10.0f) == 1.0f);
        assert(gain(rolloff, 100.0f) == 0.0f && gain(rolloff, 150.0f) == 0.0f);
    }
    assert(near(gain(Linear, 32.5f), 0.75f));
    assert(near(gain(Smooth, 32.5f), 1.0f - 0.15625f));
    assert(near(gain(Inverse, 20.0f), (0.5f - 0.1f) / 0.9f));
    assert(near(gain(Linear, 32.5f, 2.0f), 0.5625f));
    assert(kin::calculate_spatial_audio({0.0f, 0.0f}, {50.0f, 0.0f}, {.max_distance = 100.0f, .pan_strength = 0.0f}).pan == 0.0f);

    kin::AudioCatalog catalog = bus_catalog();
    catalog.add_cue({.id = "far", .clips = {"dc"}, .bus = "sfx", .loop = true, .spatial = true, .min_distance = 10.0f,
                     .max_distance = 100.0f, .rolloff = Linear, .rolloff_power = 2.0f, .pan_strength = 0.0f});
    kin::AudioEngine audio = null_engine();
    audio.add_clip("dc", dc_clip(0.5f));
    audio.play(catalog, {.cue = "far", .position = {32.5f, 0.0f}, .has_position = true});
    const std::vector<kin::f32> out = render(audio, 16);
    assert(near(out[0], 0.5f * 0.5625f) && near(out[1], out[0])); // no pan

    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "kin-audio-tests-rolloff";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    assert(kin::save_audio_catalog(catalog, dir / "audio.kinaudio"));
    const kin::AudioCatalog reloaded = kin::load_audio_catalog(dir / "audio.kinaudio");
    const kin::AudioCue* loaded = reloaded.cue("far");
    assert(loaded && loaded->rolloff == Linear && loaded->rolloff_power == 2.0f && loaded->pan_strength == 0.0f);
}

// Output devices, with SDL's dummy driver (one device).
void test_output_devices() {
    kin::AudioEngine silent = null_engine();
    assert(!silent.set_output_device("anything") && silent.output_device().empty());

    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    const std::vector<std::string> devices = kin::list_audio_output_devices();
    if (devices.empty()) {
        std::fprintf(stderr, "device selection test skipped: no devices\n");
        return;
    }
    kin::AudioEngine audio{kin::create_sdl_audio_backend(48000, 2, "no such device")};
    assert(audio.device_available() && audio.output_device().empty()); // unknown: the default

    assert(audio.set_output_device(devices.front()));
    assert(audio.output_device() == devices.front());
    assert(!audio.set_output_device("no such device"));
    assert(audio.output_device() == devices.front());

    // Still mixing after the switch.
    const kin::i64 before = audio.stats().mixed_frames;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (audio.stats().mixed_frames == before && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    assert(audio.stats().mixed_frames > before);

    std::ostringstream text;
    {
        kin::JsonWriter json{text, false};
        kin::write_audio_settings(json, audio);
    }
    const kin::JsonParseResult parsed = kin::parse_json(text.str());
    assert(parsed.ok() && parsed.value->string_at("device") == devices.front());

    assert(audio.set_output_device(""));
    assert(audio.output_device().empty());
    kin::apply_audio_settings(audio, *parsed.value);
    assert(audio.output_device() == devices.front());
}

// Edited clip and catalog files are picked up while the game runs; a broken
// catalog keeps the last good one.
void test_hot_reload() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "kin-audio-tests-reload";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    write_wav(dir / "beep.wav", 8000, 4800);
    const auto write_catalog = [&](float sfx_volume, bool with_cue) {
        std::ofstream out(dir / "audio.kinaudio");
        out << "bus master 1\nbus sfx " << sfx_volume << " master\nclip beep beep.wav\n";
        out << "cue hum sound sfx loop=true clips=beep\n";
        if (with_cue) {
            out << "cue blip sound sfx clips=beep\n";
        }
    };
    write_catalog(1.0f, false);
    kin::AudioCatalog catalog = kin::load_audio_catalog(dir / "audio.kinaudio");

    kin::FileWatcher files;
    kin::AudioEngine audio = null_engine();
    audio.watch(files);
    audio.watch_catalog(files, dir / "audio.kinaudio", catalog);
    const kin::AudioHandle old_voice = audio.play(catalog, {.cue = "hum"});
    const float before = render(audio, 16)[0];
    assert(near(before, 8000.0f / 32768.0f, 1e-3f));

    // A louder file, and a catalog that halves sfx and adds a cue.
    write_wav(dir / "beep.wav", 16000, 2400);
    write_catalog(0.5f, true);
    files.poll_now();
    files.poll_now(); // a change is reported once the file has held still for a poll
    assert(catalog.cue("blip") != nullptr);
    assert(near(catalog.bus("sfx")->volume, 0.5f));
    render(audio, 1024); // the bus volume ramps
    assert(near(render(audio, 16)[0], before * 0.5f, 1e-3f)); // the playing voice keeps the old samples
    audio.stop(old_voice);
    render(audio, 480);
    audio.play(catalog, {.cue = "blip"});
    assert(near(render(audio, 16)[0], 16000.0f / 32768.0f * 0.5f, 1e-3f)); // new plays get the new file

    // A broken edit is reported and ignored.
    std::ofstream(dir / "audio.kinaudio") << "bus master\n";
    files.poll_now();
    files.poll_now();
    assert(catalog.cue("blip") != nullptr);

    // Unloaded clips are no longer watched.
    audio.unload(catalog);
    assert(audio.loaded_clip_count() == 0);
}

// Emitters carry volume, pitch and pause, can remove or despawn themselves
// when done, and silence their sound when they go away.
void test_ecs_emitters() {
    kin::AudioEngine audio = null_engine();
    audio.add_clip("dc", dc_clip(0.5f));
    audio.add_clip("short", dc_clip(0.5f, 480));
    kin::AudioCatalog catalog = bus_catalog();
    catalog.add_cue({.id = "short", .clips = {"short"}, .bus = "sfx"});

    kin::EcsWorld world;
    kin::EcsEntity hum = world.entity("hum");
    hum.set(kin::Transform2D{});
    hum.set(kin::AudioEmitter{.cue = "hum", .volume = 0.5f});
    kin::update_audio_emitters(world, audio, catalog);
    assert(near(render(audio, 16)[0], 0.25f));

    hum.raw().get_mut<kin::AudioEmitter>()->paused = true;
    kin::update_audio_emitters(world, audio, catalog);
    render(audio, 1024);
    assert(render(audio, 16)[0] == 0.0f);
    assert(hum.raw().get<kin::AudioEmitter>()->playing);
    hum.raw().get_mut<kin::AudioEmitter>()->paused = false;
    hum.raw().get_mut<kin::AudioEmitter>()->volume = 1.0f;
    kin::update_audio_emitters(world, audio, catalog);
    render(audio, 1024);
    assert(near(render(audio, 16)[0], 0.5f));

    // Removing the emitter stops its sound.
    hum.raw().remove<kin::AudioEmitter>();
    kin::update_audio_emitters(world, audio, catalog);
    render(audio, 4800);
    assert(audio.active_voice_count() == 0);

    kin::EcsEntity removes = world.entity("removes");
    removes.set(kin::Transform2D{});
    removes.set(kin::AudioEmitter{.cue = "short", .when_done = kin::AudioEmitterEnd::Remove});
    kin::EcsEntity despawns = world.entity("despawns");
    despawns.set(kin::Transform2D{});
    despawns.set(kin::AudioEmitter{.cue = "short", .when_done = kin::AudioEmitterEnd::Despawn});
    kin::EcsEntity keeps = world.entity("keeps");
    keeps.set(kin::Transform2D{});
    keeps.set(kin::AudioEmitter{.cue = "short", .when_done = kin::AudioEmitterEnd::Keep});
    kin::update_audio_emitters(world, audio, catalog);
    assert(audio.active_voice_count() == 3);
    render(audio, 960);
    kin::update_audio_emitters(world, audio, catalog);
    kin::update_audio_emitters(world, audio, catalog);
    assert(!removes.raw().has<kin::AudioEmitter>());
    assert(!despawns.raw().is_alive());
    assert(keeps.raw().has<kin::AudioEmitter>() && !keeps.raw().get<kin::AudioEmitter>()->playing);
    assert(audio.active_voice_count() == 0); // nothing restarted

    // One-shots carry volume and pitch too.
    render(audio, 9600); // let the limiter recover from the three voices above
    kin::EcsEntity shot = world.entity("shot");
    shot.set(kin::Transform2D{});
    shot.set(kin::AudioOneShot{.cue = "short", .volume = 0.5f});
    kin::consume_audio_one_shots(world, audio, catalog);
    assert(near(render(audio, 16)[0], 0.25f));
}

} // namespace

int main() {
    test_memory_clips();
    test_catalog_roundtrip_and_loader();
    test_spatial_audio();
    test_voice_priority_caps_and_instances();
    test_music_has_dedicated_cap();
    test_ecs_audio_bridge_and_animation_events();
    test_bus_volume_follows_hierarchy();
    test_stop_fades();
    test_bus_pause();
    test_resampling_and_pitch();
    test_variation_is_seeded();
    test_limiter();
    test_equal_power_pan();
    test_mono_output();
    test_preload_async();
    test_device_thread_mixes();
    test_decodes_every_format();
    test_loop_points_and_streaming();
    test_stream_matches_decoded();
    test_catalog_clip_options();
    test_ducking();
    test_music();
    test_voice_controls();
    test_settings_and_report();
    test_lua_audio();
    test_catalog_ducks_roundtrip();
    test_filters();
    test_reverb_tail();
    test_compressor();
    test_effect_changes_and_catalog();
    test_seek();
    test_pause_one_voice();
    test_unloading();
    test_rolloff();
    test_output_devices();
    test_hot_reload();
    test_ecs_emitters();
    return 0;
}
