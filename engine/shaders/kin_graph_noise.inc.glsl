#ifndef KIN_GRAPH_NOISE_INC
#define KIN_GRAPH_NOISE_INC
// Material graphs' procedural nodes (kin_graph.inc.glsl, kin_cutout.inc.glsl): pure functions, no resources.

// --- Filtered edges ---

// The smoothstep node: its edges widened, each way, by half the pixel's footprint of x (its fwidth): a sharp
// procedural edge (a tile's seam, a terrazzo chip) seen far away filters to its share of the pixel instead of breaking
// into dots that crawl as the eye moves; up close (a footprint well under the edges' gap) it is smoothstep. Edges may
// run either way (edge0 above edge1: one minus). Where there are no derivatives (a vertex's offset:
// KIN_GRAPH_NO_DERIVATIVES) it is smoothstep.
#ifdef KIN_GRAPH_NO_DERIVATIVES
#define KIN_GRAPH_SMOOTHSTEP(T, E)                                                                                     \
    T kin_graph_smoothstep(E e0, E e1, T x) {                                                                          \
        return smoothstep(T(e0), T(e1), x);                                                                            \
    }
#else
#define KIN_GRAPH_SMOOTHSTEP(T, E)                                                                                     \
    T kin_graph_smoothstep(E e0, E e1, T x) {                                                                          \
        const T w = 0.5 * fwidth(x) * sign(T(e1) - T(e0));                                                            \
        return smoothstep(T(e0) - w, T(e1) + w, x);                                                                    \
    }
#endif
KIN_GRAPH_SMOOTHSTEP(float, float)
KIN_GRAPH_SMOOTHSTEP(vec2, vec2)
KIN_GRAPH_SMOOTHSTEP(vec3, vec3)
KIN_GRAPH_SMOOTHSTEP(vec4, vec4)
KIN_GRAPH_SMOOTHSTEP(vec2, float)
KIN_GRAPH_SMOOTHSTEP(vec3, float)
KIN_GRAPH_SMOOTHSTEP(vec4, float)
#undef KIN_GRAPH_SMOOTHSTEP

// --- Directions ---

// Where an equirectangular picture shows direction `d` (the environment's layout, environment_uv's without its turn:
// the centre +x, the top +y, right toward +z): uv in .xy, and in .z its u half a turn round (its seam elsewhere: where
// u wraps, the derivatives are that one's).
vec3 kin_graph_equirect_uv(vec3 d) {
    d = normalize(d);
    const float u = 0.5 + atan(d.z, d.x) / 6.28318530718;
    return vec3(u, acos(clamp(d.y, -1.0, 1.0)) / 3.14159265359, fract(u + 0.5));
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
#endif
