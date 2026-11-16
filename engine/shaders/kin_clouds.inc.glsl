// kin's volumetric clouds (Scene3D::clouds, kin/render3d/sky3d.hpp's Clouds3D): the layer, the clouds' density in it,
// the light that reaches them and the air in front of them. Read by clouds_trace.comp. kin_cloud_span is clouds3d.cpp's
// layer_span (the tests compare them): change them together.
#include "kin_fog.inc.glsl"

struct KinClouds {
    vec4 layer;          // x: the base's height, y: the top's, z: 1 / (2 x the planet's radius), w: the farthest distance
    vec4 scales;         // x: 1 / the shape's tile, y: 1 / the detail's, z: 1 / the coverage map's, w: erosion
    vec4 cover;          // x: coverage, y: kind, z: extinction of a full cloud, w: the most samples along a ray
    vec4 shape_offset;   // xyz: the wind's drift of the shapes (and the coverage map), w: samples toward the light
    vec4 detail_offset;  // xyz: the detail's drift, w: times the ambient light
    vec4 phase;          // x: the forward lobe's g, y: the backward's, z: the backward's share, w: each octave's share
    vec4 light;          // xyz: toward the light the clouds are lit by, w: 1 when it is the sun through the atmosphere
    vec4 light_color;    // rgb: its light (when not the atmosphere's), w: the walk toward it (units)
    vec4 ambient_top;    // rgb: the light from above (the sky's)
    vec4 ambient_bottom; // rgb: the light from below (the ground's), w: 1 to put the atmosphere's air in front
    vec4 sun_light;      // rgb: the sun's light as the fog takes it (Lights3D::sun), w: 1 with the fog on
};

// Where the ray from `eye` along the unit `dir` is first in the layer (x < y), as kin::cloud_model::layer_span.
vec2 kin_cloud_span(KinClouds c, vec3 eye, vec3 dir) {
    vec2 none = vec2(1.0, -1.0);
    float dy = dir.y;
    float a = max((1.0 - dy * dy) * c.layer.z, 1e-14);
    float y0 = eye.y;
    if (y0 < 0.0) {
        return none;
    }
    float bottom = min(c.layer.x, c.layer.y), top = max(c.layer.x, c.layer.y);
    // Under the layer, nothing below the level: kin's sky has the ground there (a flat one), though the curved shell
    // would show clouds a hair under it past the planet's horizon.
    if (y0 < bottom && dy <= 0.0) {
        return none;
    }
    // The roots of y0 + t dy + a t^2 = level (the height over the curved ground), stable, in order; x > y: none.
    #define KIN_CLOUD_ROOTS(level, out_roots) { \
        float cc = y0 - (level); \
        float disc = dy * dy - 4.0 * a * cc; \
        if (disc < 0.0) { out_roots = none; } else { \
            float q = -0.5 * (dy + (dy < 0.0 ? -sqrt(disc) : sqrt(disc))); \
            float r1 = q / a, r2 = q != 0.0 ? cc / q : r1; \
            out_roots = vec2(min(r1, r2), max(r1, r2)); } }
    vec2 s;
    KIN_CLOUD_ROOTS(top, s)
    if (s.x > s.y) {
        return none;
    }
    float end = s.y;
    vec2 g;
    KIN_CLOUD_ROOTS(0.0, g)
    if (g.x <= g.y && g.x > 0.0) {
        end = min(end, g.x);
    }
    end = min(end, max(c.layer.w, 0.0));
    float start = max(s.x, 0.0);
    vec2 r;
    KIN_CLOUD_ROOTS(bottom, r)
    if (r.x <= r.y && r.y > start) {
        if (r.x > start) {
            end = min(end, r.x);
        } else {
            start = r.y;
        }
    }
    #undef KIN_CLOUD_ROOTS
    return start < end ? vec2(start, end) : none;
}

float kin_cloud_remap(float v, float lo, float hi) {
    return clamp((v - lo) / max(hi - lo, 1e-5), 0.0, 1.0);
}

