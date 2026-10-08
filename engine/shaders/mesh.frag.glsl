#version 450 core
// kin's 3D meshes (kin/render3d): a base colour (vertex, instance and
// material colours, times a texture), lit in linear light by a sun (through
// its shadow map), ambient light and up to 16 point lights, plus the
// material's own light; or unlit. The lighting is glTF's metallic-roughness
// model: a Lambert diffuse and a Cook-Torrance specular (GGX distribution,
// Smith visibility, Schlick Fresnel). Output premultiplied, as kin's targets are.
//
// Light colours are irradiance over pi (a white surface facing a light of
// colour c shows c), so a BRDF f meets a light as f * pi * c * cos.
layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec4 vColor; // linear, straight alpha
layout(location = 2) in vec2 vUV;
layout(location = 3) in vec3 vWorld;

layout(set = 2, binding = 0) uniform sampler2D uBase;         // white when the material has none
layout(set = 2, binding = 1) uniform sampler2DShadow uShadow; // depth from the sun, compared
layout(set = 2, binding = 2) uniform sampler2D uMetalRough;   // G roughness, B metallic: white without
layout(set = 2, binding = 3) uniform sampler2D uNormalMap;    // tangent space; any without (see maps.x)
layout(set = 2, binding = 4) uniform sampler2D uOcclusion;    // R: white without
layout(set = 2, binding = 5) uniform sampler2D uEmissive;     // any without (see maps.y)

const int MAX_LIGHTS = 16;
const float PI = 3.14159265358979;
// The frame's, pushed once a pass.
layout(set = 3, binding = 0) uniform Frame {
    vec4 sun_direction; // xyz: toward the sun, length 1
    vec4 sun_color;     // linear
    vec4 ambient;       // linear: from above
    vec4 ground_ambient; // linear: from below (the same as ambient: even all round)
    vec4 fill_direction; // a second sun, without shadows
    vec4 fill_color;
    vec4 flags;         // x: encode to sRGB (a gamma target), y: decode the texture, z: shadows, w: point lights
    vec4 eye;           // xyz: the camera, in world space
    mat4 shadow_matrix; // world to the shadow map's clip space
    vec4 shadow;        // x: depth bias, y: normal offset (world units), z: a texel (uv)
    vec4 lights[MAX_LIGHTS * 2]; // position.xyz and radius; colour.rgb (linear)
} frame;
// The draw's.
layout(set = 3, binding = 1) uniform Material {
    vec4 params; // x: emissive, y: unlit, z: receives the sun's shadow, w: alpha cutoff (0: none)
    vec4 pbr;    // x: metallic, y: roughness, z: normal scale, w: occlusion strength
    vec4 maps;   // x: has a normal map, y: has an emissive map
} material;

layout(location = 0) out vec4 FragColor;

vec3 decode(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}

vec3 encode(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}

// How much of the sun reaches here: 3x3 comparisons against the shadow map,
// from a point pushed a little off the surface (no self-shadowing acne).
// Reverse Z: the map keeps the depth nearest the sun, the greatest.
float sunlight(vec3 n) {
    if (frame.flags.z < 0.5 || material.params.z < 0.5) {
        return 1.0;
    }
    const vec4 clip = frame.shadow_matrix * vec4(vWorld + n * frame.shadow.y, 1.0);
    const vec3 ndc = clip.xyz / clip.w;
    const vec2 uv = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0 || ndc.z < 0.0 || ndc.z > 1.0) {
        return 1.0;
    }
    const float ref = ndc.z + frame.shadow.x;
    float lit = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            lit += texture(uShadow, vec3(uv + vec2(x, y) * frame.shadow.z, ref));
        }
    }
    return lit / 9.0;
}

