#version 450 core
// The sky as an environment (Lights3D::sky_lights_scene): the equirectangular picture of the atmosphere and the
// ground, without the sun disc, the moon or the stars (the sun is a light of its own), that the environment filter
// turns into the scene's ambient light and reflections. Orientation as render3d/environment.hpp: the centre is +x, the
// top +y, going right turns toward +z.
#include "kin_sky.inc.glsl"

layout(set = 3, binding = 0) uniform SkyEnv {
    vec4 target; // xy: the picture's size
    KinSky atmosphere;
} env;
layout(location = 0) out vec4 FragColor;

void main() {
    const vec2 uv = gl_FragCoord.xy / env.target.xy;
    const float phi = (uv.x - 0.5) * 2.0 * KIN_PI;
    const float theta = (0.5 - uv.y) * KIN_PI;
    const vec3 d = vec3(cos(phi) * cos(theta), sin(theta), sin(phi) * cos(theta));
    FragColor = vec4(min(kin_sky_color(env.atmosphere, d), vec3(60000.0)), 1.0);
}
