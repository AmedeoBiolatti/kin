#pragma once

#include <kin/anim/value.hpp>
#include <kin/core/types.hpp>

#include <string>
#include <vector>

namespace kin {

enum class TrackSpace : u8 {
    Absolute,
    Relative,
};

struct Keyframe {
    f32 time = 0.0f;
    AnimValue value;
    Easing easing = Easing::Linear;
};

struct PropertyTrack {
    std::string property;
    std::string target;
    TrackSpace space = TrackSpace::Absolute;
    std::vector<Keyframe> keys;
};

struct SpriteBox {
    enum class Kind : u8 {
        Hitbox,
        Hurtbox,
    };

    Kind kind = Kind::Hurtbox;
    Rectf rect{};
    std::string name;
};

struct SpriteKey {
    f32 time = 0.0f;
    std::string sprite_id;
    Vec2f pivot{};
    bool has_pivot = false;
    std::vector<SpriteBox> boxes;
};

struct SpriteTrack {
    std::string target;
    std::vector<SpriteKey> keys;
};

struct EventTrack;

AnimValue sample(const PropertyTrack& track, f32 t);
i32 active_sprite_key(const SpriteTrack& track, f32 t);
f32 track_duration(const PropertyTrack& track);
f32 track_duration(const SpriteTrack& track);

} // namespace kin
