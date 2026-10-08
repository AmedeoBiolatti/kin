#include <kin/render3d/taa.hpp>

#include <algorithm>

namespace kin {

namespace {

// The `index`th element of the Halton sequence in `base`: the digits of the
// index mirrored about the point, a van der Corput sequence.
f32 halton(u32 index, u32 base) {
    f32 result = 0.0f, fraction = 1.0f / static_cast<f32>(base);
    while (index > 0) {
        result += fraction * static_cast<f32>(index % base);
        index /= base;
        fraction /= static_cast<f32>(base);
    }
    return result;
}

} // namespace

Vec2f taa_jitter(u32 frame, u32 period) {
    period = std::clamp(period, 1u, max_taa_jitter_samples);
    const u32 index = frame % period + 1;
    return {halton(index, 2) - 0.5f, halton(index, 3) - 0.5f};
}

Mat4f jitter_projection(const Mat4f& projection, Vec2f pixels, Vec2i size) {
    // Clip space's +y is up, the picture's rows run down: the sign flips there.
    Mat4f shift;
    shift.at(0, 3) = 2.0f * pixels.x / static_cast<f32>(std::max(size.x, 1));
    shift.at(1, 3) = -2.0f * pixels.y / static_cast<f32>(std::max(size.y, 1));
    return shift * projection;
}

} // namespace kin
