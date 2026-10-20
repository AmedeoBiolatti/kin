// kin's fog (kin/render3d/sky3d.hpp, Fog3D): distance and height fog with the sky's colour, shared by the main pass's
// meshes (kin_material.inc.glsl, under KIN_FOG), the sky pass (the height fog's share, to infinity) and, to come, a
// particle pass. The same arithmetic runs on the CPU in sky3d.cpp (fog_optical_depth, fog_opacity, fog_color).
#include "kin_sky.inc.glsl"

struct KinFog {
    vec4 colour;  // rgb: the fixed colour (linear), w: 1 to take it from the sky
    vec4 density; // x: per unit of distance, y: the height fog's at its base, z: its fall-off per unit of height, w: the base height
    vec4 limits;  // x: the start distance, y: the greatest opacity, z: the sun's in-scattering, w: its anisotropy
    vec4 view;    // x: 1 for an orthographic camera
};

// The optical depth from `eye` along the unit `dir` for `dist` (`infinite`: to the end of the ray, the distance term
// left out). The height fog's density is d * e^(-k (y - base)): its integral along the ray is closed.
float kin_fog_optical_depth(KinFog f, vec3 eye, vec3 dir, float dist, bool infinite) {
    const float start = f.limits.x;
    const float len = infinite ? 1e30 : max(dist - start, 0.0);
    float tau = infinite ? 0.0 : f.density.x * len;
    const float d = f.density.y;
    if (d > 0.0 && len > 0.0) {
        const float k = f.density.z;
        // From the start on, to infinity too: the sky's depth is the limit of the geometry's as `dist` grows.
        const float y0 = eye.y + dir.y * start - f.density.w;
        const float kd = k * dir.y;
        const float at_start = exp(clamp(-k * y0, -80.0, 80.0));
        float shape;
        if (infinite) {
            shape = kd > 1e-6 ? 1.0 / kd : 1e8;
        } else {
            const float x = kd * len;
            shape = abs(x) < 1e-3 ? len * (1.0 - 0.5 * x) : (1.0 - exp(clamp(-x, -80.0, 80.0))) / kd;
        }
        tau += d * at_start * shape;
    }
    return tau;
}

float kin_fog_opacity(KinFog f, float tau) {
    return min(1.0 - exp(-tau), f.limits.y);
}

// `c` (the fog's fixed colour or the sky's along `dir`) brightened toward the sun by its in-scattering (the
// Henyey-Greenstein lobe, scaled so that an even one is 1).
vec3 kin_fog_sun(KinFog f, vec3 c, vec3 dir, vec3 sun_dir, vec3 sun_light) {
    if (f.limits.z > 0.0) {
        const float g = f.limits.w;
        const float mu = dot(dir, sun_dir);
        c += sun_light * (f.limits.z * (1.0 - g * g) / pow(max(1.0 + g * g - 2.0 * g * mu, 1e-4), 1.5));
    }
    return c;
}

// The fog's colour along `dir`: the fixed one or the sky's, and the sun's in-scattering. The sky's is integrated here (the
// particles' fog, made per vertex); the meshes' read it from the fog table (kin_material.inc.glsl, kin_fog_sky) and the sky
// pass has it already.
vec3 kin_fog_colour(KinFog f, KinSky sky, vec3 dir, vec3 sun_dir, vec3 sun_light) {
    return kin_fog_sun(f, f.colour.w > 0.5 ? kin_sky_scatter(sky, dir) : f.colour.rgb, dir, sun_dir, sun_light);
}

// The fog table (sky3d.hpp, sky_model::FogTable): the sky's light at KIN_FOG_ROWS elevations, made on the CPU for each sky,
// four vec4s a row (the sun's Rayleigh and Mie terms, the moon's). The row of a ray at sin(elevation) `s` (0 to
// KIN_FOG_ROWS - 1, evenly in the log of kin_air_mass's rounded sin: the rows crowd near the horizon), FogTable::coordinate.
const int KIN_FOG_ROWS = 48; // = sky_model::FogTable::rows (sky3d.hpp): change both, the C++ static_assert sees only its side
float kin_fog_row(float s) {
    const float h = max(s, 0.0);
    const float c = min(sqrt(h * h + 0.000625), 1.0);
    return clamp(log(c * 40.0) * 0.2710850307, 0.0, 1.0) * float(KIN_FOG_ROWS - 1);
}

// Instantiate beside the pass's uniform block. Reading its rows directly avoids copying a 3 KiB array into a
// function parameter at every fragment or particle corner.
#define KIN_FOG_TABLE_READER(FUNCTION, SKY, TABLE) \
vec3 FUNCTION(vec3 dir) { \
    const vec3 v = normalize(vec3(dir.x, max(dir.y, 0.0), dir.z) + vec3(1e-6, 0.0, 0.0)); \
    const float x = kin_fog_row(v.y); \
    const int i = min(int(x), KIN_FOG_ROWS - 2); \
    const float w = x - float(i); \
    const int k = i * 4; \
    const float mu = dot(v, (SKY).sun.xyz); \
    vec3 light = kin_rayleigh_phase(mu) * mix(TABLE[k].rgb, TABLE[k + 4].rgb, w) + \
                 kin_mie_phase(mu, (SKY).shape.y) * mix(TABLE[k + 1].rgb, TABLE[k + 5].rgb, w); \
    if ((SKY).shape.w > 0.0) { \
        const float mu_moon = dot(v, (SKY).moon.xyz); \
        light += kin_rayleigh_phase(mu_moon) * mix(TABLE[k + 2].rgb, TABLE[k + 6].rgb, w) + \
                 kin_mie_phase(mu_moon, (SKY).shape.y) * mix(TABLE[k + 3].rgb, TABLE[k + 7].rgb, w); \
    } \
    return light + (SKY).night.rgb; \
}