// The cloud's density at the world point `p` (x, z) at `h` over the curved ground (its share of a full cloud, 0 to 1):
// the coverage map's, shaped by the kind's height profile and the shape noise, worn at its edges by the detail
// (`detail` false: not, for the walk toward the light). Schneider's (Nubis), as Unreal's and Unity's.
float kin_cloud_density(KinClouds c, sampler3D shape, sampler3D detail, sampler2D weather, vec3 p, float h, bool worn) {
    float hf = (h - c.layer.x) / max(c.layer.y - c.layer.x, 1.0);
    if (hf <= 0.0 || hf >= 1.0) {
        return 0.0;
    }
    vec4 w = textureLod(weather, (p.xz + c.shape_offset.xz) * c.scales.z, 0.0);
    float coverage = clamp(w.r + 2.0 * c.cover.x - 1.0, 0.0, 1.0);
    if (coverage <= 0.0) {
        return 0.0;
    }
    // The kind: how high in the layer the cloud reaches (a flat one a third of it, a heap all of it), rounded at the
    // base and the top.
    float kind = clamp(c.cover.y + (w.g - 0.5) * 0.8, 0.0, 1.0);
    float reach = mix(0.3, 1.0, kind);
    float profile = smoothstep(0.0, 0.08, hf) * (1.0 - smoothstep(reach * 0.55, reach, hf));
    if (profile <= 0.0) {
        return 0.0;
    }
    vec3 q = vec3(p.x, h, p.z) + c.shape_offset.xyz;
    vec4 s = textureLod(shape, q * c.scales.x, 0.0);
    float low = s.g * 0.625 + s.b * 0.25 + s.a * 0.125;
    float base = kin_cloud_remap(s.r, low - 1.0, 1.0) * profile;
    base = kin_cloud_remap(base, 1.0 - coverage, 1.0) * coverage;
    if (base <= 0.0 || !worn) {
        return base;
    }
    vec3 d = textureLod(detail, (vec3(p.x, h, p.z) + c.detail_offset.xyz) * c.scales.y, 0.0).rgb;
    float fine = d.r * 0.625 + d.g * 0.25 + d.b * 0.125;
    // Wispy at the base (the detail inverted there), billowy higher up.
    float wear = mix(1.0 - fine, fine, clamp(hf * 8.0, 0.0, 1.0)) * c.scales.w;
    return kin_cloud_remap(base, wear, 1.0);
}

// The light of the sun (the atmosphere's: KinSky) at height `h`: what is left of it after the air above, from a
// horizon lowered by the height (a cloud 3 km up keeps the sun for a degree and a half after sunset).
vec3 kin_cloud_sun(KinClouds c, KinSky a, float h) {
    float s = c.light.y + sqrt(max(2.0 * h, 0.0) * c.layer.z);
    if (s <= -0.01) {
        return vec3(0.0);
    }
    float m = kin_air_mass(s);
    float qr = exp(-max(h, 0.0) / 8000.0), qm = exp(-max(h, 0.0) / 1200.0);
    vec3 tau = KIN_TAU_R * qr + vec3(KIN_MIE_EXT * a.shape.x * qm) + KIN_TAU_OZONE;
    return a.sun.w * exp(-tau * m) * smoothstep(-0.01, 0.01, s);
}

// One light's scattering along `v` in the air below the share `wc` of the atmosphere's density (kin_scatter's integral
// stopped there: the air in front of a cloud at that height).
vec3 kin_cloud_scatter_below(KinSky a, vec3 v, vec3 l, float e, float wc) {
    float tau_m = a.shape.x;
    float mv = kin_air_mass(v.y);
    float ms = kin_air_mass(l.y);
    float mu = dot(v, l);
    float pr = kin_rayleigh_phase(mu);
    float pm = kin_mie_phase(mu, a.shape.y);
    vec3 sum = vec3(0.0);
    for (int i = 0; i < KIN_SKY_STEPS; ++i) {
        float u = (float(i) + 0.5) / float(KIN_SKY_STEPS);
        float dw = 2.0 * wc * u / float(KIN_SKY_STEPS);
        float q = 1.0 - wc * u * u;
        float qm = pow(q, KIN_H_RATIO);
        vec3 along = mv * (KIN_TAU_R * (1.0 - q) + vec3(KIN_MIE_EXT * tau_m * (1.0 - qm)));
        vec3 toward_light = ms * (KIN_TAU_R * q + vec3(KIN_MIE_EXT * tau_m * qm) + KIN_TAU_OZONE);
        vec3 weight = KIN_TAU_R * pr + vec3(tau_m * KIN_H_RATIO * pm * qm / q);
        sum += weight * exp(-(along + toward_light)) * dw;
    }
    return sum * (mv * KIN_PI * e);
}

