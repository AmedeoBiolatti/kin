#version 450 core
// kin's global illumination (docs/3d_plan.md, G5c): the probes' light on what the camera sees, read once a texel at
// half the view's size from the pre-pass's depth and normals, for the main pass (KIN_GI in kin_material.inc.glsl),
// which would otherwise read the probes for every fragment it shades, the overdraw's too. Each texel stands for the
// nearest surface of its 2 x 2 pixels (a thin thing in front keeps its own light) and holds the light facing that
// surface's normal, and how its brightness changes with the way faced (a linear fit from three more ways, 60 degrees
// round the normal), so that the main pass can turn it to its normal map's normal and its reflection: the light
// facing way d is rgb * (k + g . d).
layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0) uniform sampler2D uDepth;   // reverse Z, 0 is nothing
layout(set = 0, binding = 1) uniform sampler2D uNormals; // world normals, octahedral
layout(set = 0, binding = 2) uniform sampler3D uGiIrradiance;
layout(set = 0, binding = 3) uniform sampler3D uGiMoments;
layout(set = 0, binding = 4) uniform sampler3D uGiProbes;
layout(set = 1, binding = 0, rgba16f) uniform writeonly image2D uLight; // rgb: the light facing the normal; a: the
                                                                         // depth along the view (less: no probe has
                                                                         // light there; 0: nothing drawn)
layout(set = 1, binding = 1, rgba16f) uniform writeonly image2D uSlope; // xyz: g, the brightness's change with the
                                                                         // way faced, over the brightness (world); w: k

#define KIN_GI_PROBES
#include "kin_gi.inc.glsl"

layout(std140, set = 2, binding = 0) uniform Params {
    mat4 to_world;     // the camera's space to the world's
    vec4 unproject_xy; // the inverse projection, as gtao.comp's
    vec4 unproject_zw;
    vec4 sizes;        // xy: the result's size, zw: the depth's
    vec4 eye;          // xyz: the camera (world); w: 1 orthographic
    vec4 forward;      // xyz: the way it looks (world)
    KinGiProbes probes;
} params;

#include "kin_depth.inc.glsl"

const vec3 luma = vec3(0.2126, 0.7152, 0.0722);

void main() {
    const ivec2 xy = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(xy, ivec2(params.sizes.xy)))) {
        return;
    }
    const ivec2 last = ivec2(params.sizes.zw) - 1;
    ivec2 at = min(xy * 2, last);
    float depth = texelFetch(uDepth, at, 0).r;
    for (int i = 1; i < 4; ++i) {
        const ivec2 q = min(xy * 2 + ivec2(i & 1, i >> 1), last);
        const float d = texelFetch(uDepth, q, 0).r;
        if (d > depth) {
            at = q;
            depth = d;
        }
    }
    if (depth <= 0.0) {
        imageStore(uLight, xy, vec4(0.0));
        imageStore(uSlope, xy, vec4(0.0, 0.0, 0.0, 1.0));
        return;
    }
    const vec3 seen = view_position(at, depth);
    const vec3 p = (params.to_world * vec4(seen, 1.0)).xyz;
    const vec3 n = decode_normal(texelFetch(uNormals, at, 0).rg);
    const vec3 to_eye = params.eye.w > 0.5 ? -params.forward.xyz : normalize(params.eye.xyz - p);
    // The normal and three ways 60 degrees from it, 120 degrees apart round it.
    const vec3 t = normalize(abs(n.y) < 0.9 ? cross(n, vec3(0.0, 1.0, 0.0)) : cross(n, vec3(1.0, 0.0, 0.0)));
    const vec3 b = cross(n, t);
    const float c = 0.5, s = 0.8660254;
    const vec3 ways[4] = vec3[4](n, c * n + s * t, c * n + s * (-0.5 * t + 0.8660254 * b),
                                 c * n + s * (-0.5 * t - 0.8660254 * b));
    vec3 light[4];
    if (!kin_gi_light(params.probes, p, n, to_eye, ways, 4, light)) {
        imageStore(uLight, xy, vec4(0.0, 0.0, 0.0, min(seen.z, -1e-4))); // the main pass keeps the sky's light
        imageStore(uSlope, xy, vec4(0.0, 0.0, 0.0, 1.0));
        return;
    }
    // E(d) = E(n) + slope . (d - n), fitted to the four (the side ways' mean gives it along the normal, their spread
    // across it), for the brightness only: the colour is the normal's.
    const float e0 = dot(light[0], luma), e1 = dot(light[1], luma), e2 = dot(light[2], luma),
                e3 = dot(light[3], luma);
    const float along = (e0 - (e1 + e2 + e3) / 3.0) / (1.0 - c);
    const float across_t = (e1 - 0.5 * (e2 + e3)) * (2.0 / (3.0 * s));
    const float across_b = (e2 - e3) * 0.8660254 * (2.0 / (3.0 * s));
    const vec3 g = clamp((across_t * t + across_b * b + along * n) / max(e0, 1e-6), vec3(-64.0), vec3(64.0));
    imageStore(uLight, xy, vec4(min(light[0] * params.probes.bias.z, vec3(65000.0)), max(-seen.z, 1e-4)));
    imageStore(uSlope, xy, vec4(g, 1.0 - dot(g, n)));
}
