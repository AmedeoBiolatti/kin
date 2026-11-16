#version 450 core
// The clouds in the sky's environment (Scene3D::clouds with Lights3D::sky_lights_scene): drawn over sky_env.frag's
// panorama (premultiplied, "over") before it is filtered into the scene's ambient light and reflections, so that an
// overcast greys and dims them. Each texel's direction as sky_env.frag's, from the camera; the march is the view's
// (kin_cloud_march), without the fog (the panorama has none) and from the middle of each step.
layout(set = 2, binding = 0) uniform sampler3D uShape;
layout(set = 2, binding = 1) uniform sampler3D uDetail;
layout(set = 2, binding = 2) uniform sampler2D uWeather; // the coverage map: r coverage, g kind (Clouds3D::weather_map or the default)

#include "kin_clouds.inc.glsl"

layout(std140, set = 3, binding = 0) uniform Params {
    vec4 eye;    // xyz: the camera
    vec4 target; // xy: the panorama's size
    KinClouds clouds;
    KinSky atmosphere;
    KinFog fog; // (not read: sun_light.w is 0)
} params;
layout(location = 0) out vec4 FragColor;

void main() {
    const vec2 uv = gl_FragCoord.xy / params.target.xy;
    const float phi = (uv.x - 0.5) * 2.0 * KIN_PI;
    const float theta = (0.5 - uv.y) * KIN_PI;
    const vec3 dir = vec3(cos(phi) * cos(theta), sin(theta), sin(phi) * cos(theta));
    const vec2 span = kin_cloud_span(params.clouds, params.eye.xyz, dir);
    if (span.x >= span.y) {
        discard;
    }
    float dist;
    const vec4 c = kin_cloud_march(params.clouds, params.atmosphere, params.fog, uShape, uDetail, uWeather,
                                   params.eye.xyz, dir, span, 0.5, dist);
    FragColor = vec4(min(c.rgb, vec3(60000.0)), 1.0 - c.a);
}
