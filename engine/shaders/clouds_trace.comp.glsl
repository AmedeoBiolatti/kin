#version 450 core
// The clouds (Scene3D::clouds) along each ray of the view at the walk's resolution: from where the ray enters the
// layer to where it leaves it (or meets the ground, or `distance`), in steps started at a different point every frame
// (clouds_resolve.comp blends the frames). At each sample with cloud, the light toward the light source (the sun, or
// the moon at night) is walked for its optical depth, and the cloud scatters it by two lobes and octaves of the light
// scattered many times (Wrenninge's, as Hillaire's and Unreal's), with the sky's ambient light; Hillaire's
// energy-conserving step integrates each sample's slab. Then the air in front of the clouds (the procedural sky's) and
// the fog (as the sky behind them takes it). Out: rgb the light added over what is behind, a the share of what is
// behind still seen; and the clouds' mean distance (weighted by the light they take out), the depth they are drawn at.
layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0) uniform sampler3D uShape;
layout(set = 0, binding = 1) uniform sampler3D uDetail;
layout(set = 0, binding = 2) uniform sampler2D uWeather; // the coverage map: r coverage, g kind (Clouds3D::weather_map or the default)

layout(set = 0, binding = 3) uniform sampler2D uDepth; // the pre-pass's, reverse Z: 0 is nothing (depth.z: read)

layout(set = 1, binding = 0, rgba16f) uniform writeonly image2D uOut;
layout(set = 1, binding = 1, r32f) uniform writeonly image2D uDistance;

#include "kin_clouds.inc.glsl"

layout(std140, set = 2, binding = 0) uniform Params {
    mat4 to_world; // clip space (without the jitter) to world
    vec4 eye;      // xyz: the camera, w: this frame's number (the start's offset moves with it)
    vec4 size;     // xy: the walk's texels, z: 1 for an orthographic camera
    KinClouds clouds;
    KinSky atmosphere;
    KinFog fog;
    vec4 unproject_xy; // the depth to view space, as gtao.comp's
    vec4 unproject_zw;
    vec4 depth;        // xy: the depth's size; z: 1 when it bounds the rays
} params;

// The distance to the farthest surface the pre-pass drew in the walk texel `xy`'s pixels (the four corners of its
// footprint): the clouds past it are hidden by what is nearer in each pixel (the drawing's depth test), the clouds in
// front of it are drawn over it. 1e30: nothing drawn (the sky), or no depth.
float kin_cloud_limit(ivec2 xy) {
    if (params.depth.z < 0.5) {
        return 1e30;
    }
    vec2 scale = params.depth.xy / params.size.xy;
    ivec2 last = ivec2(params.depth.xy) - 1;
    ivec2 p0 = min(ivec2(floor(vec2(xy) * scale)), last);
    ivec2 p1 = clamp(ivec2(floor(vec2(xy + 1) * scale)) - 1, p0, last);
    float farthest = 1.0;
    for (int i = 0; i < 4; ++i) {
        ivec2 p = ivec2((i & 1) == 0 ? p0.x : p1.x, (i & 2) == 0 ? p0.y : p1.y);
        farthest = min(farthest, texelFetch(uDepth, p, 0).r);
    }
    if (farthest <= 0.0) {
        return 1e30;
    }
    // The view-space point of the footprint's middle at that depth (rows down in the picture, up in clip space).
    vec2 pixel = 0.5 * vec2(p0 + p1) + 0.5;
    vec2 ndc = vec2(pixel.x, params.depth.y - pixel.y) * (2.0 / params.depth.xy) - 1.0;
    float w = params.unproject_zw.z * farthest + params.unproject_zw.w;
    vec3 seen = vec3(params.unproject_xy.x * ndc.x + params.unproject_xy.z,
                     params.unproject_xy.y * ndc.y + params.unproject_xy.w,
                     params.unproject_zw.x * farthest + params.unproject_zw.y) / w;
    if (params.size.z > 0.5) {
        // Orthographic: along the ray from its point on the near plane (depth 1), the view depth between them.
        float near_z = (params.unproject_zw.x + params.unproject_zw.y) / (params.unproject_zw.z + params.unproject_zw.w);
        return abs(seen.z - near_z);
    }
    return length(seen);
}

void main() {
    const ivec2 xy = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(xy, ivec2(params.size.xy)))) {
        return;
    }
    const vec2 uv = (vec2(xy) + 0.5) / params.size.xy;
    const vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    const vec4 near = params.to_world * vec4(ndc, 1.0, 1.0);
    // The ray: from the camera through the pixel, or (orthographic: size.z) parallel to the view from the pixel's point
    // on the near plane toward its point on the far one.
    vec3 eye = params.eye.xyz;
    vec3 dir;
    if (params.size.z > 0.5) {
        const vec4 far_point = params.to_world * vec4(ndc, 0.0, 1.0);
        eye = near.xyz / near.w;
        dir = normalize(far_point.xyz / far_point.w - eye);
    } else {
        dir = normalize(near.xyz / near.w - eye);
    }
    vec2 span = kin_cloud_span(params.clouds, eye, dir);
    span.y = min(span.y, kin_cloud_limit(xy));
    if (span.x >= span.y) {
        imageStore(uOut, xy, vec4(0.0, 0.0, 0.0, 1.0));
        imageStore(uDistance, xy, vec4(0.0));
        return;
    }
    // The samples' start moves each frame (interleaved gradient noise, its pattern shifted).
    const float frame = mod(params.eye.w, 64.0);
    const vec2 noise_at = vec2(xy) + 5.588238 * frame;
    const float offset = fract(52.9829189 * fract(dot(noise_at, vec2(0.06711056, 0.00583715))));
    float dist;
    const vec4 clouds = kin_cloud_march(params.clouds, params.atmosphere, params.fog, uShape, uDetail, uWeather, eye, dir,
                                        span, offset, dist);
    imageStore(uOut, xy, clouds);
    imageStore(uDistance, xy, vec4(dist));
}
