// kin's global illumination (docs/3d_plan.md, G5): what its shaders share. The GI scene's walk (with KIN_GI_SCENE:
// the including shader declares uGiDistance and uGiAlbedo, the scene's 3D textures) and the probes' light (with
// KIN_GI_PROBES: uGiIrradiance, uGiMoments and uGiProbes, the probes' 3D atlases).
//
// The probes (G5b; DDGI, Majercik et al. 2019) stand on a grid: probe (i, j, k) at origin + (i, j, k) * spacing, plus
// its own offset (moved out of walls). Each holds, in a tile of the irradiance atlas, the light a surface facing each
// way receives (irradiance over pi, as kin's light colours are: a white surface shows it), 6 x 6 texels of an
// octahedral map with a border of one; and in a tile of the moments atlas the mean and mean square of its rays'
// distances each way, 14 x 14 with a border. A tile's layer (z) is the probe's k, its place in the layer i and j.

// The probe grid, as the frame's block and the compute shaders' carry it.
struct KinGiProbes {
    vec4 origin; // xyz: probe (0, 0, 0)'s place (world); w: the spacing (world units)
    vec4 counts; // xyz: probes along each axis; w: 1 when they light the scene
    vec4 bias;   // x: the look-up moved out along the normal (world units), y: toward the eye, z: their light's strength
};

vec2 kin_gi_sign(vec2 v) {
    return vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
}

// A unit direction to the square [-1, 1]^2 and back (the octahedron's faces unfolded).
vec2 kin_gi_octahedral(vec3 n) {
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    return n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * kin_gi_sign(n.xy);
}
vec3 kin_gi_direction(vec2 e) {
    vec3 v = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (v.z < 0.0) {
        v.xy = (1.0 - abs(v.yx)) * kin_gi_sign(v.xy);
    }
    return normalize(v);
}

#ifdef KIN_GI_SCENE
// Walks from `o` along the unit `way` (both in voxels) through the GI scene of `size` voxels, up to `far` voxels: true
// at the first voxel a surface is in (`hit`, and `t` where the ray reached it). One voxel at a time where a surface is
// near, else in jumps the distances allow (from a voxel whose centre is d voxels from the nearest surface voxel's,
// no surface voxel's box is nearer than d - 1.74 to any point of it). `steps` counts what it took.
bool kin_gi_march(vec3 o, vec3 way, float far, vec3 size, int max_steps, out ivec3 hit, out float t,
                  inout int steps) {
    const vec3 inv = 1.0 / way;
    const vec3 t0 = -o * inv, t1 = (size - o) * inv;
    const float enter = max(max(max(min(t0.x, t1.x), min(t0.y, t1.y)), min(t0.z, t1.z)), 0.0);
    const float leave = min(min(min(max(t0.x, t1.x), max(t0.y, t1.y)), max(t0.z, t1.z)), far);
    t = enter + 1e-4;
    hit = ivec3(0);
    const ivec3 last = ivec3(size) - 1;
    for (; steps < max_steps && t < leave; ++steps) {
        const vec3 q = o + way * t;
        const ivec3 voxel = clamp(ivec3(floor(q)), ivec3(0), last);
        if (texelFetch(uGiAlbedo, voxel, 0).a > 0.0) {
            hit = voxel;
            return true;
        }
        const float d = texelFetch(uGiDistance, voxel, 0).r;
        if (d >= 3.0) {
            t += d - 1.74;
        } else {
            const vec3 exit = (vec3(voxel) + step(vec3(0.0), way) - o) * inv;
            t = max(min(min(exit.x, exit.y), exit.z), t) + 1e-4;
        }
    }
    return false;
}
#endif

#ifdef KIN_GI_PROBES
// Where direction `n` reads in probe `probe`'s tile of an atlas whose tiles are `tile` texels a side (the map inside a
// border of one), as the coordinates of a sampler3D over the atlas.
vec3 kin_gi_atlas(ivec3 probe, vec3 n, float tile, vec3 counts) {
    const vec2 e = kin_gi_octahedral(n) * 0.5 + 0.5;
    const vec2 texel = vec2(probe.xy) * tile + 1.0 + e * (tile - 2.0);
    return vec3(texel / (counts.xy * tile), (float(probe.z) + 0.5) / counts.z);
}

// The light a surface at `p` facing `n` receives from the probes (irradiance over pi), seen from `to_eye` (unit, toward
// the camera; zero where there is none): the eight probes round it, each weighted by how near it is (trilinear), by
// whether it is in front of the surface (a wrapped cosine) and by whether it can see the surface (Chebyshev's
// inequality on its distances that way: a wall between them keeps its light out). A probe asleep or inside a wall
// (its data's w under 0.5) gives nothing.
vec3 kin_gi_irradiance(KinGiProbes g, vec3 p, vec3 n, vec3 to_eye) {
    const vec3 biased = p + n * g.bias.x + to_eye * g.bias.y;
    const vec3 grid = (biased - g.origin.xyz) / g.origin.w;
    const ivec3 last = ivec3(g.counts.xyz) - 1;
    const ivec3 base = clamp(ivec3(floor(grid)), ivec3(0), max(last - 1, ivec3(0)));
    const vec3 alpha = clamp(grid - vec3(base), 0.0, 1.0);
    vec3 sum = vec3(0.0);
    float total = 0.0;
    for (int i = 0; i < 8; ++i) {
        const ivec3 offset = ivec3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        const ivec3 probe = min(base + offset, last);
        const vec4 data = texelFetch(uGiProbes, probe, 0);
        if (data.w < 0.5) {
            continue;
        }
        const vec3 at = g.origin.xyz + vec3(probe) * g.origin.w + data.xyz;
        const vec3 trilinear = mix(1.0 - alpha, alpha, vec3(offset));
        const vec3 to_probe = at - p;
        const float facing = (dot(to_probe * inversesqrt(max(dot(to_probe, to_probe), 1e-12)), n) + 1.0) * 0.5;
        float weight = facing * facing + 0.2;
        const vec3 from_probe = biased - at;
        const float d = length(from_probe);
        const vec2 m = textureLod(uGiMoments, kin_gi_atlas(probe, from_probe / max(d, 1e-6), 16.0, g.counts.xyz), 0.0).rg;
        if (d > m.x) {
            const float variance = abs(m.y - m.x * m.x);
            const float away = d - m.x;
            float chebyshev = variance / (variance + away * away);
            chebyshev = chebyshev * chebyshev * chebyshev;
            weight *= max(0.05, chebyshev);
        }
        weight = max(weight, 1e-6);
        if (weight < 0.2) {
            weight *= weight * weight / 0.04; // the faintest lose more: a probe that barely sees leaks less
        }
        weight *= trilinear.x * trilinear.y * trilinear.z;
        sum += weight * textureLod(uGiIrradiance, kin_gi_atlas(probe, n, 8.0, g.counts.xyz), 0.0).rgb;
        total += weight;
    }
    return total > 0.0 ? sum / total : vec3(0.0);
}
#endif