// The atmosphere's light along `v` in front of a cloud at height `h` (kin_sky_scatter's share there), and in `kept` the
// share of the cloud's light that gets through that air.
vec3 kin_cloud_air(KinSky a, vec3 v, float h, out vec3 kept) {
    v = normalize(vec3(v.x, max(v.y, 0.0), v.z) + vec3(1e-6, 0.0, 0.0));
    float wc = min(1.0 - exp(-max(h, 0.0) / 8000.0), KIN_SKY_TOP);
    float mv = kin_air_mass(v.y);
    kept = exp(-mv * (KIN_TAU_R * wc + vec3(KIN_MIE_EXT * a.shape.x * (1.0 - pow(1.0 - wc, KIN_H_RATIO)))));
    float twilight_sun = a.sun.w * exp(-max(-a.sun.y, 0.0) / KIN_SKY_TWILIGHT);
    vec3 light = kin_cloud_scatter_below(a, v, a.sun.xyz, twilight_sun, wc);
    if (a.shape.w > 0.0) {
        light += kin_cloud_scatter_below(a, v, a.moon.xyz, a.shape.w * exp(-max(-a.moon.y, 0.0) / KIN_SKY_TWILIGHT), wc);
    }
    return light * (2.0 * a.shape.z) + a.night.rgb * (wc / KIN_SKY_TOP);
}

float kin_cloud_hg(float mu, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * KIN_PI * pow(max(1.0 + g2 - 2.0 * g * mu, 1e-4), 1.5));
}

// The height of `p` over the curved ground, as seen from `eye` (the shell's drop with the distance across).
float kin_cloud_height(KinClouds c, vec3 eye, vec3 p) {
    vec2 across = p.xz - eye.xz;
    return p.y + dot(across, across) * c.layer.z;
}

