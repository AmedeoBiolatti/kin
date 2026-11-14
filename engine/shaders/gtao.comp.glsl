#version 450 core
// kin's ambient occlusion (Scene3D::ambient_occlusion): ground-truth AO
// (Jimenez et al., 2016) from the pre-pass's depth and normals. Each pixel
// looks along two slices through itself (a different angle every pixel, and
// different step offsets, to be blurred out by ao_blur.comp), finds how high
// the surface rises on each side by marching the depth buffer, and
// integrates the cosine-weighted visible arc between those horizons round the
// normal. A thickness heuristic lets the horizon sink again behind a thin
// occluder, so poles and railings do not shut out all that is behind them.
// Writes the share of the ambient light that reaches the pixel (1: all).
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D uDepth;   // reverse Z, 0 is nothing
layout(set = 0, binding = 1) uniform sampler2D uNormals; // world normals, octahedral
layout(set = 1, binding = 0, rg8) uniform writeonly image2D uResult;
layout(set = 2, binding = 0) uniform Params {
    mat4 view;           // world to the camera's (its rotation turns the normals)
    // The inverse projection, without its zeroes (no skew): x and y are
    // (a ndc.x + c, b ndc.y + d) over w, z is (e depth + f) over w, w is g depth + h.
    vec4 unproject_xy;   // a, b, c, d
    vec4 unproject_zw;   // e, f, g, h
    vec4 sizes;          // xy: the result's size, zw: the depth's
    vec4 shape;          // x: radius (units), y: the power of the result, z: pixels a unit spans at depth 1, w: 1 if orthographic
    vec4 detail;         // x: full-weight reach beneath the GI lookup offset
} params;

const float PI = 3.14159265358979;
const float HALF_PI = 1.5707963267949;
const int SLICES = 2;
const int STEPS = 8;
// The share of the way down to the lowest horizon a sample below the current
// one pulls it: a thin occluder is only in the way for a moment.
const float THICKNESS = 0.15;
// Samples fade out over the outer 0.615 of the radius (XeGTAO's), so a distant
// thing does not pop in or out of the shadow with the radius.
const float FALLOFF = 0.615;
// A lone pixel cannot say more than this: never black.
const float LEAST = 0.03;
// Never look further than this many depth pixels (a surface at arm's length
// would otherwise spread over the screen, for a cache that is missing all the time).
const float MOST_PIXELS_PER_VIEW = 0.12;

#include "kin_depth.inc.glsl"

