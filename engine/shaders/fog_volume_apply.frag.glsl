#version 450 core
// The fog's light from the volume (Fog3D::volumetric), added to the picture the main pass drew, once a pixel: the sum
// fog_volume_integrate.comp made along the pixel's ray, read at the view depth of what the pre-pass drew there (the
// whole volume's for the sky and beyond its depth). The main pass's fog has already dimmed what lies behind the air by
// the same density; this is the light the air adds. When the volume holds the sun's in-scattering (the main pass then
// leaves it out), the share of the ray's fog past the volume (its opacity to the surface, or the sky's, less the
// volume's) takes the sun's unshadowed, as the main pass would have. A blended surface takes the light of the ray to
// what is behind it.
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

layout(set = 2, binding = 0) uniform sampler2D uDepth;  // the pre-pass's, reverse Z: 0 is nothing
layout(set = 2, binding = 1) uniform sampler3D uSum;    // fog_volume_integrate.comp's

#include "kin_fog.inc.glsl"

layout(std140, set = 3, binding = 0) uniform Params {
    mat4 to_world;
    vec4 unproject_xy;
    vec4 unproject_zw;
    vec4 sizes;   // xy: the view's size, zw: the depth's
    vec4 forward; // xyz: the way the camera looks; w: the volume's depth
    vec4 cells;   // xyz: the volume's cells; w: 1 when the volume holds the sun's in-scattering
    vec4 eye;     // xyz: the camera
    vec4 sun;     // xyz: toward the sun
    vec4 sun_light;
    KinFog fog;
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
    const float w = sky ? 1.0 : min(sqrt(max(dot(to, params.forward.xyz), 0.0) / params.forward.w), 1.0);
    // Each cell holds the sum to its slice's far edge, (z + 1) / slices in w; its middle is read at (z + 0.5) / slices.
    const vec2 uv = gl_FragCoord.xy / params.sizes.xy;
    const float z = w - 0.5 / params.cells.z;
    // Nearer than the first slice's far edge, a share of the first's.
    const float near = clamp(w * params.cells.z, 0.0, 1.0);
    const vec4 sum = textureLod(uSum, vec3(uv, max(z, 0.5 / params.cells.z)), 0.0);
    vec3 light = sum.rgb * near;
    if (params.cells.w > 0.5) {
        // The ray's fog past the volume, lit by the sun as the main pass's (the sky's: the height fog's alone).
        KinFog f = params.fog;
        float total;
        if (sky) {
            f.density.x = 0.0;
            total = f.density.y > 0.0 ? kin_fog_opacity(f, kin_fog_optical_depth(f, params.eye.xyz, dir, 0.0, true))
                                      : 0.0;
        } else {
            total = kin_fog_opacity(f, kin_fog_optical_depth(f, params.eye.xyz, dir, dist, false));
        }
        const float inside = 1.0 - mix(1.0, sum.a, near);
        const float past = max(total - inside, 0.0);
        light += kin_fog_sun_light(params.fog, dir, params.sun.xyz, params.sun_light.rgb) * past;
    }
    FragColor = vec4(light, 0.0);
}
