// kin's procedural sky (kin/render3d/sky3d.hpp, P3a): a single-scattering atmosphere, Rayleigh and Mie, seen over a flat
// ground, shared by the sky pass (sky.frag), the environment it makes for image-based light (sky_env.frag) and the fog
// that takes its colour from the sky (kin_fog.inc.glsl). The same arithmetic runs on the CPU in sky3d.cpp (sky_radiance):
// change the constants and the steps together, the tests compare the two.
//
// The light is kin's: a white surface facing a sun of light E shows E. The scattering integral is over the height of
// the ray's points, in the variable w = 1 - e^(-h/H_R) (Rayleigh's own optical depth below h): there, Rayleigh's
// weight is flat and Mie's (1-w)^(H_R/H_M - 1) falls. Eight midpoints of u (w = top * u^2: the ground's air, where a
// low ray's light comes from, gets the finer steps).
const float KIN_PI = 3.14159265358979;
const vec3 KIN_TAU_R = vec3(0.0464, 0.1080, 0.2650); // Rayleigh's optical depth at the zenith: red, green, blue
// The ozone layer's absorption (a tent of 10 km of effective thickness at 25 km, over nearly all of the air): it takes
// the green from the sun's light on its way down, and keeps the zenith blue at twilight, where the Rayleigh path alone
// would take the blue too.
const vec3 KIN_TAU_OZONE = vec3(0.0065, 0.0188, 0.00085);
const float KIN_MIE_PER_TURBIDITY = 0.0105;
const float KIN_MIE_EXT = 1.1;
const float KIN_H_RATIO = 6.66666667; // Rayleigh's scale height (8 km) over Mie's (1.2 km)
const float KIN_SKY_TOP = 0.9994;     // the share of the density below the atmosphere's top (60 km)
const float KIN_SKY_TWILIGHT = 0.07;
const float KIN_GROUND_BAND = 0.15;
const int KIN_SKY_STEPS = 8;

struct KinSky {
    vec4 sun;    // xyz: toward the sun (length 1), w: its light above the atmosphere
    vec4 shape;  // x: Mie's optical depth at the zenith, y: its anisotropy g, z: brightness (the sky's intensity), w: the moon's light
    vec4 ground; // rgb: the ground's colour (albedo)
    vec4 moon;   // xyz: toward the moon (length 1)
    vec4 night;  // rgb: the night sky's own light
};

// How many zenith paths through the atmosphere a ray at this sin(elevation) meets (Kasten and Young, rounded at the horizon).
float kin_air_mass(float s) {
    // sqrt(s^2 + e^2), e = 0.025 (1.4 degrees): the horizon's air mass (38) is not met at the horizon itself but rounded
    // off over its last degree, so that the sky's colour has no kink where the ground begins (the slope is 0 there).
    float c = min(sqrt(max(s, 0.0) * max(s, 0.0) + 0.000625), 1.0);
    float zenith_degrees = degrees(acos(c));
    return 1.0 / (c + 0.50572 * pow(96.07995 - zenith_degrees, -1.6364));
}

float kin_rayleigh_phase(float mu) {
    return 3.0 / (16.0 * KIN_PI) * (1.0 + mu * mu);
}

float kin_mie_phase(float mu, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * KIN_PI * pow(max(1.0 + g2 - 2.0 * g * mu, 1e-4), 1.5));
}

// The sun's light through the atmosphere to the ground, per colour, for a sun at sin(elevation) `s`.
vec3 kin_sun_transmittance(KinSky a, float s) {
    float m = kin_air_mass(s);
    return exp(-(KIN_TAU_R + vec3(KIN_MIE_EXT * a.shape.x) + KIN_TAU_OZONE) * m);
}

// One light's scattering along the unit ray `v` (above the horizon): `e` its light above the atmosphere, `l` toward it.
vec3 kin_scatter(KinSky a, vec3 v, vec3 l, float e) {
    float tau_m = a.shape.x;
    float mv = kin_air_mass(v.y);
    float ms = kin_air_mass(l.y);
    float mu = dot(v, l);
    float pr = kin_rayleigh_phase(mu);
    float pm = kin_mie_phase(mu, a.shape.y);
    vec3 sum = vec3(0.0);
    for (int i = 0; i < KIN_SKY_STEPS; ++i) {
        float u = (float(i) + 0.5) / float(KIN_SKY_STEPS);
        float dw = 2.0 * KIN_SKY_TOP * u / float(KIN_SKY_STEPS);   // dw = top * d(u^2)
        float q = 1.0 - KIN_SKY_TOP * u * u;                        // e^(-h/H_R)
        float qm = pow(q, KIN_H_RATIO);                             // e^(-h/H_M)
        vec3 along = mv * (KIN_TAU_R * (1.0 - q) + vec3(KIN_MIE_EXT * tau_m * (1.0 - qm)));
        vec3 toward_light = ms * (KIN_TAU_R * q + vec3(KIN_MIE_EXT * tau_m * qm) + KIN_TAU_OZONE);
        vec3 weight = KIN_TAU_R * pr + vec3(tau_m * KIN_H_RATIO * pm * qm / q);
        sum += weight * exp(-(along + toward_light)) * dw;
    }
    return sum * (mv * KIN_PI * e);
}

// The light of the atmosphere along `v` (any direction: below the horizon it is the horizon's): the sun's and the moon's
// scattering and the night's own, times the sky's brightness.
vec3 kin_sky_scatter(KinSky a, vec3 v) {
    v = normalize(vec3(v.x, max(v.y, 0.0), v.z) + vec3(1e-6, 0.0, 0.0));
    float twilight_sun = a.sun.w * exp(-max(-a.sun.y, 0.0) / KIN_SKY_TWILIGHT);
    vec3 light = kin_scatter(a, v, a.sun.xyz, twilight_sun);
    if (a.shape.w > 0.0) {
        light += kin_scatter(a, v, a.moon.xyz, a.shape.w * exp(-max(-a.moon.y, 0.0) / KIN_SKY_TWILIGHT));
    }
    return light * (2.0 * a.shape.z) + a.night.rgb;
}

// The ground as it is lit: its albedo times the light a horizontal surface takes from the sun and the zenith's sky.
vec3 kin_sky_ground(KinSky a) {
    float cut = smoothstep(-0.01, 0.04, a.sun.y);
    vec3 sun = a.sun.w * kin_sun_transmittance(a, a.sun.y) * max(a.sun.y, 0.0) * cut;
    return a.ground.rgb * (sun + kin_sky_scatter(a, vec3(0.0, 1.0, 0.0)));
}

// What the sky shows along `v` given the atmosphere's light along it (kin_sky_scatter's), going over the horizon to the
// ground's light.
vec3 kin_sky_over_ground(KinSky a, vec3 v, vec3 sky) {
    if (v.y >= 0.0) {
        return sky;
    }
    return mix(sky, kin_sky_ground(a), smoothstep(0.0, KIN_GROUND_BAND, -v.y));
}

// What the sky shows along `v`: the atmosphere, going over the horizon to the ground's light.
vec3 kin_sky_color(KinSky a, vec3 v) {
    return kin_sky_over_ground(a, v, kin_sky_scatter(a, v));
}
