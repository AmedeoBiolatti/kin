#version 450 core
// The fog's sunlight through the sun's shadows (Fog3D::sun_shadows), added to the picture the main pass drew (whose fog,
// the sky's and the particles' left it out), once a pixel: the sun's in-scattering toward the pixel's ray, times the
// fog's opacity to what the pre-pass drew there (to the end of the height fog for the sky), times the share of that
// fog the sun reaches (fog_sun.comp). Added in the main pass, the share was read for every fragment drawn, the hidden
// ones too. A blended surface takes the light of the ray to what is behind it.
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

layout(set = 2, binding = 0) uniform sampler2D uDepth; // the pre-pass's, reverse Z: 0 is nothing
layout(set = 2, binding = 1) uniform sampler2D uShare; // fog_sun.comp's (x: the share, z: the ray's distance)

#include "kin_fog.inc.glsl"

layout(std140, set = 3, binding = 0) uniform Params {
    mat4 to_world;
    vec4 unproject_xy;
    vec4 unproject_zw;
    vec4 sizes; // xy: the share's size, zw: the depth's
    vec4 eye;
    vec4 forward;
    KinFog fog;
    mat4 shadow_matrix[4];
    vec4 cascade_end;
    vec4 cascade_bias;
    vec4 shadow;
    vec4 sun;       // xyz: toward the sun
    vec4 sun_light; // rgb: its light
} params;

#include "kin_depth.inc.glsl"

void main() {
    const ivec2 p = ivec2(gl_FragCoord.xy);
    const float depth = texelFetch(uDepth, p, 0).r;
    const bool sky = depth <= 0.0;
    const vec3 seen = view_position(p, sky ? 1e-7 : depth);
    const vec3 to = (params.to_world * vec4(seen, 0.0)).xyz;
    const float dist = length(to);
    const vec3 dir = to / max(dist, 1e-9);
    KinFog f = params.fog;
    float opacity = 0.0;
    if (sky) {
        // As the sky pass has it: the height fog's alone, to infinity.
        f.density.x = 0.0;
        if (f.density.y > 0.0) {
            opacity = kin_fog_opacity(f, kin_fog_optical_depth(f, params.eye.xyz, dir, 0.0, true));
        }
    } else {
        opacity = kin_fog_opacity(f, kin_fog_optical_depth(f, params.eye.xyz, dir, dist, false));
    }
    const vec3 sun = kin_fog_sun_light(f, dir, params.sun.xyz, params.sun_light.rgb);
    FragColor = vec4(sun * (opacity * kin_fog_share(uShare, gl_FragCoord.xy, sky ? 0.0 : dist)), 0.0);
}
