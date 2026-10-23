// Links only kin::audio. ECS adapters are tested separately through kin::audio_ecs.
#include <kin/components.hpp>
#include <kin/audio/audio.hpp>
#include <cassert>

int main() {
    static_assert(kin::components::audio);
    kin::AudioEngine audio{kin::create_null_audio_backend(48000, 2)};
    kin::AudioCatalog catalog;
    catalog.add_bus({.id = "master"});
    auto clip = kin::make_memory_audio_clip("tone", std::vector<kin::f32>(960, 0.25f), 2, 48000);
    assert(clip.valid());
    audio.add_clip("tone", std::move(clip));
    catalog.add_cue({.id = "tone", .clips = {"tone"}, .bus = "master"});
    auto handle = audio.play(catalog, {.cue = "tone"});
    assert(handle.valid());
    audio.update(1.0f / 60.0f);
    assert(audio.stats().mixed_frames > 0);
}
