// Shared stock metallic-roughness BRDF for native and WebGL2 materials.
// The including shader defines PI. No backend-specific resources occur here.

// GGX's share of the microfacets facing the half vector, alpha = roughness^2.
float distribution(float n_h, float alpha) {
    float a2 = alpha * alpha;
    float d = n_h * n_h * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

// Smith's visibility, height-correlated: the geometry term over 4 n.l n.v.
float visibility(float n_l, float n_v, float alpha) {
    float a2 = alpha * alpha;
    float v = n_l * sqrt(n_v * n_v * (1.0 - a2) + a2);
    float l = n_v * sqrt(n_l * n_l * (1.0 - a2) + a2);
    return 0.5 / max(v + l, 1e-5);
}

vec3 fresnel(vec3 f0, float v_h) {
    float k = pow(1.0 - v_h, 5.0);
    return f0 + (vec3(1.0) - f0) * k;
}

// The specular albedo of a surface lit from everywhere, as the scale and the
// offset of its F0 (the split-sum integral of the BRDF against a white sky),
// in Karis' fit for mobile.
vec2 environment_brdf(float roughness, float n_v) {
    vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * n_v)) * r.x + r.y;
    return vec2(-1.04, 1.04) * a004 + r.zw;
}

// What a surface of reflectance `f0` reflects of a white sky, a rough one's
// second bounce included (Fdez-Aguera's multiple-scattering fit): a white
// metal gives all of it back, as a furnace test wants.
vec3 sky_reflectance(vec3 f0, vec2 ab) {
    float single = ab.x + ab.y;
    vec3 one = f0 * ab.x + ab.y;
    vec3 average = f0 + (vec3(1.0) - f0) / 21.0;
    return one + (1.0 - single) * one * average / (vec3(1.0) - (1.0 - single) * average);
}

// One light from `l` (toward it, length 1) of `radiance` (kin's colour).
struct Surface {
    vec3 n;
    vec3 v;
    float n_v;
    float alpha;
    vec3 diffuse; // the colour it scatters
    vec3 f0;      // what it reflects head on
    vec3 boost;   // the light a rough specular loses to a second bounce, put back
    vec3 backlight; // a thin surface's (a leaf's) colour for the light it lets through from behind; 0: none
    float coat;       // a clear coat's strength over it (glTF's KHR_materials_clearcoat); 0: none
    float coat_alpha; // the coat's roughness squared
    float dielectric; // what a dielectric reflects head on (0.04: KinSurface::specular 0.5)
    float wrap;       // how far the diffuse light wraps round (KinSurface::diffuse_wrap); 0: Lambert's
};

// The diffuse light's share at `n_l` (the cosine to the light), wrapped by `wrap`: Lambert's at 0; at 0.5 the light's
// first two spherical harmonic bands (1/4 + n.l / 2: some to the side facing away, as Unreal's directional volume
// lighting); at 1 the first alone (1/4 from every side, its non-directional). The same on average over every way a
// surface faces.
float wrapped_diffuse(float n_l, float wrap) {
    return max(min(0.5 * wrap, 0.25) + (1.0 - wrap) * n_l, 0.0);
}

// The light a thin surface lets through from `l` (Unreal's two-sided foliage): the light on its back, wrapped by half
// (McAuley's energy-conserving wrap: some still where the front is lit at a slant), scattered on mostly the way it
// went, by a GGX lobe of alpha 0.6 about it, in kin's units (a light of colour 1 on white: 1, as the diffuse is).
vec3 transmitted(Surface s, vec3 l, float n_l) {
    float wrap = clamp((0.5 - n_l) / 2.25, 0.0, 1.0);
    float on = clamp(-dot(s.v, l), 0.0, 1.0);
    float d = on * on * (0.36 - 1.0) + 1.0;
    return s.backlight * (wrap * 0.36 / (d * d));
}

vec3 shade(Surface s, vec3 l, vec3 radiance) {
    float n_l = dot(s.n, l);
    vec3 through = transmitted(s, l, n_l) * radiance;
    if (s.wrap > 0.0) {
        through += s.diffuse * (wrapped_diffuse(n_l, s.wrap) * (1.0 - s.dielectric)) * radiance;
    }
    if (n_l <= 0.0) {
        return through;
    }
    vec3 h = normalize(l + s.v);
    float n_h = clamp(dot(s.n, h), 0.0, 1.0);
    float v_h = clamp(dot(s.v, h), 0.0, 1.0);
    vec3 f = fresnel(s.f0, v_h);
    // What the specular reflects the diffuse does not: kd takes the rest, by
    // a dielectric's Fresnel (the diffuse of a metal is already gone from
    // `diffuse`; between, glTF mixes the two materials).
    vec3 specular = s.boost * f * (distribution(n_h, s.alpha) * visibility(n_l, max(s.n_v, 1e-4), s.alpha) * PI);
    // (A wrapped diffuse is in `through`, weighed by the reflectance head on: Lambert's here.)
    vec3 lit = (s.wrap > 0.0 ? vec3(0.0) : s.diffuse * (1.0 - fresnel(vec3(s.dielectric), v_h))) + specular;
    if (s.coat > 0.0) {
        // A clear coat (a dielectric of IOR 1.5, glTF's KHR_materials_clearcoat): its own highlight, and what it
        // reflects the layer below does not get.
        float fc = s.coat * fresnel(vec3(0.04), v_h).x;
        lit = lit * (1.0 - fc) + vec3(fc * distribution(n_h, s.coat_alpha) * visibility(n_l, max(s.n_v, 1e-4), s.coat_alpha) * PI);
    }
    return lit * radiance * n_l + through;
}
