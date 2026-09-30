#include <kin/audio/audio.hpp>
#include <kin/anim/player.hpp>
#include <kin/ecs/audio.hpp>
#include <kin/platform/log.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
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
    config.queue_target_frames = 128;
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

int main() {
    test_memory_clips();
    test_catalog_roundtrip_and_loader();
    test_spatial_audio();
    test_voice_priority_caps_and_instances();
    test_music_has_dedicated_cap();
    test_ecs_audio_bridge_and_animation_events();
    return 0;
}
