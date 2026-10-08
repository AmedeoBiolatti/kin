#version 450 core
// The ambient occlusion's blur (after gtao.comp), one direction a dispatch:
// 7 taps, weighted by their distance and by how near their depth is to the
// pixel's, so the shadow of a crease does not bleed over an edge onto what is
// in front of it or behind it.
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D uDepth;
layout(set = 0, binding = 1) uniform sampler2D uSource; // R8
layout(set = 1, binding = 0, r8) uniform writeonly image2D uResult;
layout(set = 2, binding = 0) uniform Params {
    vec4 sizes;  // xy: the result's size, zw: the depth's
    vec4 step;   // xy: the direction (in result pixels)
    vec4 depth;  // a, b, c, d: a view-space z of a depth is (a * depth + b) / (c * depth + d)
} params;

const int TAPS = 3;
// The pixel's depth differs by this share of its distance and a tap counts for e^-1.
const float EDGE = 0.03;
const float WEIGHTS[TAPS + 1] = float[](1.0, 0.8, 0.5, 0.25);

float view_z(float d) {
    return (params.depth.x * d + params.depth.y) / (params.depth.z * d + params.depth.w);
}

void main() {
    const ivec2 xy = ivec2(gl_GlobalInvocationID.xy);
    const ivec2 size = ivec2(params.sizes.xy);
    if (xy.x >= size.x || xy.y >= size.y) {
        return;
    }
    const ivec2 depth_size = ivec2(params.sizes.zw);
    const vec2 scale = params.sizes.zw / params.sizes.xy;
    const ivec2 direction = ivec2(params.step.xy);
    const float centre_depth = texelFetch(uDepth, min(ivec2(vec2(xy) * scale), depth_size - 1), 0).r;
    if (centre_depth <= 0.0) {
        imageStore(uResult, xy, vec4(1.0));
        return;
    }
    const float centre = view_z(centre_depth);
    float sum = 0.0, total = 0.0;
    for (int i = -TAPS; i <= TAPS; ++i) {
        const ivec2 q = clamp(xy + direction * i, ivec2(0), size - 1);
        const float d = texelFetch(uDepth, min(ivec2(vec2(q) * scale), depth_size - 1), 0).r;
        // The sky has no depth to compare, and no occlusion of its own.
        const float w = WEIGHTS[abs(i)] * (d > 0.0 ? exp(-abs(view_z(d) - centre) / (EDGE * max(abs(centre), 1e-3))) : 0.0);
        sum += texelFetch(uSource, q, 0).r * w;
        total += w;
    }
    imageStore(uResult, xy, vec4(total > 0.0 ? sum / total : 1.0));
}
