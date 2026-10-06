#include <kin/audio/audio.hpp>
#include <kin/anim/player.hpp>
#include <kin/core/jobs.hpp>
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
    return 0;
}
