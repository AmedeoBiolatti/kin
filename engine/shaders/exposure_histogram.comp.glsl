#version 450 core
// Auto exposure (ViewOptions3D::auto_exposure), its first half: how bright the view's HDR picture is, as a histogram of
// the log2 of its luminance. A thread reads one 2 x 2 block (one bilinear sample at its centre: their mean), so a
// workgroup of 16 x 16 covers 32 x 32 pixels. Uncovered pixels (alpha 0: the transparent background) are left out.
// The workgroup counts into shared memory and adds its counts to the view's buffer once; exposure_adapt.comp reads
// the buffer and clears it again for the next frame.
layout(local_size_x = 16, local_size_y = 16) in;

const uint BINS = 128;

layout(set = 0, binding = 0) uniform sampler2D uScene;
// The view's exposure state (GpuView3D::_exposure): the histogram, then the adapted exposure.
layout(std430, set = 1, binding = 0) buffer Exposure {
    uint bins[BINS];
    float ev;       // the adapted exposure, in stops
    float exposure; // exp2(ev)
    float target;   // what the last frame's histogram asked for, in stops
    float pad;
} state;
layout(std140, set = 2, binding = 0) uniform Params {
    vec4 a; // xy: 1 / the picture's size, z: the histogram's lowest log2 luminance, w: bins per stop
    vec4 b; // x: centre weight (0: every pixel alike; 1: the middle counts most), yz: the block grid's size
} P;

shared uint local_bins[BINS];

void main() {
    const uint index = gl_LocalInvocationIndex;
    if (index < BINS) {
        local_bins[index] = 0u;
    }
    barrier();
    const uvec2 block = gl_GlobalInvocationID.xy;
    if (float(block.x) < P.b.y && float(block.y) < P.b.z) {
        // The centre of the 2 x 2 block: one linear sample averages its four pixels (clamped at the edges).
        const vec2 uv = (vec2(block) * 2.0 + 1.0) * P.a.xy;
        const vec4 c = textureLod(uScene, uv, 0.0);
        if (c.a > 0.0) {
            // Premultiplied: the colour of the surface, out of its coverage.
            const float luminance = dot(c.rgb / c.a, vec3(0.2126, 0.7152, 0.0722));
            // Black (and anything below the range) goes in bin 0, which the average leaves out.
            uint bin = 0u;
            if (luminance > 0.0) {
                const float at = (log2(luminance) - P.a.z) * P.a.w;
                bin = uint(clamp(at, 1.0, float(BINS - 1u)));
            }
            uint weight = 4u;
            if (P.b.x > 0.0) {
                // Centre-weighted metering: a falloff from the middle (1) to the corners (about 0.1).
                const vec2 d = uv * 2.0 - 1.0;
                const float centre = exp(-2.3 * dot(d, d));
                weight = uint(round(4.0 * mix(1.0, 4.0 * centre, P.b.x))) + 1u;
            }
            atomicAdd(local_bins[bin], weight);
        }
    }
    barrier();
    if (index < BINS && local_bins[index] != 0u) {
        atomicAdd(state.bins[index], local_bins[index]);
    }
}