// The clouds along the unit ray from `eye` along `dir` within `span` (kin_cloud_span's), samples started at `offset`
// (0 to 1) of a step: rgb the light they add over what is behind (with the atmosphere's air in front when
// ambient_bottom.w, and the fog as the sky behind takes it when sun_light.w), a the share of what is behind still
// seen; `dist` their mean distance (weighted by the light they take out). The light: each sample with cloud walks
// toward the light source for its optical depth, two lobes and octaves of the light scattered many times (Wrenninge's,
// as Hillaire's and Unreal's), the diffuse light through a thick cloud, the sky's and the ground's ambient light;
// Hillaire's energy-conserving step integrates each sample's slab.
vec4 kin_cloud_march(KinClouds c, KinSky atmosphere, KinFog fog, sampler3D shape, sampler3D detail, sampler2D weather,
                     vec3 eye, vec3 dir, vec2 span, float offset, out float dist) {
    // The samples: as many as the layer's thickness cut in 24 (a ray straight up), more for a longer path, at most
    // cover.w; started at `offset` (0 to 1) of the first.
    float thickness = max(c.layer.y - c.layer.x, 1.0);
    float path = span.y - span.x;
    int most = int(c.cover.w);
    int n = clamp(int(ceil(path / (thickness / 24.0))), min(16, most), most);
    float step_length = path / float(n);

    float mu = dot(dir, c.light.xyz);
    int light_steps = int(c.shape_offset.w);
    float light_reach = c.light_color.w;
    float fade_from = c.layer.w * 0.8;
    float through = 1.0;
    vec3 light = vec3(0.0);
    float weighted = 0.0;
    for (int i = 0; i < n; ++i) {
        float t = span.x + (float(i) + offset) * step_length;
        vec3 p = eye + dir * t;
        float h = kin_cloud_height(c, eye, p);
        float density = kin_cloud_density(c, shape, detail, weather, p, h, true);
        density *= 1.0 - smoothstep(fade_from, c.layer.w, t);
        if (density <= 0.0) {
            continue;
        }
        float extinction = density * c.cover.z;
        // The optical depth toward the light: samples further apart the further they are (the near cloud matters
        // most), without the detail.
        float depth = 0.0;
        for (int k = 0; k < light_steps; ++k) {
            float a = float(k) / float(light_steps), b = float(k + 1) / float(light_steps);
            float s = light_reach * 0.5 * (a * a + b * b);
            vec3 q = p + c.light.xyz * s;
            depth += kin_cloud_density(c, shape, detail, weather, q, kin_cloud_height(c, eye, q), false) *
                     (light_reach * (b * b - a * a));
        }
        depth *= c.cover.z;
        vec3 source = c.light.w > 0.5 ? kin_cloud_sun(c, atmosphere, h) : c.light_color.rgb;
        // The lobes, then each octave of the light scattered again: fainter, less deep in the cloud, wider.
        float scattered = 0.0;
        float share = 1.0, deep = 1.0, narrow = 1.0;
        for (int o = 0; o < 3; ++o) {
            float lobe = mix(kin_cloud_hg(mu, c.phase.x * narrow), kin_cloud_hg(mu, c.phase.y * narrow), c.phase.z);
            scattered += share * lobe * exp(-depth * deep);
            share *= c.phase.w;
            deep *= c.phase.w;
            narrow *= c.phase.w;
        }
        // kin's light: a white surface facing a light E shows E (the radiance pi E's scattering over the sphere).
        float hf = clamp((h - c.layer.x) / thickness, 0.0, 1.0);
        // The ambient light: the sky's from above, half of it at the base (the cloud above hides the rest: Unreal's
        // bottom occlusion), and the ground's from below (its hemisphere's half of the sphere), fading going up.
        vec3 ambient =
            (c.ambient_top.rgb * mix(0.5, 1.0, hf) + c.ambient_bottom.rgb * (0.5 * (1.0 - hf))) * c.detail_offset.w;
        // Deep in a thick cloud the octaves die out, but light diffused all the way through does not: the two-stream
        // diffuse transmission 1 / (1 + 3/4 (1 - g) tau) of the light falling on the layer from above (the sun's on a
        // level surface and the sky's), scattered evenly. The lit side keeps the lobes (the larger of the two).
        float g = mix(c.phase.x, c.phase.y, c.phase.z);
        float diffused = min(2.0 * c.phase.w, 1.0) / (1.0 + 0.75 * (1.0 - g) * depth);
        vec3 through_cloud = (source * max(c.light.y, 0.0) + c.ambient_top.rgb * c.detail_offset.w) * diffused;
        vec3 radiance = max(source * (KIN_PI * scattered), through_cloud) + ambient; // per unit of scattering
        float kept = exp(-extinction * step_length);
        // Hillaire's step: the slab's light, S (1 - e^(-s L)) / s with S = s radiance.
        light += through * radiance * (1.0 - kept);
        weighted += t * through * (1.0 - kept);
        through *= kept;
        if (through < 0.003) {
            through = 0.0;
            break;
        }
    }
    float covered = 1.0 - through;
    float dist_mean = covered > 1e-4 ? weighted / covered : 0.5 * (span.x + span.y);
    // The air in front (the procedural sky's), then the fog as the sky behind takes it (its height fog to infinity).
    vec3 at = eye + dir * dist_mean;
    if (c.ambient_bottom.w > 0.5) {
        vec3 kept;
        vec3 air = kin_cloud_air(atmosphere, dir, kin_cloud_height(c, eye, at), kept);
        light = light * kept + covered * air;
    }
    if (c.sun_light.w > 0.5) {
        float veil = kin_fog_opacity(fog, kin_fog_optical_depth(fog, eye, dir, 0.0, true));
        vec3 colour = kin_fog_colour(fog, atmosphere, dir, atmosphere.sun.xyz, c.sun_light.rgb);
        light = light * (1.0 - veil) + covered * colour * veil;
    }
    dist = dist_mean;
    return vec4(light, through);
}
