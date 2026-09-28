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
    const f32 gain = 1.0f - smoothstep(t);
    const f32 pan = std::clamp(dx / std::max(1.0f, spatial.max_distance), -1.0f, 1.0f) * spatial.pan_strength;
    return {
        .gain = gain,
        .pan = std::clamp(pan, -1.0f, 1.0f),
    };
}

} // namespace kin