void main() {
    const ivec2 xy = ivec2(gl_GlobalInvocationID.xy);
    const ivec2 size = ivec2(params.sizes.xy);
    if (xy.x >= size.x || xy.y >= size.y) {
        return;
    }
    const ivec2 depth_size = ivec2(params.sizes.zw);
    // The depth pixel this one stands for (the same, or the corner of its 2 x 2).
    const ivec2 at = min(ivec2(vec2(xy) * params.sizes.zw / params.sizes.xy), depth_size - 1);
    const float depth = texelFetch(uDepth, at, 0).r;
    if (depth <= 0.0) {
        imageStore(uResult, xy, vec4(1.0));
        return;
    }
    const bool ortho = params.shape.w > 0.5;
    const vec3 p = view_position(at, depth);
    const vec3 v = ortho ? vec3(0.0, 0.0, 1.0) : normalize(-p);
    const vec3 n = normalize(mat3(params.view) * decode_normal(texelFetch(uNormals, at, 0).rg));

    const float radius = params.shape.x;
    // The lookup offset bounds unresolved geometry. Preserve full occlusion inside it; the authored
    // falloff then fades that detail out, capped by the ordinary AO radius.
    const float detail_radius = min(params.detail.x / (1.0 - FALLOFF), radius);
    const bool separate_detail = detail_radius < radius;
    const float detail_reach = min(params.shape.z * detail_radius / (ortho ? 1.0 : max(-p.z, 1e-3)),
                                  MOST_PIXELS_PER_VIEW * params.sizes.w);
    const bool gather_detail = separate_detail && detail_reach >= 1.5;
    const float detail_falloff_mul = -1.0 / (FALLOFF * detail_radius);
    const float detail_falloff_add = 1.0 / FALLOFF;

    const float reach = min(params.shape.z * radius / (ortho ? 1.0 : max(-p.z, 1e-3)),
                            MOST_PIXELS_PER_VIEW * params.sizes.w);
    if (reach < 1.5) {
        imageStore(uResult, xy, vec4(1.0)); // less than a pixel across: no occlusion to see
        return;
    }
    const float falloff_range = FALLOFF * radius;
    const float falloff_mul = -1.0 / falloff_range;
    const float falloff_add = (radius - falloff_range) / falloff_range + 1.0;

    const float angle_noise = noise(vec2(at));
    const float step_noise = noise(vec2(at.yx) + vec2(37.0, 17.0));
    float visibility = 0.0, detail_visibility = 0.0, detail_open = 0.0;
    for (int slice = 0; slice < SLICES; ++slice) {
        const float phi = (float(slice) + angle_noise) * (PI / float(SLICES));
        const vec2 omega = vec2(cos(phi), -sin(phi)); // in depth pixels (y down)
        const vec3 direction = vec3(cos(phi), sin(phi), 0.0);
        const vec3 ortho_direction = direction - dot(direction, v) * v;
        const vec3 axis = normalize(cross(ortho_direction, v));
        const vec3 projected = n - axis * dot(n, axis);
        const float projected_length = max(length(projected), 1e-4);
        const float sign_n = sign(dot(ortho_direction, projected));
        const float cos_n = clamp(dot(projected, v) / projected_length, 0.0, 1.0);
        const float angle_n = sign_n * acos(cos_n);

        // The horizons start at the tangent plane's: nothing shuts out more than that.
        const float low0 = cos(angle_n + HALF_PI);
        const float low1 = cos(angle_n - HALF_PI);
        float horizon0 = low0;
        float horizon1 = low1;
        float detail0 = low0, detail1 = low1;
        for (int i = 0; i < STEPS; ++i) {
            float s = (float(i) + step_noise) / float(STEPS);
            s *= s; // more samples near the pixel
            const float offset = max(s * reach, float(i) + 1.0);
            const ivec2 o = ivec2(round(omega * offset));
            for (int side = 0; side < 2; ++side) {
                const ivec2 q = side == 0 ? at + o : at - o;
                if (q.x < 0 || q.y < 0 || q.x >= depth_size.x || q.y >= depth_size.y) {
                    continue; // off the screen: nothing known there
                }
                const float d = texelFetch(uDepth, q, 0).r;
                if (d <= 0.0) {
                    continue; // the sky
                }
                const vec3 delta = view_position(q, d) - p;
                const float dist = length(delta);
                const float weight = clamp(dist * falloff_mul + falloff_add, 0.0, 1.0);
                // A second horizon pair reuses the taps inside the unresolved GI near field.
                // Far taps must not relax a short-range horizon back to the tangent plane.
                if (gather_detail && dist <= detail_radius) {
                    const float local_weight = clamp(dist * detail_falloff_mul + detail_falloff_add, 0.0, 1.0);
                    const float local_h = mix(side == 0 ? low0 : low1, dot(delta, v) / max(dist, 1e-5), local_weight);
                    const float local_relaxation = dot(delta, n) > max(0.005, detail_radius * 0.01) ? 0.0 : THICKNESS;
                    if (side == 0)
                        detail0 = local_h > detail0 ? local_h : mix(detail0, local_h, local_relaxation);
                    else
                        detail1 = local_h > detail1 ? local_h : mix(detail1, local_h, local_relaxation);
                }
                float h = dot(delta, v) / max(dist, 1e-5);
                h = mix(side == 0 ? low0 : low1, h, weight);
                // A continuing raised surface (a recess's frame/wall) still blocks the horizon. Relax only
                // after returning to the receiver plane; the tolerance excludes coplanar depth/normal noise.
                const float relaxation = dot(delta, n) > max(0.005, radius * 0.01) ? 0.0 : THICKNESS;
                if (side == 0) {
                    horizon0 = h > horizon0 ? h : mix(horizon0, h, relaxation);
                } else {
                    horizon1 = h > horizon1 ? h : mix(horizon1, h, relaxation);
                }
            }
        }
        // Integrate the visible arc between the horizons, clamped to the
        // hemisphere round the normal, weighted by the cosine.
        float h0 = -acos(clamp(horizon1, -1.0, 1.0));
        float h1 = acos(clamp(horizon0, -1.0, 1.0));
        h0 = angle_n + clamp(h0 - angle_n, -HALF_PI, HALF_PI);
        h1 = angle_n + clamp(h1 - angle_n, -HALF_PI, HALF_PI);
        const float arc0 = (cos_n + 2.0 * h0 * sin(angle_n) - cos(2.0 * h0 - angle_n)) * 0.25;
        const float arc1 = (cos_n + 2.0 * h1 * sin(angle_n) - cos(2.0 * h1 - angle_n)) * 0.25;
        visibility += projected_length * (arc0 + arc1);
        if (gather_detail) {
            // Residual visibility is relative to these same slices without local occluders. Their two-slice
            // approximation of the open hemisphere need not be one, especially at grazing normals.
            detail_open += projected_length * (cos_n + angle_n * sin(angle_n));
            float d0 = -acos(clamp(detail1, -1.0, 1.0));
            float d1 = acos(clamp(detail0, -1.0, 1.0));
            d0 = angle_n + clamp(d0 - angle_n, -HALF_PI, HALF_PI);
            d1 = angle_n + clamp(d1 - angle_n, -HALF_PI, HALF_PI);
            const float a0 = (cos_n + 2.0 * d0 * sin(angle_n) - cos(2.0 * d0 - angle_n)) * 0.25;
            const float a1 = (cos_n + 2.0 * d1 * sin(angle_n) - cos(2.0 * d1 - angle_n)) * 0.25;
            detail_visibility += projected_length * (a0 + a1);
        }
    }
    visibility = clamp(visibility / float(SLICES), LEAST, 1.0);
    const float full = pow(visibility, params.shape.y);
    const float local = gather_detail
                           ? pow(clamp(detail_visibility / max(detail_open, 1e-5), LEAST, 1.0), params.shape.y)
                           : 1.0;
    // Removing distant occluders must not make the short-range result darker.
    imageStore(uResult, xy, vec4(full, separate_detail ? max(full, local) : full, 0.0, 0.0));
}
