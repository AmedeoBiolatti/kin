#include <kin/ecs/audio.hpp>
#include <kin/platform/log.hpp>
#if defined(KIN_EXPERIMENTAL_3D)
#include <kin/ecs/scene3d.hpp>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

namespace kin {
namespace {

Vec2f entity_position(flecs::entity entity) {
    if (const auto* transform = entity.get<Transform2D>()) {
        return transform->pos;
    }
    return {};
}

// Where an entity sounds: in 3D at its WorldTransform3D, at the point `offset_3d` of its own space, when it has one;
// else in 2D at its Transform2D plus `offset` (the origin without either). Not `valid` where the 3D point is not finite.
struct SoundPlace {
    bool is_3d = false;
    bool valid = true;
    Vec2f position{};
    Vec3f position_3d{};
};

#if defined(KIN_EXPERIMENTAL_3D)
bool finite(Vec3f v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// Once: a sound or listener whose WorldTransform3D cannot place it is left where it was (a one-shot is dropped).
void warn_unplaced() {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true)) {
        KIN_LOG_WARN("audio", "ECS audio: a non-finite or flattened WorldTransform3D cannot place a sound or the listener; left as it was");
    }
}
#endif

SoundPlace sound_place(flecs::entity entity, Vec2f offset, Vec3f offset_3d) {
#if defined(KIN_EXPERIMENTAL_3D)
    if (const auto* world = entity.get<WorldTransform3D>()) {
        const Vec3f at = world->matrix.apply(offset_3d);
        if (!finite(at)) {
            warn_unplaced();
            return {.is_3d = true, .valid = false};
        }
        return {.is_3d = true, .position_3d = at};
    }
#else
    (void)offset_3d;
#endif
    const Vec2f base = entity_position(entity);
    return {.position = {base.x + offset.x, base.y + offset.y}};
}

void place_voice(AudioEngine& audio, AudioHandle handle, const SoundPlace& place) {
    if (!place.valid) {
        return;
    }
    if (place.is_3d) {
        audio.set_position_3d(handle, place.position_3d);
    } else {
        audio.set_position(handle, place.position);
    }
}

#if defined(KIN_EXPERIMENTAL_3D)
// Ears at a world transform: there, facing its -Z with its +Y up, as a camera looks (its scale taken out). None for a
// transform that is not finite or is flattened along those axes.
std::optional<AudioListener3D> listener_at(const Mat4f& matrix) {
    const Vec3f position{matrix.m[12], matrix.m[13], matrix.m[14]};
    const Vec3f forward = matrix.apply_vector({0.0f, 0.0f, -1.0f});
    const Vec3f up = matrix.apply_vector({0.0f, 1.0f, 0.0f});
    if (!finite(position) || !finite(forward) || !finite(up) || length_squared(forward) < 1e-12f ||
        length_squared(cross(forward, up)) < 1e-12f) {
        warn_unplaced();
        return std::nullopt;
    }
    return AudioListener3D{.position = position, .forward = normalize(forward), .up = normalize(up)};
}
#endif

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
#if defined(KIN_EXPERIMENTAL_3D)
    // 3D over 2D: an entity with both transforms hears in 3D, and a 3D listener wins a tie with a 2D one.
    bool found_3d = false;
    i32 best_priority_3d = 0;
    Mat4f best_matrix{};
    world.each([&](flecs::entity, AudioListener& listener, const WorldTransform3D& transform) {
        if (!found_3d || listener.priority > best_priority_3d) {
            found_3d = true;
            best_priority_3d = listener.priority;
            best_matrix = transform.matrix;
        }
    });
    if (!found && !found_3d) {
        // No listener entity: the camera a SceneSession3D views the world through, chosen as apply_scene3d_settings
        // chooses it (the highest priority enabled SceneCamera3D, a tie to the lowest entity id).
        flecs::entity camera;
        i32 priority = std::numeric_limits<i32>::min();
        world.each([&](flecs::entity entity, const SceneCamera3D& settings, const WorldTransform3D& transform) {
            if (settings.enabled && (!camera || settings.priority > priority || (settings.priority == priority && entity.id() < camera.id()))) {
                camera = entity;
                priority = settings.priority;
                best_matrix = transform.matrix;
            }
        });
        found_3d = camera.is_valid();
    }
    if (found_3d && (!found || best_priority_3d >= best_priority)) {
        if (const auto listener = listener_at(best_matrix)) {
            audio.set_listener_3d(*listener);
        }
        return;
    }
#endif
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
        const SoundPlace place = sound_place(entity, {}, emitter.offset_3d);
        if (emitter.handle && audio.playing(emitter.handle)) {
            place_voice(audio, emitter.handle, place);
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
        // Its own catalog when bound, and a sound file's only cue when it names none.
        const AudioCatalog& source = emitter.catalog ? *emitter.catalog : catalog;
        std::string_view cue = emitter.cue;
        if (cue.empty() && emitter.catalog && emitter.catalog->cues().size() == 1) {
            cue = emitter.catalog->cues().begin()->first;
        }
        if (!emitter.auto_start || cue.empty() || !place.valid) {
            return; // nowhere to sound from yet: it starts once its transform places it
        }

        emitter.handle = audio.play(source, {
            .cue = std::string(cue),
            .position = place.position,
            .has_position = !place.is_3d,
            .position_3d = place.position_3d,
            .has_position_3d = place.is_3d,
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
        const SoundPlace place = sound_place(entity, one_shot.offset, one_shot.offset_3d);
        if (place.valid) { // one with nowhere to sound from is dropped, not played at the listener
            audio.play(catalog, {
                .cue = one_shot.cue,
                .position = place.position,
                .has_position = !place.is_3d,
                .position_3d = place.position_3d,
                .has_position_3d = place.is_3d,
                .volume = one_shot.volume,
                .pitch = one_shot.pitch,
                .priority_boost = one_shot.priority_boost,
            });
        }
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

void update_audio_emitters(EcsWorld& world, AudioEngine& audio) {
    static const AudioCatalog none;
    update_audio_emitters_raw(world.raw(), audio, none);
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
