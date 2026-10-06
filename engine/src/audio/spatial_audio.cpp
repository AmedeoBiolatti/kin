#include <kin/audio/spatial_audio.hpp>

#include <algorithm>
#include <cmath>

namespace kin {
namespace {

f32 smoothstep(f32 value) {
    const f32 t = std::clamp(value, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

} // namespace

SpatialAudioResult calculate_spatial_audio(Vec2f listener, Vec2f source, SpatialAudio spatial) {
    const f32 dx = source.x - listener.x;
    const f32 dy = source.y - listener.y;
    const f32 distance = std::sqrt(dx * dx + dy * dy);
    const f32 range = std::max(0.001f, spatial.max_distance - spatial.min_distance);
    const f32 t = std::clamp((distance - spatial.min_distance) / range, 0.0f, 1.0f);
    f32 gain = 1.0f;
    switch (spatial.rolloff) {
    case AudioRolloff::Smooth: gain = 1.0f - smoothstep(t); break;
    case AudioRolloff::Linear: gain = 1.0f - t; break;
    case AudioRolloff::Inverse: {
        // min/d, shifted and scaled so it still reaches 0 at max_distance.
        const f32 near = std::max(spatial.min_distance, 1.0f);
        const f32 far = std::max(spatial.max_distance, near + 0.001f);
        const f32 d = std::clamp(distance, near, far);
        gain = (near / d - near / far) / (1.0f - near / far);
        break;
    }
    }
    gain = std::clamp(gain, 0.0f, 1.0f);
    if (spatial.rolloff_power != 1.0f && gain > 0.0f) {
        gain = std::pow(gain, std::max(0.01f, spatial.rolloff_power));
    }
    const f32 pan = std::clamp(dx / std::max(1.0f, spatial.max_distance), -1.0f, 1.0f) * spatial.pan_strength;
    return {
        .gain = gain,
        .pan = std::clamp(pan, -1.0f, 1.0f),
    };
}

} // namespace kin
