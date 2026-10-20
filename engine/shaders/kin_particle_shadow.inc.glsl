// Opt-in shadow reception. Direct irradiance follows the existing isotropic
// particle model; visibility is sampled at each fragment's world position.
layout(location = 8) in vec3 vWorld;
layout(location = 9) flat in vec3 vCentre;
layout(set = 2, binding = 2) uniform sampler2DArrayShadow uShadow;
layout(set = 2, binding = 3) uniform sampler2DArrayShadow uLocalShadow;
layout(set = 2, binding = 4) uniform sampler2DArrayShadow uMoonShadow;
#define KIN_FRAME_BINDING 1
#include "kin_frame_uniforms.inc.glsl"
#define KIN_LIGHTS_BINDING 2
#include "kin_local_lights.inc.glsl"
#define KIN_LIGHTS
// A volume has no receiving surface normal. Offset toward the light instead
// of inventing a normal from the billboard orientation.
struct ParticleReceiver { vec4 params; };
const ParticleReceiver material = ParticleReceiver(vec4(0.0, 0.0, 1.0, 0.0));
vec3 shadow_offset_normal(vec3 n) { return n; }
#include "kin_shadows.inc.glsl"

vec3 particle_shadow_tint() {
    const float lit = frag.disc.w;
    const float sun = mix(1.0, sunlight(frame.sun_direction.xyz), locals.counts.w);
    const float moon = moonlight(frame.fill_direction.xyz);
    // frag.tint already includes unshadowed ambient and directional light.
    vec3 tint = frag.tint.rgb + 0.5 * lit *
        (frame.sun_color.rgb * (sun - 1.0) + frame.fill_color.rgb * (moon - 1.0));
    for (int i = 0; i < int(frame.flags.w); ++i) {
        const vec4 at = frame.lights[2 * i];
        const vec4 colour = frame.lights[2 * i + 1];
        const vec3 to = at.xyz - vCentre;
        const float d2 = dot(to, to), r2 = d2 / (at.w * at.w);
        const float window = clamp(1.0 - r2 * r2, 0.0, 1.0);
        if (window <= 0.0) continue;
        const vec3 shadow_to = at.xyz - vWorld;
        const float distance = length(shadow_to);
        const vec3 toward = shadow_to * inversesqrt(max(dot(shadow_to, shadow_to), 1e-8));
        const float visibility = point_light_shadow(i, toward, toward, distance);
        tint += 0.5 * lit * colour.rgb * (window * window * visibility / max(d2, colour.w));
    }
    for (int i = 0; i < int(locals.counts.x); ++i) {
        const vec4 at = locals.spots[4 * i];
        const vec4 colour = locals.spots[4 * i + 1];
        const vec4 axis = locals.spots[4 * i + 2];
        const vec4 more = locals.spots[4 * i + 3];
        const vec3 to = at.xyz - vCentre;
        const float d2 = dot(to, to), r2 = d2 / (at.w * at.w);
        const float window = clamp(1.0 - r2 * r2, 0.0, 1.0);
        const vec3 toward = to * inversesqrt(max(d2, 1e-8));
        const float cone = clamp((dot(-toward, axis.xyz) - axis.w) * more.x, 0.0, 1.0);
        if (window <= 0.0 || cone <= 0.0) continue;
        const vec3 shadow_to = at.xyz - vWorld;
        const float distance = length(shadow_to);
        const vec3 shadow_toward = shadow_to * inversesqrt(max(dot(shadow_to, shadow_to), 1e-8));
        const float visibility = more.y < 0.0 ? 1.0 : local_shadow(int(more.y), shadow_toward, shadow_toward, distance);
        tint += 0.5 * lit * colour.rgb * (window * window * cone * cone * visibility / max(d2, colour.w));
    }
    return max(tint, vec3(0.0));
}
