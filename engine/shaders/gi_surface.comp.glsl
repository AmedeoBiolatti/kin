#version 450 core
// Reconstruct visibility holes from nearby, connected parts of the receiver plane. Window/readiness coverage is
// retained: borrowing light cannot extend a GI window or make untraced probes ready. Raw resolve history stays raw.
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D uDepth;
layout(set = 0, binding = 1) uniform sampler2D uNormals;
layout(set = 0, binding = 2) uniform sampler2D uLight;
layout(set = 0, binding = 3) uniform sampler2D uSlope;
layout(set = 0, binding = 4) uniform sampler2D uCoverage; // visible share, domain/readiness share
layout(set = 1, binding = 0, rgba16f) uniform writeonly image2D uSurfaceLight;
layout(set = 1, binding = 1, rgba16f) uniform writeonly image2D uSurfaceSlope;
layout(set = 1, binding = 2, r16f) uniform writeonly image2D uSurfaceCoverage;
layout(std140, set = 2, binding = 0) uniform Params {
    mat4 to_world;
    vec4 unproject_xy;
    vec4 unproject_zw;
    vec4 sizes;
    vec4 repair; // x: maximum world-space distance; y: receiver-plane tolerance
} params;
#include "kin_depth.inc.glsl"

bool receiver(ivec2 q, out vec3 p, out vec3 n) {
    if (any(lessThan(q, ivec2(0))) || any(greaterThanEqual(q, ivec2(params.sizes.xy)))) return false;
    const ivec2 last = ivec2(params.sizes.zw) - 1;
    ivec2 at = min(q * 2, last);
    float depth = texelFetch(uDepth, at, 0).r;
    for (int i = 1; i < 4; ++i) {
        const ivec2 candidate = min(q * 2 + ivec2(i & 1, i >> 1), last);
        const float d = texelFetch(uDepth, candidate, 0).r;
        if (d > depth) { depth = d; at = candidate; }
    }
    if (depth <= 0.0) return false;
    p = (params.to_world * vec4(view_position(at, depth), 1.0)).xyz;
    n = decode_normal(texelFetch(uNormals, at, 0).rg);
    return true;
}

bool same_surface(ivec2 q, vec3 p, vec3 n) {
    vec3 there, normal;
    if (!receiver(q, there, normal)) return false;
    const vec3 offset = there - p;
    return dot(n, normal) > 0.98 && abs(dot(offset, n)) <= params.repair.y &&
           dot(offset, offset) <= params.repair.x * params.repair.x;
}

void main() {
    const ivec2 xy = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(xy, ivec2(params.sizes.xy)))) return;
    vec4 light = texelFetch(uLight, xy, 0), slope = texelFetch(uSlope, xy, 0);
    const vec2 shares = texelFetch(uCoverage, xy, 0).rg;
    float coverage = shares.r;
    const float confidence = shares.g > 0.001 ? clamp(shares.r / shares.g, 0.0, 1.0) : 1.0;
    vec3 p, n;
    if (light.a > 0.0 && confidence < 0.98 && receiver(xy, p, n)) {
        const ivec2 directions[8] = ivec2[8](ivec2(1, 0), ivec2(1, 1), ivec2(0, 1), ivec2(-1, 1),
                                             ivec2(-1, 0), ivec2(-1, -1), ivec2(0, -1), ivec2(1, -1));
        // Stop at the nearest ring with reliable lighting. The world-space bound keeps this local at every FOV and
        // resolution; intermediate receivers reject a shortcut across a silhouette or a gap between objects.
        for (int radius = 1; radius <= 64; radius *= 2) {
            vec3 sum = vec3(0.0);
            vec4 sum_slope = vec4(0.0);
            float total = 0.0;
            for (int i = 0; i < 8; ++i) {
                const ivec2 offset = directions[i] * radius, q = xy + offset;
                if (any(lessThan(q, ivec2(0))) || any(greaterThanEqual(q, ivec2(params.sizes.xy)))) continue;
                const vec2 its = texelFetch(uCoverage, q, 0).rg;
                if (its.g <= 0.001 || its.r < 0.98 * its.g || !same_surface(q, p, n)) continue;
                if (radius > 1 && !same_surface(xy + offset / 2, p, n)) continue;
                if (radius > 3 && (!same_surface(xy + offset / 4, p, n) ||
                                  !same_surface(xy + offset * 3 / 4, p, n))) continue;
                const vec4 value = texelFetch(uLight, q, 0);
                if (value.a <= 0.0) continue;
                const float weight = ((i & 1) == 0 ? 1.0 : 0.5);
                sum += weight * value.rgb;
                sum_slope += weight * texelFetch(uSlope, q, 0);
                total += weight;
            }
            if (total > 0.0) {
                const float amount = 1.0 - smoothstep(0.85, 0.98, confidence);
                light.rgb = mix(light.rgb, sum / total, amount);
                slope = mix(slope, sum_slope / total, amount);
                coverage = mix(coverage, shares.g, amount);
                break;
            }
        }
    }
    imageStore(uSurfaceLight, xy, light);
    imageStore(uSurfaceSlope, xy, slope);
    imageStore(uSurfaceCoverage, xy, vec4(coverage));
}
