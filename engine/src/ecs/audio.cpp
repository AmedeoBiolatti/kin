#include <kin/ecs/audio.hpp>

#include <algorithm>
#include <vector>

namespace kin {
namespace {

Vec2f entity_position(flecs::entity entity) {
    if (const auto* transform = entity.get<Transform2D>()) {
        return transform->pos;
    }
    return {};
}

// Internal flecs-typed implementations; the public EcsWorld& API forwards here.

void update_audio_listeners_raw(flecs::world& world, AudioEngine& audio) {
    bool found = false;
    i32 best_priority = 0;
    Vec2f best_position{};
    world.each([&](flecs::entity entity, AudioListener& listener, Transform2D& transform) {
        if (!found || listener.priority > best_priority) {
            found = true;
            best_priority = listener.priority;
            best_position = transform.pos;
        }
    });
    if (found) {
        audio.set_listener(best_position);
    }
}

// The voices emitters had last update, so a sound whose emitter went away
// (component removed, entity destroyed) can be stopped.
struct AudioEmitterVoices {
    std::vector<AudioHandle> handles;
};

// How long a sound fades out when its emitter goes away.
constexpr f32 emitter_gone_fade = 0.05f;

void update_audio_emitters_raw(flecs::world& world, AudioEngine& audio, const AudioCatalog& catalog) {
    std::vector<AudioHandle> live;
    std::vector<flecs::entity> remove;
    std::vector<flecs::entity> despawn;
    world.each([&](flecs::entity entity, AudioEmitter& emitter) {
        const Vec2f pos = entity_position(entity);
        if (emitter.handle && audio.playing(emitter.handle)) {
            audio.set_position(emitter.handle, pos);
            audio.set_volume(emitter.handle, emitter.volume);
            audio.set_pitch(emitter.handle, emitter.pitch);
            audio.set_paused(emitter.handle, emitter.paused);
            emitter.playing = true;
            live.push_back(emitter.handle);
            return;
        }

        const bool finished = emitter.handle.valid(); // it had a voice, and it ended
        emitter.playing = false;
        if (finished && emitter.when_done != AudioEmitterEnd::Restart) {
            if (emitter.when_done == AudioEmitterEnd::Remove) {
                remove.push_back(entity);
            } else if (emitter.when_done == AudioEmitterEnd::Despawn) {
                despawn.push_back(entity);
            }
            return; // Keep: stays silent until the game clears `handle` or sets a new cue
        }
        if (!emitter.auto_start || emitter.cue.empty()) {
            return;
        }

        emitter.handle = audio.play(catalog, {
            .cue = emitter.cue,
            .position = pos,
            .has_position = true,
            .volume = emitter.volume,
            .pitch = emitter.pitch,
            .fade_in = emitter.fade_in,
        });
        if (emitter.handle && emitter.paused) {
            audio.set_paused(emitter.handle, true);
        }
        emitter.playing = emitter.handle.valid();
        if (emitter.handle) {
            live.push_back(emitter.handle);
        }
    });

    AudioEmitterVoices* known = world.get_mut<AudioEmitterVoices>();
    if (!known) {
        world.set<AudioEmitterVoices>({});
        known = world.get_mut<AudioEmitterVoices>();
    }
    for (const AudioHandle handle : known->handles) {
        if (std::ranges::find(live, handle) == live.end()) {
            audio.stop(handle, emitter_gone_fade); // its emitter is gone
        }
    }
    known->handles = std::move(live);

    for (flecs::entity entity : remove) {
        entity.remove<AudioEmitter>();
    }
    for (flecs::entity entity : despawn) {
        entity.destruct();
    }
}

void consume_audio_one_shots_raw(flecs::world& world, AudioEngine& audio, const AudioCatalog& catalog) {
    std::vector<flecs::entity> consumed;
    world.each([&](flecs::entity entity, AudioOneShot& one_shot) {
        const Vec2f base = entity_position(entity);
        audio.play(catalog, {
            .cue = one_shot.cue,
            .position = {base.x + one_shot.offset.x, base.y + one_shot.offset.y},
            .has_position = true,
            .volume = one_shot.volume,
            .pitch = one_shot.pitch,
            .priority_boost = one_shot.priority_boost,
        });
        consumed.push_back(entity);
    });
    for (flecs::entity entity : consumed) {
        entity.remove<AudioOneShot>();
    }
}

} // namespace

void update_audio_listeners(EcsWorld& world, AudioEngine& audio) {
    update_audio_listeners_raw(world.raw(), audio);
}

void update_audio_emitters(EcsWorld& world, AudioEngine& audio, const AudioCatalog& catalog) {
    update_audio_emitters_raw(world.raw(), audio, catalog);
}

void consume_audio_one_shots(EcsWorld& world, AudioEngine& audio, const AudioCatalog& catalog) {
    consume_audio_one_shots_raw(world.raw(), audio, catalog);
}

EventInterpreter make_audio_event_interpreter(AudioEngine& audio, const AudioCatalog& catalog) {
    return [&audio, &catalog](EcsEntity entity, const AnimationEvent& event) {
        if (event.value.empty()) {
            return;
        }
        const Vec2f base = entity_position(entity.raw());
        audio.play(catalog, {
            .cue = event.value,
            .position = {base.x + event.offset.x, base.y + event.offset.y},
            .has_position = true,
        });
    };
}

void play_animation_audio_events(EcsWorld& world, AudioEngine& audio, const AudioCatalog& catalog) {
    AnimationEventDispatch dispatch;
    dispatch.on("sound", make_audio_event_interpreter(audio, catalog));
    dispatch.run(world);
}

} // namespace kin
