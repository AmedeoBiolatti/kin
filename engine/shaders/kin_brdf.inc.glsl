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
};

vec3 shade(Surface s, vec3 l, vec3 radiance) {
    float n_l = dot(s.n, l);
    if (n_l <= 0.0) {
        return vec3(0.0);
    }
    vec3 h = normalize(l + s.v);
    float n_h = clamp(dot(s.n, h), 0.0, 1.0);
    float v_h = clamp(dot(s.v, h), 0.0, 1.0);
    vec3 f = fresnel(s.f0, v_h);
    // What the specular reflects the diffuse does not: kd takes the rest, by
    // a dielectric's Fresnel (the diffuse of a metal is already gone from
    // `diffuse`; between, glTF mixes the two materials).
    vec3 specular = s.boost * f * (distribution(n_h, s.alpha) * visibility(n_l, max(s.n_v, 1e-4), s.alpha) * PI);
    return (s.diffuse * (1.0 - fresnel(vec3(0.04), v_h)) + specular) * radiance * n_l;
}
