#version 450 core
// The clouds' shadow map (Clouds3D::shadows): for each texel of a square of the plane at the layer's middle height
// (`shadow_extent` across, round where the camera's ray toward the sun crosses it), the sun's light that gets through
// the layer along the sun's ray through that point: e^-tau, tau summed over `steps` samples between the base and the
// top of the clouds' shapes without their detail (the shell's curvature left out: the square is small beside the
// planet). Read with kin_cloud_shadow.
layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0) uniform sampler3D uShape;
layout(set = 0, binding = 1) uniform sampler3D uDetail;
layout(set = 0, binding = 2) uniform sampler2D uWeather; // the coverage map: r coverage, g kind (Clouds3D::weather_map or the default)

layout(set = 1, binding = 0, rgba16f) uniform writeonly image2D uOut; // r: the share of the sun through

#include "kin_clouds.inc.glsl"

layout(std140, set = 2, binding = 0) uniform Params {
    vec4 map;    // as kin_cloud_shadow's: the square's middle (x, z), 1 / its side, the plane's height
    vec4 toward; // xyz: toward the sun (height at least 0.05); w: samples
    vec4 size;   // x: the texels across
    KinClouds clouds;
} params;

void main() {
    const ivec2 xy = ivec2(gl_GlobalInvocationID.xy);
    const int n = int(params.size.x);
    if (any(greaterThanEqual(xy, ivec2(n)))) {
        return;
    }
    const KinClouds c = params.clouds;
    const vec2 q = params.map.xy + ((vec2(xy) + 0.5) / float(n) - 0.5) / params.map.z;
    const float bottom = min(c.layer.x, c.layer.y), top = max(c.layer.x, c.layer.y);
    const vec3 l = params.toward.xyz;
    const int steps = int(params.toward.w);
    const float dy = (top - bottom) / float(steps);
    const float ds = dy / l.y; // the ray's length per sample
    float depth = 0.0;
    // Each texel's samples moved by its own share of a step (interleaved gradient noise): with the same heights
    // everywhere, the steps lined up into bands across the ground along the sun.
    const float offset = fract(52.9829189 * fract(dot(vec2(xy), vec2(0.06711056, 0.00583715))));
    for (int k = 0; k < steps; ++k) {
        const float y = bottom + (float(k) + offset) * dy;
        const vec3 p = vec3(q.x + l.x / l.y * (y - params.map.w), y, q.y + l.z / l.y * (y - params.map.w));
        // (The shapes without their worn detail: a shadow from kilometres up is soft, and the detail, finer than the
        // samples' spacing along the sun, banded it.)
        depth += kin_cloud_density(c, uShape, uDetail, uWeather, p, y, false);
    }
    imageStore(uOut, xy, vec4(exp(-depth * c.cover.z * ds), 0.0, 0.0, 1.0));
}