// The normal bent by the map. The tangent frame comes from how the world
// position and the UVs change across the pixel, so meshes need no tangents:
// the directions u and v increase in, solved from the two derivatives (the
// same on either way the screen's y runs). glTF's maps have green up and v
// runs down. `front` is -1 seen from behind, where the whole frame turns.
vec3 mapped_normal(vec3 n, float front) {
    const vec3 dp1 = dFdx(vWorld), dp2 = dFdy(vWorld);
    const vec2 duv1 = dFdx(vUV), duv2 = dFdy(vUV);
    const float det = duv1.x * duv2.y - duv2.x * duv1.y;
    if (abs(det) < 1e-20) {
        return n; // no UVs to go by here
    }
    vec3 t = (dp1 * duv2.y - dp2 * duv1.y) * sign(det);
    vec3 b = (dp2 * duv1.x - dp1 * duv2.x) * sign(det);
    t = normalize(t - n * dot(n, t));
    b = normalize(b - n * dot(n, b));
    vec3 m = texture(uNormalMap, vUV).xyz * 2.0 - 1.0;
    m.xy *= material.pbr.z;
    return normalize(front * (t * m.x - b * m.y) + n * m.z);
}

// GGX's share of the microfacets facing the half vector, alpha = roughness^2.
float distribution(float n_h, float alpha) {
    const float a2 = alpha * alpha;
    const float d = n_h * n_h * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

// Smith's visibility, height-correlated: the geometry term over 4 n.l n.v.
float visibility(float n_l, float n_v, float alpha) {
    const float a2 = alpha * alpha;
    const float v = n_l * sqrt(n_v * n_v * (1.0 - a2) + a2);
    const float l = n_v * sqrt(n_l * n_l * (1.0 - a2) + a2);
    return 0.5 / max(v + l, 1e-5);
}

vec3 fresnel(vec3 f0, float v_h) {
    const float k = pow(1.0 - v_h, 5.0);
    return f0 + (vec3(1.0) - f0) * k;
}

// The specular albedo of a surface lit from everywhere, as the scale and the
// offset of its F0 (the split-sum integral of the BRDF against a white sky),
// in Karis' fit for mobile.
vec2 environment_brdf(float roughness, float n_v) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    const vec4 r = roughness * c0 + c1;
    const float a004 = min(r.x * r.x, exp2(-9.28 * n_v)) * r.x + r.y;
    return vec2(-1.04, 1.04) * a004 + r.zw;
}

// What a surface of reflectance `f0` reflects of a white sky, a rough one's
// second bounce included (Fdez-Aguera's multiple-scattering fit): a white
// metal gives all of it back, as a furnace test wants.
vec3 sky_reflectance(vec3 f0, vec2 ab) {
    const float single = ab.x + ab.y;
    const vec3 one = f0 * ab.x + ab.y;
    const vec3 average = f0 + (vec3(1.0) - f0) / 21.0;
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
    const float n_l = dot(s.n, l);
    if (n_l <= 0.0) {
        return vec3(0.0);
    }
    const vec3 h = normalize(l + s.v);
    const float n_h = clamp(dot(s.n, h), 0.0, 1.0);
    const float v_h = clamp(dot(s.v, h), 0.0, 1.0);
    const vec3 f = fresnel(s.f0, v_h);
    // What the specular reflects the diffuse does not: kd takes the rest, by
    // a dielectric's Fresnel (the diffuse of a metal is already gone from
    // `diffuse`; between, glTF mixes the two materials).
    const vec3 specular = s.boost * f * (distribution(n_h, s.alpha) * visibility(n_l, max(s.n_v, 1e-4), s.alpha) * PI);
    return (s.diffuse * (1.0 - fresnel(vec3(0.04), v_h)) + specular) * radiance * n_l;
}

