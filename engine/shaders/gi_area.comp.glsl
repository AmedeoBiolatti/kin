#version 450 core
// kin's area lights (GlobalIllumination3D::area_lights; kin_area_light.inc.glsl): the light of the emissive surfaces
// lit exactly, on what the camera sees, at gi_resolve.comp's texels (half the view's size, each the nearest surface of
// its 2 x 2 pixels), with gi_resolve's coverage carried along: rgb is the irradiance over pi facing the surface, a the
// probes' share. The main pass (KIN_GI) reads it where it reads the coverage, and adds the light as a direct light's.
// With GlobalIllumination3D::area_lights.interleave 2 or 4, gi_area_fill.comp first takes every texel it can from the
// render before and lists those it cannot; then one indirect dispatch of this finds this render's share (half the
// texels: those whose x + y has the render's parity; or a quarter: one of each 2 x 2, in turn; the threads packed
// onto them, so no lane idles), 8 x 8 tiles of it first, and after them the listed texels, 64 a group.
layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0) uniform sampler2D uDepth;    // reverse Z, 0 is nothing
layout(set = 0, binding = 1) uniform sampler2D uNormals;  // world normals, octahedral
layout(set = 0, binding = 2) uniform sampler2D uCoverage; // gi_resolve.comp's
layout(std430, set = 0, binding = 3) readonly buffer AreaTriangles {
    vec4 kin_area_triangles[];
};
layout(std430, set = 0, binding = 4) readonly buffer AreaNodes {
    vec4 kin_area_nodes[];
};
layout(std430, set = 0, binding = 5) readonly buffer AreaEmitters {
    vec4 kin_area_emitters[];
};
layout(std430, set = 0, binding = 6) readonly buffer Emitters {
    float strengths[]; // the GI's emitters' strengths this render
};
layout(std430, set = 0, binding = 7) readonly buffer AreaScene {
    vec4 kin_area_scene[]; // the copies' hierarchy, then the copies
};
layout(std430, set = 0, binding = 8) readonly buffer Retry {
    uint retry[]; // gi_area_fill's lists of texels to find: at share.z, the count, then each (x | y << 16)
};
layout(set = 1, binding = 0, rgba16f) uniform writeonly image2D uArea;

layout(std140, set = 2, binding = 0) uniform Params {
    mat4 to_world;     // the camera's space to the world's
    vec4 unproject_xy; // the inverse projection, as gtao.comp's
    vec4 unproject_zw;
    vec4 sizes; // xy: the result's size, zw: the depth's
    vec4 eye;   // xyz: the camera (world)
    vec4 area;  // x: emitters, y: the light's strength (the probes'), z, w: the shadow rays' offset (base, a unit away)
    vec4 scene; // x: where the copies start in AreaScene; y, z: the settled hierarchy's first node and triangle, w: 1
                // when there is one
    vec4 mode;  // x: 0 every texel (a 2-D dispatch); 1 half (y: the render's parity), 2 a quarter (the texel at
                // (y, z) of each 2 x 2): a 1-D dispatch of the share's tiles, then the list's groups
    vec4 share; // x: the share's tiles across, y: its tiles, z: the list's first word in Retry
} params;

float kin_area_strength(uint gi_emitter) {
    return strengths[gi_emitter - 1u];
}

#define KIN_AREA_LINES 4
#define KIN_AREA_EXACT_FORM
#include "kin_area_light.inc.glsl"
#include "kin_depth.inc.glsl"

void main() {
    ivec2 xy = ivec2(gl_GlobalInvocationID.xy);
    if (params.mode.x > 0.5) {
        const uint group = gl_WorkGroupID.x, tiles = uint(params.share.y);
        if (group < tiles) {
            const uint across = uint(params.share.x);
            const ivec2 at = ivec2(group % across, group / across) * 8 + ivec2(gl_LocalInvocationID.xy);
            xy = params.mode.x > 1.5 ? at * 2 + ivec2(params.mode.yz) // the quarter: the one of each 2 x 2 at (y, z)
                                     : ivec2(at.x * 2 + ((at.y + int(params.mode.y)) & 1), at.y); // the half
        } else {
            const uint base = uint(params.share.z), i = (group - tiles) * 64u + gl_LocalInvocationIndex;
            if (i >= retry[base]) {
                return;
            }
            const uint packed = retry[base + 1u + i];
            xy = ivec2(int(packed & 0xffffu), int(packed >> 16));
        }
    }
    if (any(greaterThanEqual(xy, ivec2(params.sizes.xy)))) {
        return;
    }
    const float coverage = texelFetch(uCoverage, xy, 0).r;
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
        imageStore(uArea, xy, vec4(0.0, 0.0, 0.0, coverage));
        return;
    }
    const vec3 point = (params.to_world * vec4(view_position(at, depth), 1.0)).xyz;
    const vec3 n = decode_normal(texelFetch(uNormals, at, 0).rg);
    const float away = length(point - params.eye.xyz);
    const vec3 o = point + n * (params.area.z + params.area.w * away);
    const vec3 light = kin_area_light(uint(params.area.x), uvec4(params.scene), point, n, o) * params.area.y;
    imageStore(uArea, xy, vec4(min(light, vec3(65000.0)), coverage));
}
