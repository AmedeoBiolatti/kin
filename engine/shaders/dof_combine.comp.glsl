#version 450 core
// Depth of field (DepthOfField3D), last pass: each full pixel between itself and the half-size blur, by how wide the
// blur reaching it is (its own radius, or a blurred thing's in front of it): sharp under about a pixel, the blur past
// one and a half. The half-size texels are read as their distance agrees with the pixel's (a blurred background does
// not bleed over a sharp edge in front of it, nor the other way).
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D uColour;  // the full picture
layout(set = 0, binding = 1) uniform sampler2D uDepth;
layout(set = 0, binding = 2) uniform sampler2D uFocus;   // 1 x 1, with autofocus
layout(set = 0, binding = 3) uniform sampler2D uBlurred; // dof_gather's
layout(set = 0, binding = 4) uniform sampler2D uReach;
layout(set = 1, binding = 0, rgba16f) uniform writeonly image2D uResult;
#include "kin_dof.inc.glsl"

void main() {
    const ivec2 q = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(q, ivec2(params.size.xy)))) {
        return;
    }
    const vec4 sharp = texelFetch(uColour, q, 0);
    const float focus = params.lens.w > 0.5 ? texelFetch(uFocus, ivec2(0), 0).x : params.lens.y;
    const float z = dof_distance(uDepth, (vec2(q) + 0.5) * params.size.zw);
    const float own = abs(dof_radius(z, focus));
    // The four half texels round the pixel, bilinear weights times how well their distance agrees.
    const ivec2 extent = textureSize(uBlurred, 0);
    const vec2 at = (vec2(q) + 0.5) * 0.5 - 0.5;
    const ivec2 base = ivec2(floor(at));
    const vec2 f = at - vec2(base);
    vec4 blurred = vec4(0.0);
    float reach = 0.0, weights = 0.0;
    for (int i = 0; i < 4; ++i) {
        const ivec2 o = ivec2(i & 1, i >> 1);
        const ivec2 t = clamp(base + o, ivec2(0), extent - 1);
        const vec2 r = texelFetch(uReach, t, 0).xy;
        const float bilinear = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y);
        const float w = bilinear / (0.02 + abs(r.y - z) / max(min(r.y, z), 1e-3)) + 1e-6;
        blurred += texelFetch(uBlurred, t, 0) * w;
        reach += r.x * w;
        weights += w;
    }
    blurred /= weights;
    reach /= weights;
    const float amount = smoothstep(0.75, 1.5, max(own, reach));
    imageStore(uResult, q, mix(sharp, blurred, amount));
}