void main() {
    vec4 tex = texture(uBase, vUV);
    if (frame.flags.y > 0.5) {
        tex.rgb = decode(tex.rgb);
    }
    const vec3 base = vColor.rgb * tex.rgb;
    float alpha = vColor.a * tex.a;
    if (alpha < material.params.w) {
        discard;
    }
    // A cut-out is opaque where it stays.
    if (material.params.w > 0.0) {
        alpha = 1.0;
    }
    vec3 c;
    if (material.params.y > 0.5) {
        c = base * (vec3(1.0) + vec3(material.params.x));
    } else {
        // Seen from behind (double-sided), the face turns its normal round.
        const float front = gl_FrontFacing ? 1.0 : -1.0;
        vec3 n = normalize(vNormal) * front;
        if (material.maps.x > 0.5) {
            n = mapped_normal(n, front);
        }
        const vec3 v = normalize(frame.eye.xyz - vWorld);
        const vec3 mr = texture(uMetalRough, vUV).rgb;
        const float metallic = clamp(material.pbr.x * mr.b, 0.0, 1.0);
        // A floor on the roughness: a perfect mirror would be a single bright pixel.
        const float roughness = clamp(material.pbr.y * mr.g, 0.045, 1.0);
        Surface s;
        s.n = n;
        s.v = v;
        s.n_v = clamp(dot(n, v), 0.0, 1.0);
        s.alpha = roughness * roughness;
        s.diffuse = base * (1.0 - metallic);
        s.f0 = mix(vec3(0.04), base, metallic);
        // A rough microsurface lights itself with a bounce that single
        // scattering leaves out, most of all a metal's (Fdez-Aguera's
        // multiple-scattering fit): put back in the specular, taken from the diffuse.
        const vec2 ab = environment_brdf(roughness, s.n_v);
        const float single = ab.x + ab.y;
        s.boost = vec3(1.0) + s.f0 * (1.0 / max(single, 1e-3) - 1.0);

        vec3 light = vec3(0.0);
        const float sun = dot(n, frame.sun_direction.xyz);
        if (sun > 0.0) {
            light += shade(s, frame.sun_direction.xyz, frame.sun_color.rgb * sunlight(n));
        }
        if (dot(frame.fill_color.rgb, vec3(1.0)) > 0.0) {
            light += shade(s, frame.fill_direction.xyz, frame.fill_color.rgb);
        }
        const int count = int(frame.flags.w);
        for (int i = 0; i < count; ++i) {
            const vec4 at = frame.lights[2 * i];
            const vec3 to = at.xyz - vWorld;
            const float d2 = dot(to, to);
            // The inverse square, windowed to nothing at the radius (Karis'
            // fall-off, as glTF's punctual lights); no brighter than at the
            // light's own size (w: its square), so a surface inside a flash
            // is lit as at its edge, not without bound.
            const float r2 = d2 / (at.w * at.w);
            const float window = clamp(1.0 - r2 * r2, 0.0, 1.0);
            const float k = window * window / max(d2, frame.lights[2 * i + 1].w);
            light += shade(s, to * inversesqrt(max(d2, 1e-8)), frame.lights[2 * i + 1].rgb * k);
        }

        // Ambient, from the sky above to the ground below by which way a
        // direction looks: the diffuse by the normal, the specular by the
        // reflection (turned toward the normal as the surface roughens: a
        // rough one sees a wide cone of the sky).
        const float occlusion = mix(1.0, texture(uOcclusion, vUV).r, material.pbr.w);
        const vec3 reflected = normalize(mix(reflect(-v, n), n, s.alpha));
        const vec3 around = mix(frame.ground_ambient.rgb, frame.ambient.rgb, n.y * 0.5 + 0.5);
        const vec3 above = mix(frame.ground_ambient.rgb, frame.ambient.rgb, reflected.y * 0.5 + 0.5);
        const vec3 dielectric = sky_reflectance(vec3(0.04), ab);
        const vec3 reflectance = mix(dielectric, sky_reflectance(base, ab), metallic);
        light += (s.diffuse * (vec3(1.0) - dielectric) * around + reflectance * above) * occlusion;

        // The material's own light: its colour's share, or its map's.
        vec3 own = base;
        if (material.maps.y > 0.5) {
            own = texture(uEmissive, vUV).rgb;
            if (frame.flags.y > 0.5) {
                own = decode(own);
            }
        }
        c = light + own * material.params.x;
    }
    if (frame.flags.x > 0.5) {
        c = encode(c);
    }
    FragColor = vec4(c * alpha, alpha);
}
