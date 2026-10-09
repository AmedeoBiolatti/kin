#version 450 core
// ao_blur.comp for both screen-space shadows at once (ambient occlusion and contact shadows, when they have the same
// size): the depth-aware weights of a tap are worked out once and applied to both sources, so the depth is read, and
// the weights computed, half as often as with two dispatches. This is the chain version (KIN_AO_CLASSIC=1 uses ao_blur_both.comp): the
// depth comes from the linear depth chain's first level (ao_depth_chain.comp), so no tap needs a divide. Each result is what ao_blur.comp would have written.
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D uChain; // the distance in front of the camera, 0 is nothing (level 0 only)
layout(set = 0, binding = 1) uniform sampler2D uSource;        // R8: the occlusion
layout(set = 0, binding = 2) uniform sampler2D uSourceContact; // R8: the contact shadows
layout(set = 1, binding = 0, r8) uniform writeonly image2D uResult;
layout(set = 1, binding = 1, r8) uniform writeonly image2D uResultContact;
layout(set = 2, binding = 0) uniform Params {
    vec4 sizes;  // xy: the results' size, zw: the depth's
    vec4 step;   // xy: the direction (in result pixels)
} params;

const int TAPS = 3;
// The pixel's depth differs by this share of its distance and a tap counts for e^-1.
const float EDGE = 0.03;
const float WEIGHTS[TAPS + 1] = float[](1.0, 0.8, 0.5, 0.25);

void main() {
    const ivec2 xy = ivec2(gl_GlobalInvocationID.xy);
    const ivec2 size = ivec2(params.sizes.xy);
    if (xy.x >= size.x || xy.y >= size.y) {
        return;
    }
    const ivec2 depth_size = ivec2(params.sizes.zw);
    const vec2 scale = params.sizes.zw / params.sizes.xy;
    const ivec2 direction = ivec2(params.step.xy);
    const float centre_depth = texelFetch(uChain, min(ivec2(vec2(xy) * scale), depth_size - 1), 0).r;
    if (centre_depth <= 0.0) {
        imageStore(uResult, xy, vec4(1.0));
        imageStore(uResultContact, xy, vec4(1.0));
        return;
    }
    const float edge_scale = 1.0 / (EDGE * max(centre_depth, 1e-3));
    float sum = 0.0, sum_contact = 0.0, total = 0.0;
    for (int i = -TAPS; i <= TAPS; ++i) {
        const ivec2 q = clamp(xy + direction * i, ivec2(0), size - 1);
        const float d = texelFetch(uChain, min(ivec2(vec2(q) * scale), depth_size - 1), 0).r;
        // The sky has no depth to compare, and no occlusion of its own.
        const float w = WEIGHTS[abs(i)] * (d > 0.0 ? exp(-abs(d - centre_depth) * edge_scale) : 0.0);
        sum += texelFetch(uSource, q, 0).r * w;
        sum_contact += texelFetch(uSourceContact, q, 0).r * w;
        total += w;
    }
    imageStore(uResult, xy, vec4(total > 0.0 ? sum / total : 1.0));
    imageStore(uResultContact, xy, vec4(total > 0.0 ? sum_contact / total : 1.0));
}
