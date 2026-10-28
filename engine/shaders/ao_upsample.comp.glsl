#version 450 core
// Reconstruct half-size AO on the full-size geometry. Linear filtering alone
// carries a background surface's occlusion onto a foreground silhouette.
// Reject neighbours at different depths or with different normals instead.
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D uDepth;
layout(set = 0, binding = 1) uniform sampler2D uNormals;
layout(set = 0, binding = 2) uniform sampler2D uSource;
layout(set = 1, binding = 0, r8) uniform writeonly image2D uResult;
// Shared with the horizon search's OcclusionUniforms.
layout(set = 2, binding = 0) uniform Params {
    mat4 view;
    vec4 unproject_xy;
    vec4 unproject_zw;
    vec4 sizes; // xy: AO size, zw: full-size depth/result
    vec4 shape; // x: AO radius
} params;

#include "kin_depth.inc.glsl"

float view_distance(float depth) {
    return -(params.unproject_zw.x * depth + params.unproject_zw.y) /
            (params.unproject_zw.z * depth + params.unproject_zw.w);
}

void main() {
    const ivec2 xy = ivec2(gl_GlobalInvocationID.xy);
    const ivec2 size = ivec2(params.sizes.zw);
    if (any(greaterThanEqual(xy, size)))
        return;
    const float depth = texelFetch(uDepth, xy, 0).r;
    if (depth <= 0.0) {
        imageStore(uResult, xy, vec4(1.0));
        return;
    }
    const ivec2 ao_size = ivec2(params.sizes.xy);
    const vec2 scale = params.sizes.zw / params.sizes.xy;
    const ivec2 lo = min(ivec2(vec2(xy) / scale), ao_size - 1);
    const ivec2 hi = min(lo + 1, ao_size - 1);
    // GTAO samples floor(ao_pixel * scale), not a bilinear texture centre.
    // Use those same locations, including the nonuniform cells of odd sizes.
    const ivec2 start = ivec2(vec2(lo) * scale);
    const ivec2 end = ivec2(vec2(hi) * scale);
    const vec2 fraction = clamp(vec2(xy - start) / vec2(max(end - start, ivec2(1))), 0.0, 1.0);
    // At an original sample, interpolation has exactly one nonzero weight.
    // Reuse that value without fetching neighbours/normals or evaluating exp.
    const bvec2 at_start = equal(xy, start), at_end = equal(xy, end);
    if ((at_start.x || at_end.x) && (at_start.y || at_end.y)) {
        const ivec2 q = ivec2(at_start.x ? lo.x : hi.x, at_start.y ? lo.y : hi.y);
        imageStore(uResult, xy, vec4(texelFetch(uSource, q, 0).r));
        return;
    }
    const float centre = view_distance(depth);
    const vec3 normal = decode_normal(texelFetch(uNormals, xy, 0).rg);
    const float edge = max(min(0.03 * centre, 0.25 * params.shape.x), 1e-4);
    float sum = 0.0, total = 0.0;
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            const float spatial = (x == 0 ? 1.0 - fraction.x : fraction.x) *
                                  (y == 0 ? 1.0 - fraction.y : fraction.y);
            if (spatial <= 0.0)
                continue;
            const ivec2 q = ivec2(x == 0 ? lo.x : hi.x, y == 0 ? lo.y : hi.y);
            const ivec2 at = min(ivec2(vec2(q) * scale), size - 1);
            const float d = texelFetch(uDepth, at, 0).r;
            if (d <= 0.0)
                continue;
            const vec3 n = decode_normal(texelFetch(uNormals, at, 0).rg);
            const float alignment = max(dot(normal, n), 0.0);
            const float a2 = alignment * alignment;
            const float a4 = a2 * a2;
            const float weight = spatial * exp(-abs(view_distance(d) - centre) / edge) * a4 * a4;
            sum += texelFetch(uSource, q, 0).r * weight;
            total += weight;
        }
    }
    // No neighbour represents this surface: avoid borrowing a dark background.
    imageStore(uResult, xy, vec4(total > 1e-5 ? sum / total : 1.0));
}
