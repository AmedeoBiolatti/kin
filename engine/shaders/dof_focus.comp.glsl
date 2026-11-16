#version 450 core
// Depth of field's autofocus (DepthOfField3D::autofocus): the distance at the focus point (the mean of 64 points in a
// small square round it, the sky left out unless all are sky), eased from the last render's toward it. One work group.
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D uDepth;
layout(set = 0, binding = 1) uniform sampler2D uPrevious; // 1 x 1: x the focus the last render used
layout(set = 1, binding = 0, rgba16f) uniform writeonly image2D uFocus;
#include "kin_dof.inc.glsl"
// params.extra: xy the focus point (0 to 1), z the share of the way to go this render (1: all), w 1 to start afresh

shared vec2 found[64]; // the sum of distances and how many

void main() {
    const uint i = gl_LocalInvocationIndex;
    const vec2 offset = (vec2(gl_LocalInvocationID.xy) - 3.5) * 0.004; // a square 3 % of the view across
    const float z = dof_distance(uDepth, clamp(params.extra.xy + offset * vec2(params.size.y * params.size.z, 1.0),
                                               vec2(0.0), vec2(1.0)));
    found[i] = z < 60000.0 ? vec2(z, 1.0) : vec2(0.0);
    barrier();
    for (uint step = 32u; step > 0u; step >>= 1u) {
        if (i < step) {
            found[i] += found[i + step];
        }
        barrier();
    }
    if (i == 0u) {
        const float target = found[0].y > 0.0 ? found[0].x / found[0].y : 60000.0;
        const float before = params.extra.w > 0.5 ? params.lens.y : texelFetch(uPrevious, ivec2(0), 0).x;
        // Eased in the distance's reciprocal, which the blur goes by: from far to near as fast as back.
        const float eased = 1.0 / mix(1.0 / max(before, 1e-3), 1.0 / max(target, 1e-3), params.extra.z);
        imageStore(uFocus, ivec2(0), vec4(min(eased, 60000.0), target, 0.0, 0.0));
    }
}
