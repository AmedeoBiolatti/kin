#include <kin/anim/events.hpp>
#include <kin/anim/player.hpp>
#include <kin/audio/audio.hpp>
#include <kin/ecs/audio.hpp>
#include <kin/ecs/render.hpp>
#include <kin/particles/animation_particles.hpp>
#include <kin/particles/particle_catalog.hpp>
#include <kin/particles/particle_system.hpp>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <utility>

namespace {

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

void write_wav(const std::filesystem::path& path) {
    std::ofstream out(path, std::ios::binary);
    const kin::u16 channels = 1;
    const kin::u32 sample_rate = 48000;
    const kin::u16 bits = 16;
    const kin::u32 frames = 32;
    const kin::u32 data_bytes = frames * channels * (bits / 8);

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
    for (kin::u32 i = 0; i < frames; ++i) {
        write_u16(out, 4000);
    }
}

void test_sound_interpreter_plays_cue() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "kin-anim-event-consumer-audio";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    write_wav(dir / "beep.wav");

    kin::AudioCatalog catalog;
    catalog.set_root(dir);
    catalog.add_clip({.id = "beep", .path = "beep.wav"});
    catalog.add_cue({.id = "step", .clips = {"beep"}, .category = kin::AudioCategory::Sound});
    kin::AudioEngine audio{kin::create_null_audio_backend()};

    kin::EcsWorld world;
    world.component<kin::AnimationEventQueue>("AnimationEventQueue");
    world.component<kin::Transform2D>("Transform2D");
    world.entity("actor")
        .set(kin::Transform2D{.pos = {4.0f, 5.0f}})
        .set(kin::AnimationEventQueue{.pending = {{
            .channel = "sound",
            .value = "step",
            .offset = {1.0f, 0.0f},
        }}});

    kin::AnimationEventDispatch dispatch;
    dispatch.on("sound", kin::make_audio_event_interpreter(audio, catalog));
    dispatch.run(world);

    assert(audio.active_voice_count() == 1);
}

void test_particle_interpreter_uses_target_position_and_offset() {
    kin::ParticleCatalog catalog;
    catalog.set_effect("spark", {.burst = {
        .count = 1,
        .speed = {0.0f, 0.0f},
        .lifetime = {1.0f, 1.0f},
    }});
    kin::ParticleSystem particles{kin::make_key(9)};
    particles.set_catalog(std::move(catalog));

    kin::EcsWorld world;
    world.component<kin::AnimationEventQueue>("AnimationEventQueue");
    world.component<kin::AnimationPlayer>("AnimationPlayer");
    world.component<kin::Transform2D>("Transform2D");

    kin::EcsEntity root = world.entity("root")
                              .set(kin::AnimationPlayer{})
                              .set(kin::AnimationEventQueue{.pending = {{
                                  .channel = "particle",
                                  .value = "spark",
                                  .target = "hand",
                                  .offset = {2.0f, -1.0f},
                              }}});
    world.entity("hand")
        .set(kin::Transform2D{.pos = {10.0f, 20.0f}})
        .child_of(root);

    kin::AnimationEventDispatch dispatch;
    dispatch.on("particle", kin::make_particle_event_interpreter(particles));
    dispatch.run(world);

    assert(particles.active_count() == 1);
    assert((particles.particles()[0].position == kin::Vec2f{12.0f, 19.0f}));
}

} // namespace

void run_event_consumer_tests() {
    test_sound_interpreter_plays_cue();
    test_particle_interpreter_uses_target_position_and_offset();
}
