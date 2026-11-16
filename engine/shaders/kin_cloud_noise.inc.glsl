// The clouds' tiling noise functions (cloud_noise.comp's and cloud_weather.comp's): the same arithmetic as clouds3d.cpp's
// noise_texel (the tests compare it): change them together.
// PCG3D (Jarzynski and Olano): integer bits, the same on every GPU and in clouds3d.cpp.
uvec3 pcg3d(uvec3 v) {
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return v;
}

uvec3 cell_hash(ivec3 c, int period, uint salt) {
    // (c is never under -1: one period more makes it positive, where GLSL's % is defined.)
    uvec3 w = uvec3(c + ivec3(period)) % uint(period);
    return pcg3d(w + uvec3(salt * 0x9E3779B9u, salt * 0x85EBCA6Bu, salt * 0xC2B2AE35u));
}

vec3 unit3(uvec3 h) {
    return vec3(h >> 8u) * (1.0 / 16777216.0);
}

float worley(vec3 p, int cells, uint salt) {
    vec3 q = p * float(cells);
    vec3 c = floor(q);
    vec3 f = q - c;
    float nearest = 1e9;
    for (int oz = -1; oz <= 1; ++oz) {
        for (int oy = -1; oy <= 1; ++oy) {
            for (int ox = -1; ox <= 1; ++ox) {
                ivec3 o = ivec3(ox, oy, oz);
                vec3 d = vec3(o) + unit3(cell_hash(ivec3(c) + o, cells, salt)) - f;
                nearest = min(nearest, dot(d, d));
            }
        }
    }
    return 1.0 - min(sqrt(nearest), 1.0);
}

float worley_fbm(vec3 p, int cells, uint salt) {
    return 0.625 * worley(p, cells, salt) + 0.25 * worley(p, cells * 2, salt + 1u) + 0.125 * worley(p, cells * 4, salt + 2u);
}

float gradient_dot(uvec3 h, vec3 v) {
    switch (h.x % 12u) {
    case 0u: return v.x + v.y;
    case 1u: return -v.x + v.y;
    case 2u: return v.x - v.y;
    case 3u: return -v.x - v.y;
    case 4u: return v.x + v.z;
    case 5u: return -v.x + v.z;
    case 6u: return v.x - v.z;
    case 7u: return -v.x - v.z;
    case 8u: return v.y + v.z;
    case 9u: return -v.y + v.z;
    case 10u: return v.y - v.z;
    default: return -v.y - v.z;
    }
}

float perlin(vec3 p, int cells, uint salt) {
    vec3 q = p * float(cells);
    vec3 c = floor(q);
    vec3 f = q - c;
    ivec3 i = ivec3(c);
    float corner[8];
    for (int k = 0; k < 8; ++k) {
        ivec3 o = ivec3(k & 1, (k >> 1) & 1, k >> 2);
        corner[k] = gradient_dot(cell_hash(i + o, cells, salt), f - vec3(o));
    }
    vec3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float x00 = mix(corner[0], corner[1], u.x), x10 = mix(corner[2], corner[3], u.x);
    float x01 = mix(corner[4], corner[5], u.x), x11 = mix(corner[6], corner[7], u.x);
    return mix(mix(x00, x10, u.y), mix(x01, x11, u.y), u.z);
}

float perlin_fbm(vec3 p, int cells, int octaves, uint salt) {
    float sum = 0.0, amplitude = 1.0, total = 0.0;
    for (int k = 0; k < octaves; ++k) {
        sum += amplitude * perlin(p, cells << k, salt + uint(k));
        total += amplitude;
        amplitude *= 0.5;
    }
    return clamp(0.5 + 1.1 * sum / total, 0.0, 1.0);
}

// The texel at `at` of a noise `size` across: 0 the shape volume, 1 the detail volume, 2 the coverage map (at.z unused).
vec4 kin_cloud_texel(uint noise, uvec3 at, uint size) {
    float inv = 1.0 / float(size);
    if (noise == 0u) {
        vec3 p = (vec3(at) + 0.5) * inv;
        float low = worley_fbm(p, 4, 0u);
        float pw = perlin_fbm(p, 4, 3, 10u);
        return vec4(low + pw * (1.0 - low), low, worley_fbm(p, 8, 20u), worley_fbm(p, 16, 30u));
    }
    if (noise == 1u) {
        vec3 p = (vec3(at) + 0.5) * inv;
        return vec4(worley_fbm(p, 2, 40u), worley_fbm(p, 4, 50u), worley_fbm(p, 8, 60u), 1.0);
    }
    // A plane through the middle of a tiling volume (off its lattice, where gradient noise is zero).
    vec3 p = vec3((vec2(at.xy) + 0.5) * inv, 0.37);
    float coverage = clamp(0.5 + 1.6 * (perlin_fbm(p, 3, 5, 70u) - 0.5), 0.0, 1.0);
    float kind = clamp(0.5 + 1.6 * (perlin_fbm(p, 2, 3, 80u) - 0.5), 0.0, 1.0);
    return vec4(coverage, kind, 0.0, 1.0);
}
