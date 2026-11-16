#version 450 core
// The clouds' frames blended (Clouds3D::history): each texel of this frame's walk (clouds_trace.comp, its start moved
// each frame) with the last blend where its clouds were then: the clouds' point (at their mean distance; with none,
// the direction) seen by the last frame's camera. That history is held within this frame's neighbourhood (the 3 x 3
// texels' least and most), so that what moved or appeared is not smeared. Out: the blend and its distance, read by
// clouds_apply.frag and by the next frame.
layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0) uniform sampler2D uCurrent;          // clouds_trace.comp's light and share behind
layout(set = 0, binding = 1) uniform sampler2D uCurrentDistance;  // and distance
layout(set = 0, binding = 2) uniform sampler2D uHistory;          // the last blend (read linearly)
layout(set = 0, binding = 3) uniform sampler2D uHistoryDistance;  // and its distance (read at the nearest texel)

layout(set = 1, binding = 0, rgba16f) uniform writeonly image2D uOut;
layout(set = 1, binding = 1, r32f) uniform writeonly image2D uOutDistance;

layout(std140, set = 2, binding = 0) uniform Params {
    mat4 to_world; // this frame's clip space (without the jitter) to world
    mat4 previous; // the last frame's view-projection (without the jitter)
    vec4 eye;      // xyz: the camera
    vec4 size;     // xy: the texels, z: the history's share, w: 1 with a history
} params;

void main() {
    const ivec2 xy = ivec2(gl_GlobalInvocationID.xy);
    const ivec2 size = ivec2(params.size.xy);
    if (any(greaterThanEqual(xy, size))) {
        return;
    }
    const vec4 current = texelFetch(uCurrent, xy, 0);
    const float dist = texelFetch(uCurrentDistance, xy, 0).r;
    if (params.size.w < 0.5) {
        imageStore(uOut, xy, current);
        imageStore(uOutDistance, xy, vec4(dist));
        return;
    }
    vec4 lo = current, hi = current;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            const vec4 n = texelFetch(uCurrent, clamp(xy + ivec2(x, y), ivec2(0), size - 1), 0);
            lo = min(lo, n);
            hi = max(hi, n);
        }
    }
    const vec2 uv = (vec2(xy) + 0.5) / params.size.xy;
    const vec4 near = params.to_world * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 1.0, 1.0);
    const vec3 dir = normalize(near.xyz / near.w - params.eye.xyz);
    const vec4 before = dist > 0.0 ? params.previous * vec4(params.eye.xyz + dir * dist, 1.0)
                                   : params.previous * vec4(dir, 0.0);
    vec4 result = current;
    float result_distance = dist;
    if (before.w > 0.0) {
        const vec2 ndc = before.xy / before.w;
        const vec2 then = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
        if (all(greaterThanEqual(then, vec2(0.0))) && all(lessThanEqual(then, vec2(1.0)))) {
            const vec4 history = clamp(textureLod(uHistory, then, 0.0), lo, hi);
            result = mix(current, history, params.size.z);
            const float d = texelFetch(uHistoryDistance, clamp(ivec2(then * params.size.xy), ivec2(0), size - 1), 0).r;
            if (dist > 0.0 && d > 0.0) {
                result_distance = mix(dist, d, params.size.z);
            }
        }
    }
    imageStore(uOut, xy, result);
    imageStore(uOutDistance, xy, vec4(result_distance));
}
