#pragma once

#include <kin/core/math3d.hpp>
#include <kin/core/types.hpp>

namespace kin {

struct AudioListener2D {
    Vec2f position{};
    Vec2f forward{0.0f, -1.0f};
};

// A listener in 3D (a first-person camera): where it is, which way it faces
// and which way is up for it (+Y up, as kin's 3D has it).
struct AudioListener3D {
    Vec3f position{};
    Vec3f forward{0.0f, 0.0f, -1.0f};
    Vec3f up{0.0f, 1.0f, 0.0f};
};

// How a positioned sound fades between min_distance (full volume) and
// max_distance (silent).
enum class AudioRolloff {
    Smooth, // an S-curve: holds near the source, eases out at the edge
    Linear,
    Inverse, // like real sound: falls fast near the source, slowly far away
};

struct SpatialAudio {
    f32 min_distance = 32.0f;
    f32 max_distance = 512.0f;
    // How far a sound at max_distance to the side pans: 0 keeps it centred.
    f32 pan_strength = 1.0f;
    AudioRolloff rolloff = AudioRolloff::Smooth;
    // Raises the curve's gain to this power: above 1 it falls sooner, below 1 later.
    f32 rolloff_power = 1.0f;
};

struct SpatialAudioResult {
    f32 gain = 1.0f;
    f32 pan = 0.0f;
};

SpatialAudioResult calculate_spatial_audio(Vec2f listener, Vec2f source, SpatialAudio spatial);
// In 3D: the gain from the distance, the pan from the direction (to the
// listener's right is right), less pan close up (nothing jumps across the
// head), and up to a fifth quieter behind.
SpatialAudioResult calculate_spatial_audio_3d(const AudioListener3D& listener, Vec3f source, SpatialAudio spatial);

} // namespace kin
