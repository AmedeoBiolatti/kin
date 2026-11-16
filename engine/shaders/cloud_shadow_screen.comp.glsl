#version 450 core
// The clouds' shadow on what the pre-pass drew (Clouds3D::shadows): for each pixel, the share of the sun its surface
// gets through the clouds (kin_cloud_shadow at its world point), times the contact shadows' share when they were
// marched. The main pass reads it in their place (uContactShadow): the sun's term of every lit surface the pre-pass
// drew, and the decals, without a sampler more.
layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0) uniform sampler2D uDepth;   // the pre-pass's, reverse Z: 0 is nothing
layout(set = 0, binding = 1) uniform sampler2D uShadow;  // cloud_shadow.comp's
layout(set = 0, binding = 2) uniform sampler2D uContact; // the contact shadows (white without)

layout(set = 1, binding = 0, r8) uniform writeonly image2D uOut;

#include "kin_cloud_shadow.inc.glsl"

layout(std140, set = 2, binding = 0) uniform Params {
    mat4 to_world;     // view space to world (the camera's)
    vec4 unproject_xy; // the inverse projection, as gtao.comp's
    vec4 unproject_zw;
    vec4 sizes;        // xy: the result's size, zw: the depth's
    vec4 map;          // kin_cloud_shadow's
    vec4 toward;
} params;

#include "kin_depth.inc.glsl"

void main() {
    const ivec2 xy = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(xy, ivec2(params.sizes.xy)))) {
        return;
    }
    const float depth = texelFetch(uDepth, xy, 0).r;
    // (Without contact shadows, a 1 x 1 white texture: read at its one texel.)
    const float contact = texelFetch(uContact, min(xy, textureSize(uContact, 0) - 1), 0).r;
    float through = 1.0;
    if (depth > 0.0) {
        const vec3 p = (params.to_world * vec4(view_position(xy, depth), 1.0)).xyz;
        through = kin_cloud_shadow(uShadow, params.map, params.toward, p);
    }
    imageStore(uOut, xy, vec4(contact * through, 0.0, 0.0, 1.0));
}
