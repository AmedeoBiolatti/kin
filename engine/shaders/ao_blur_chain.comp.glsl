#version 450 core
// The ambient occlusion's blur (after gtao.comp), one direction a dispatch:
// 7 taps, weighted by their distance and by how near their depth is to the
// pixel's, so the shadow of a crease does not bleed over an edge onto what is
// in front of it or behind it.
//
// This is the chain version (selected automatically for large views): the depth comes from the linear depth chain's first level
// (ao_depth_chain.comp), so a tap needs no divide to find its distance, and the centre's reciprocal is worked out once.
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D uChain; // the distance in front of the camera, 0 is nothing (level 0 only)
layout(set = 0, binding = 1) uniform sampler2D uSource; // R8
layout(set = 1, binding = 0, r8) uniform writeonly image2D uResult;
layout(set = 2, binding = 0) uniform Params {
    vec4 sizes;  // xy: the result's size, zw: the depth's
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
        return;
    }
    const float edge_scale = 1.0 / (EDGE * max(centre_depth, 1e-3));
    float sum = 0.0, total = 0.0;
    for (int i = -TAPS; i <= TAPS; ++i) {
        const ivec2 q = clamp(xy + direction * i, ivec2(0), size - 1);
        const float d = texelFetch(uChain, min(ivec2(vec2(q) * scale), depth_size - 1), 0).r;
        // The sky has no depth to compare, and no occlusion of its own.
        const float w = WEIGHTS[abs(i)] * (d > 0.0 ? exp(-abs(d - centre_depth) * edge_scale) : 0.0);
        sum += texelFetch(uSource, q, 0).r * w;
        total += w;
    }
    imageStore(uResult, xy, vec4(total > 0.0 ? sum / total : 1.0));
}
