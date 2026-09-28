#pragma once

#include <kin/core/types.hpp>

namespace kin {

struct AudioListener2D {
    Vec2f position{};
    Vec2f forward{0.0f, -1.0f};
};

struct SpatialAudio {
    f32 min_distance = 32.0f;
    f32 max_distance = 512.0f;
    f32 pan_strength = 1.0f;
};

struct SpatialAudioResult {
    f32 gain = 1.0f;
    f32 pan = 0.0f;
};

SpatialAudioResult calculate_spatial_audio(Vec2f listener, Vec2f source, SpatialAudio spatial);

} // namespace kin
