#include <kin/anim/track.hpp>

#include <algorithm>

namespace kin {

AnimValue sample(const PropertyTrack& track, f32 t) {
    if (track.keys.empty()) {
        return {};
    }
    if (t <= track.keys.front().time) {
        return track.keys.front().value;
    }
    if (t >= track.keys.back().time) {
        return track.keys.back().value;
    }

    const auto next = std::upper_bound(track.keys.begin(), track.keys.end(), t, [](f32 time, const Keyframe& key) {
        return time < key.time;
    });
    const auto prev = next - 1;
    const f32 span = next->time - prev->time;
    const f32 local = span > 0.0f ? (t - prev->time) / span : 0.0f;
    return interpolate(prev->value, next->value, local, next->easing);
}

i32 active_sprite_key(const SpriteTrack& track, f32 t) {
    if (track.keys.empty() || t < track.keys.front().time) {
        return -1;
    }

    const auto next = std::upper_bound(track.keys.begin(), track.keys.end(), t, [](f32 time, const SpriteKey& key) {
        return time < key.time;
    });
    return static_cast<i32>((next - track.keys.begin()) - 1);
}

f32 track_duration(const PropertyTrack& track) {
    return track.keys.empty() ? 0.0f : track.keys.back().time;
}

f32 track_duration(const SpriteTrack& track) {
    return track.keys.empty() ? 0.0f : track.keys.back().time;
}

} // namespace kin
