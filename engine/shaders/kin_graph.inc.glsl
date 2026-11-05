// What material graphs' nodes call (docs/material_graph_plan.md; kin/render3d/material_graph3d.hpp writes the calls):
// included by a graph's surface shader, after kin_mesh_surface.inc.glsl (the kin_* functions and the samplers).

// A map read as colour: sRGB decoded where kin reads its textures undecoded (as kin_base_color does), or as data.
vec4 kin_graph_sample(sampler2D map, vec2 uv, bool srgb) {
    vec4 t = texture(map, uv);
    if (srgb && frame.flags.y > 0.5) {
        t.rgb = decode(t.rgb);
    }
    return t;
}

// The surface's normal before any map: the mesh's, on the viewer's side (a face's own where the mesh has none).
vec3 kin_graph_geometry_normal() {
    const vec3 dp1 = dFdx(vWorld), dp2 = dFdy(vWorld);
    const float front = gl_FrontFacing ? 1.0 : -1.0;
    return dot(vNormal, vNormal) < 1e-20 ? kin_screen_normal(dp1, dp2) : normalize(vNormal) * front;
}

// A tangent-space normal (x right, y up as glTF's maps have it, z out) in world space, by the frame the UVs make across
// the pixel (as mapped_normal's), about `n`.
vec3 kin_graph_tangent_to_world(vec3 m, vec2 uv, vec3 n) {
    const vec3 dp1 = dFdx(vWorld), dp2 = dFdy(vWorld);
    const vec2 duv1 = dFdx(uv), duv2 = dFdy(uv);
    const float det = duv1.x * duv2.y - duv2.x * duv1.y;
    if (abs(det) < 1e-20) {
        return n;
    }
    const float front = gl_FrontFacing ? 1.0 : -1.0;
    vec3 t = (dp1 * duv2.y - dp2 * duv1.y) * sign(det);
    vec3 b = (dp2 * duv1.x - dp1 * duv2.x) * sign(det);
    t = normalize(t - n * dot(n, t));
    b = normalize(b - n * dot(n, b));
    return normalize(front * (t * m.x - b * m.y) + n * m.z);
}

// A normal map's texel as a tangent-space normal, its slope times `strength` (two-channel maps: z rebuilt).
vec3 kin_graph_unpack_normal(vec4 texel, float strength) {
    vec3 m = texel.xyz * 2.0 - 1.0;
    if (m.z < 0.0) {
        m.z = sqrt(max(1.0 - dot(m.xy, m.xy), 0.0));
    }
    m.xy *= strength;
    return normalize(m);
}

// A map projected along the three world axes, blended by how the surface faces each (`sharpness`: the blend's power).
vec4 kin_graph_triplanar(sampler2D map, vec3 p, vec3 n, float scale, float sharpness, bool srgb) {
    vec3 w = pow(abs(n), vec3(max(sharpness, 1.0)));
    w /= max(w.x + w.y + w.z, 1e-6);
    return kin_graph_sample(map, p.zy * scale, srgb) * w.x + kin_graph_sample(map, p.xz * scale, srgb) * w.y +
           kin_graph_sample(map, p.xy * scale, srgb) * w.z;
}

// --- Noise: deterministic, hash-based (the same picture on every GPU) ---

uint kin_graph_hash(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
float kin_graph_random(ivec3 c) {
    return float(kin_graph_hash(uint(c.x) * 0x8da6b343u ^ kin_graph_hash(uint(c.y) * 0xd8163841u ^
                                                                        kin_graph_hash(uint(c.z) * 0xcb1ab31fu)))) /
           4294967295.0;
}
vec3 kin_graph_random3(ivec3 c) {
    return vec3(kin_graph_random(c), kin_graph_random(c + ivec3(17, 59, 113)), kin_graph_random(c + ivec3(71, 3, 37)));
}

// Value noise in [0, 1]: random values at the lattice's points, smoothly interpolated.
float kin_graph_value_noise(vec3 p) {
    const ivec3 i = ivec3(floor(p));
    const vec3 f = fract(p), u = f * f * (3.0 - 2.0 * f);
    float v[8];
    for (int k = 0; k < 8; ++k) {
        v[k] = kin_graph_random(i + ivec3(k & 1, (k >> 1) & 1, k >> 2));
    }
    return mix(mix(mix(v[0], v[1], u.x), mix(v[2], v[3], u.x), u.y), mix(mix(v[4], v[5], u.x), mix(v[6], v[7], u.x), u.y),
               u.z);
}

// Gradient (Perlin) noise in [0, 1].
float kin_graph_gradient_noise(vec3 p) {
    const ivec3 i = ivec3(floor(p));
    const vec3 f = fract(p), u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float v[8];
    for (int k = 0; k < 8; ++k) {
        const ivec3 c = ivec3(k & 1, (k >> 1) & 1, k >> 2);
        const vec3 g = normalize(kin_graph_random3(i + c) * 2.0 - 1.0 + 1e-6);
        v[k] = dot(g, f - vec3(c));
    }
    const float n = mix(mix(mix(v[0], v[1], u.x), mix(v[2], v[3], u.x), u.y),
                        mix(mix(v[4], v[5], u.x), mix(v[6], v[7], u.x), u.y), u.z);
    return clamp(n * 0.866 + 0.5, 0.0, 1.0);
}

// Cellular (Worley) noise: the distance to the nearest of one random point a cell, about [0, 1].
float kin_graph_cellular_noise(vec3 p) {
    const ivec3 i = ivec3(floor(p));
    const vec3 f = fract(p);
    float nearest = 8.0;
    for (int z = -1; z <= 1; ++z)
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x) {
                const ivec3 c = ivec3(x, y, z);
                nearest = min(nearest, length(vec3(c) + kin_graph_random3(i + c) - f));
            }
    return clamp(nearest, 0.0, 1.0);
}

// Fractal sum of gradient noise: `octaves` (1 to 8) of it, each `lacunarity` times finer and `gain` times weaker.
float kin_graph_fbm(vec3 p, int octaves, float lacunarity, float gain) {
    float sum = 0.0, amplitude = 1.0, total = 0.0;
    for (int o = 0; o < 8; ++o) {
        if (o >= octaves) {
            break;
        }
        sum += kin_graph_gradient_noise(p) * amplitude;
        total += amplitude;
        p *= lacunarity;
        amplitude *= gain;
    }
    return sum / max(total, 1e-6);
}

// A checkerboard of 0 and 1, `scale` squares a unit.
float kin_graph_checker(vec2 uv, float scale) {
    const ivec2 c = ivec2(floor(uv * scale));
    return float((c.x + c.y) & 1);
}
