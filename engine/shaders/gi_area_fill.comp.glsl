#version 450 core
// kin's area lights with GlobalIllumination3D::area_lights.interleave 2 or 4, before gi_area.comp: every texel but
// this render's share (half: the texels whose x + y has the render's parity; a quarter: the one at (mode.y, mode.w) of
// each 2 x 2), which gi_area.comp finds next, takes where its surface was in the render before's whole (by the motion
// between the two views without the temporal anti-aliasing's jitter: a still view's texel is its own), when that held
// the same surface (its depth along that view within 2%, the four pixels under it facing the same way); else it is
// listed, and gi_area.comp finds it with the share (the same indirect dispatch: the share's groups, then as many as
// the list needs). The coverage is gi_resolve.comp's, as gi_area.comp carries it. RGBA16F, half the view's size; it
// is the next render's "before".
layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0) uniform sampler2D uDepth;     // reverse Z, 0 is nothing (full size)
layout(set = 0, binding = 1) uniform sampler2D uBefore;    // the render before's whole
layout(set = 0, binding = 2) uniform sampler2D uLight;     // gi_resolve.comp's this render: a, its depth along the view
layout(set = 0, binding = 3) uniform sampler2D uLastLight; // and the render before's
layout(set = 0, binding = 4) uniform sampler2D uCoverage;
layout(set = 0, binding = 5) uniform sampler2D uNormals;     // this render's world normals, octahedral (full size)
layout(set = 0, binding = 6) uniform sampler2D uPastNormals; // the render before's (gi_resolve's)
layout(set = 1, binding = 0, rgba16f) uniform writeonly image2D uArea;
// The texels no render before could give, for gi_area.comp to find: two turns, this render's appended to and the
// other's cleared for the next. Each turn: gi_area.comp's indirect dispatch's groups (x: the share's tiles and the
// list's groups; y, z: 1) in RetryArgs, the count and the texels (x | y << 16) in RetryList.
layout(std430, set = 1, binding = 1) buffer RetryArgs {
    uint retry_args[];
};
layout(std430, set = 1, binding = 2) buffer RetryList {
    uint retry_list[];
};

layout(std140, set = 2, binding = 0) uniform Params {
    mat4 to_world;     // the camera's space to the world's
    vec4 unproject_xy; // the inverse projection, as gtao.comp's
    vec4 unproject_zw;
    vec4 sizes; // xy: the result's size, zw: the depth's
    mat4 view_projection;      // this render's, without the temporal anti-aliasing's jitter
    mat4 last_view_projection; // the render before's, without it
    vec4 last_eye;             // xyz: its camera; w: 1 when there is a render before to take from
    vec4 last_forward;         // xyz: its way
    vec4 retry; // x: the share's tiles (gi_area.comp's groups before the list's); y: this turn's list's first word, z:
                // the other turn's; w: this turn's arguments' first word (the other's at 4 - w)
    vec4 mode; // x: 2 for a quarter (the texels at (y, w) of each 2 x 2), else the checkerboard's half (y: its
               // parity); z: 1 with the render before's normals
} params;

#include "kin_depth.inc.glsl"

void main() {
    const ivec2 xy = ivec2(gl_GlobalInvocationID.xy);
    if (gl_GlobalInvocationID.x == 0u && gl_GlobalInvocationID.y == 0u) {
        const uint other = 4u - uint(params.retry.w);
        retry_args[other] = uint(params.retry.x);
        retry_args[other + 1u] = 1u;
        retry_args[other + 2u] = 1u;
        retry_list[uint(params.retry.z)] = 0u;
    }
    const ivec2 size = ivec2(params.sizes.xy);
    if (any(greaterThanEqual(xy, size))) {
        return;
    }
    const float coverage = texelFetch(uCoverage, xy, 0).r;
    const bool quarter = params.mode.x > 1.5;
    const ivec2 at_quarter = ivec2(params.mode.yw);
    if (quarter ? all(equal(xy & 1, at_quarter)) : ((xy.x + xy.y + int(params.mode.y)) & 1) == 0) {
        return; // this render's share: gi_area.comp's
    }
    const float here = abs(texelFetch(uLight, xy, 0).a);
    if (here <= 0.0) {
        imageStore(uArea, xy, vec4(0.0, 0.0, 0.0, coverage)); // nothing drawn
        return;
    }
    // The texel's surface: the nearest pixel of its 2 x 2 (gi_resolve's choice), and its normal.
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
    const vec3 n = decode_normal(texelFetch(uNormals, at, 0).rg);
    // Where it was, seen by the last view.
    if (params.last_eye.w > 0.5) {
        const vec3 point = (params.to_world * vec4(view_position(at, depth), 1.0)).xyz;
        const vec4 clip = params.last_view_projection * vec4(point, 1.0);
        const vec4 now = params.view_projection * vec4(point, 1.0);
        if (clip.w > 0.0 && now.w > 0.0) {
            // The texel moved by the point's motion between the two views (none for a still camera: the same texel).
            const vec2 motion = (clip.xy / clip.w - now.xy / now.w) * vec2(0.5, -0.5);
            const vec2 uv = (vec2(xy) + 0.5) / vec2(size) + motion;
            if (all(greaterThanEqual(uv, vec2(0.0))) && all(lessThan(uv, vec2(1.0)))) {
                const ivec2 was = min(ivec2(floor(uv * vec2(size))), size - 1);
                const float then = abs(texelFetch(uLastLight, was, 0).a);
                const float expected = dot(point - params.last_eye.xyz, params.last_forward.xyz);
                bool same = then > 0.0 && abs(then - expected) <= 0.02 * expected;
                for (int z = 0; z < 4 && same; ++z) {
                    const ivec2 p = min(was * 2 + ivec2(z & 1, z >> 1), textureSize(uPastNormals, 0) - 1);
                    same = dot(n, decode_normal(texelFetch(uPastNormals, p, 0).rg)) > 0.98;
                }
                if (same) {
                    imageStore(uArea, xy, vec4(texelFetch(uBefore, was, 0).rgb, coverage));
                    return;
                }
            }
        }
    }
    // Not to be had from the render before: listed, for gi_area.comp to find with the share.
    const uint base = uint(params.retry.y), i = atomicAdd(retry_list[base], 1u);
    retry_list[base + 1u + i] = uint(xy.x) | (uint(xy.y) << 16);
    atomicMax(retry_args[uint(params.retry.w)], uint(params.retry.x) + (i >> 6) + 1u);
}
