#version 450 core
// Depth of field (DepthOfField3D), first pass: the picture at half size, each texel the mean of its four pixels, each
// weighed down by its brightness (Karis: a single very bright pixel does not make a sparkling disc), and the blur
// radius (full pixels) and distance of its nearest pixel (so a sharp thing in front keeps its edge).
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D uColour; // the picture, linear, premultiplied
layout(set = 0, binding = 1) uniform sampler2D uDepth;
layout(set = 0, binding = 2) uniform sampler2D uFocus; // 1 x 1, with autofocus
layout(set = 1, binding = 0, rgba16f) uniform writeonly image2D uHalf;
layout(set = 1, binding = 1, rgba16f) uniform writeonly image2D uHalfBlur; // x: radius, y: distance
#include "kin_dof.inc.glsl"

void main() {
    const ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    const ivec2 half_size = (ivec2(params.size.xy) + 1) / 2;
    if (any(greaterThanEqual(p, half_size))) {
        return;
    }
    const float focus = params.lens.w > 0.5 ? texelFetch(uFocus, ivec2(0), 0).x : params.lens.y;
    vec4 sum = vec4(0.0);
    float weights = 0.0, nearest = 1e9, radius = 0.0;
    for (int i = 0; i < 4; ++i) {
        const ivec2 q = min(p * 2 + ivec2(i & 1, i >> 1), ivec2(params.size.xy) - 1);
        const vec4 c = texelFetch(uColour, q, 0);
        const float w = 1.0 / (1.0 + dot(c.rgb, vec3(0.2126, 0.7152, 0.0722)));
        sum += c * w;
        weights += w;
        const float z = dof_distance(uDepth, (vec2(q) + 0.5) * params.size.zw);
        if (z < nearest) {
            nearest = z;
            radius = dof_radius(z, focus);
        }
    }
    imageStore(uHalf, p, sum / weights);
    imageStore(uHalfBlur, p, vec4(radius, nearest, 0.0, 0.0));
}
