#version 300 es
// Volumetric clouds in WebGL2: native clouds_trace.comp as a fragment of the walk's picture (rows up, as GL's): the
// clouds along the pixel's ray (kin_cloud_march), its start moved each frame; out0 rgb the light they add, a the share
// of what is behind still seen; out1 r their mean distance.
precision highp float;
precision highp int;
precision highp sampler3D;
#include "kin_clouds.inc.glsl"
uniform highp sampler3D uShape, uDetail;
uniform highp sampler2D uWeather;
uniform mat4 uToWorld; // clip space (no jitter: none in the browser) to world
uniform vec4 uEye;     // xyz: the camera, w: the frame's number
uniform vec2 uSize;    // the walk's texels
uniform KinClouds uClouds;
uniform KinSky uSky;
uniform KinFog uFog;
uniform highp sampler2D uDepth; // the opaque meshes' depth (kin's reverse Z 0 to 1; 0 nothing)
uniform vec4 uUnprojectXY, uUnprojectZW; // the depth to view space, as native's
uniform vec4 uDepthSize;                 // xy: its size; z: 1 when it bounds the rays
layout(location = 0) out vec4 out0;
layout(location = 1) out vec4 out1;
// The farthest surface in the walk texel's pixels (native kin_cloud_limit; rows up): its distance, 1e30 for none.
float cloud_limit() {
    if (uDepthSize.z < 0.5) return 1e30;
    vec2 scale = uDepthSize.xy / uSize;
    ivec2 last = ivec2(uDepthSize.xy) - 1;
    ivec2 xy = ivec2(gl_FragCoord.xy);
    ivec2 p0 = min(ivec2(floor(vec2(xy) * scale)), last);
    ivec2 p1 = clamp(ivec2(floor(vec2(xy + 1) * scale)) - 1, p0, last);
    float farthest = 1.0;
    for (int i = 0; i < 4; ++i) {
        ivec2 p = ivec2((i & 1) == 0 ? p0.x : p1.x, (i & 2) == 0 ? p0.y : p1.y);
        farthest = min(farthest, texelFetch(uDepth, p, 0).r);
    }
    if (farthest <= 0.0) return 1e30;
    vec2 ndc = (0.5 * vec2(p0 + p1) + 0.5) * (2.0 / uDepthSize.xy) - 1.0;
    float w = uUnprojectZW.z * farthest + uUnprojectZW.w;
    vec3 seen = vec3(uUnprojectXY.x * ndc.x + uUnprojectXY.z, uUnprojectXY.y * ndc.y + uUnprojectXY.w,
                     uUnprojectZW.x * farthest + uUnprojectZW.y) / w;
    return length(seen);
}
void main() {
    vec2 ndc = gl_FragCoord.xy / uSize * 2.0 - 1.0;
    vec4 far_point = uToWorld * vec4(ndc, 0.0, 1.0);
    vec3 eye = uEye.xyz;
    vec3 dir = normalize(far_point.xyz - eye * far_point.w);
    vec2 span = kin_cloud_span(uClouds, eye, dir);
    span.y = min(span.y, cloud_limit());
    if (span.x >= span.y) {
        out0 = vec4(0.0, 0.0, 0.0, 1.0);
        out1 = vec4(0.0);
        return;
    }
    vec2 noise_at = floor(gl_FragCoord.xy) + 5.588238 * mod(uEye.w, 64.0);
    float offset = fract(52.9829189 * fract(dot(noise_at, vec2(0.06711056, 0.00583715))));
    float dist;
    out0 = kin_cloud_march(uClouds, uSky, uFog, uShape, uDetail, uWeather, eye, dir, span, offset, dist);
    out1 = vec4(dist, 0.0, 0.0, 1.0);
}
