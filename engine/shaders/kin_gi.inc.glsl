// kin's global illumination (docs/3d_plan.md, G5): what its shaders share. The GI scene's walk (with KIN_GI_SCENE:
// the including shader declares uGiDistance and uGiAlbedo, the scene's 3D textures) and the probes' light (with
// KIN_GI_PROBES: uGiIrradiance, uGiMoments and uGiProbes, the probes' 3D atlases).
//
// The GI scene is in levels (G5c), each a window of the same number of voxels, each level's voxels twice the last's,
// round the camera (or over the whole area where one level holds it). A window is stored wrapped round (toroidally): a
// voxel of world index w (in its level's voxels) is at w mod the window's size, so a window that moves keeps what it
// still covers and only the slab it gains is made. The levels are stacked along z in each texture.
//
// The probes (G5b; DDGI, Majercik et al. 2019) are a window of each level too: probe q (in probe steps from the world's
// origin) stands at q * spacing, plus its own offset (moved out of walls), and is stored at q mod the window's counts.
// Each holds, in a tile of the irradiance atlas, the light a surface facing each way receives (irradiance over pi, as
// kin's light colours are: a white surface shows it), 6 x 6 texels of an octahedral map with a border of one; and in a
// tile of the moments atlas the mean and mean square of its rays' distances each way, 14 x 14 with a border. A tile's
// layer is the probe's stored z (plus its level's), its place in the layer its stored x and y.

#define KIN_GI_MAX_LEVELS 4

// The GI scene's levels.
struct KinGiScene {
    vec4 size;      // xyz: voxels a window; w: levels
    vec4 level[4];  // xyz: the window's first voxel (whole numbers, in the level's voxels); w: the voxel's side (world)
};

// The probes' levels, as the frame's block and the compute shaders carry them.
struct KinGiProbes {
    vec4 counts;    // xyz: probes a window; w: 1 when they light the scene
    vec4 bias;      // x: the look-up moved out along the normal, y: toward the eye (in spacings); z: their light's
                    // strength; w: levels
    vec4 level[4];  // xyz: the window's first probe (whole numbers, in probe steps); w: the spacing (world)
};

ivec3 kin_gi_wrap(ivec3 w, ivec3 s) {
    return (w + s * 4096) % s;
}

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

// Where window voxel `local` of level `k` is stored (the level's offset along z included).
ivec3 kin_gi_stored(KinGiScene g, int k, ivec3 local) {
    const ivec3 s = ivec3(g.size.xyz);
    const ivec3 st = kin_gi_wrap(local + ivec3(g.level[k].xyz), s);
    return ivec3(st.xy, st.z + k * s.z);
}

#ifdef KIN_GI_SCENE
// Walks from `o` along the unit `way` (world) through the GI scene, up to `far` (world): true at the first voxel a
// surface is in (its stored place `hit`, its level `level`, and `t` where the ray reached it, world). It starts in the
// finest level whose window holds `o` and goes on in the next one up when it leaves a window. In a level: one voxel at
// a time where a surface is near, else in jumps the distances allow (from a voxel whose centre is d voxels from the
// nearest surface voxel's, no surface voxel's box is nearer than d - 1.74 to any point of it; the distances are kept
// to 8). `steps` counts what it took.
bool kin_gi_march(KinGiScene g, vec3 o, vec3 way, float far, int max_steps, out ivec3 hit, out int level, out float t,
                  inout int steps) {
    hit = ivec3(0);
    level = 0;
    t = 0.0;
    const vec3 size = g.size.xyz;
    const int levels = int(g.size.w);
    const vec3 inv = 1.0 / way;
    int k = 0;
    for (; k < levels; ++k) {
        const vec3 local = o / g.level[k].w - g.level[k].xyz;
        if (all(greaterThanEqual(local, vec3(0.0))) && all(lessThan(local, size))) {
            break;
        }
    }
    k = min(k, levels - 1); // outside every window: from the outermost one, where the ray meets it
    for (; k < levels; ++k) {
        const float cell = g.level[k].w;
        // In the level's window voxels, from where the ray is now.
        const vec3 lo = o / cell - g.level[k].xyz;
        const vec3 t0 = -lo * inv, t1 = (size - lo) * inv;
        const float enter = max(max(max(min(t0.x, t1.x), min(t0.y, t1.y)), min(t0.z, t1.z)), t / cell);
        const float leave = min(min(min(max(t0.x, t1.x), max(t0.y, t1.y)), max(t0.z, t1.z)), far / cell);
        float tv = enter + 1e-4;
        const ivec3 last = ivec3(size) - 1;
        for (; steps < max_steps && tv < leave; ++steps) {
            const vec3 q = lo + way * tv;
            const ivec3 voxel = clamp(ivec3(floor(q)), ivec3(0), last);
            const ivec3 at = kin_gi_stored(g, k, voxel);
            if (texelFetch(uGiAlbedo, at, 0).a > 0.0) {
                hit = at;
                level = k;
                t = tv * cell;
                return true;
            }
            const float d = texelFetch(uGiDistance, at, 0).r;
            if (d >= 3.0) {
                tv += d - 1.74;
            } else {
                const vec3 exit = (vec3(voxel) + step(vec3(0.0), way) - lo) * inv;
                tv = max(min(min(exit.x, exit.y), exit.z), tv) + 1e-4;
            }
        }
        t = max(tv, leave) * cell;
        if (steps >= max_steps || t >= far) {
            return false;
        }
    }
    return false;
}
#endif

#ifdef KIN_GI_PROBES
// Where direction `n` reads in the tile of the probe stored at `stored` (its level's z included) of an atlas whose
// tiles are `tile` texels a side (the map inside a border of one), as the coordinates of a sampler3D over the atlas.
vec3 kin_gi_atlas(ivec3 stored, vec3 n, float tile, vec3 counts, float levels) {
    const vec2 e = kin_gi_octahedral(n) * 0.5 + 0.5;
    const vec2 texel = vec2(stored.xy) * tile + 1.0 + e * (tile - 2.0);
    return vec3(texel / (counts.xy * tile), (float(stored.z) + 0.5) / (counts.z * levels));
}

// The probes' light of level `k` at `p` facing `n` (which the eight probes are weighted by), read the ways `ways`
// (`count` of them, up to 4) into `light`, the look-up moved to `biased` (world); false where none of the eight has
// light to give (asleep, in a wall, not yet traced).
bool kin_gi_level_light(KinGiProbes g, int k, vec3 p, vec3 biased, vec3 n, vec3 grid, vec3 ways[4], int count,
                        out vec3 light[4]) {
    const ivec3 counts = ivec3(g.counts.xyz);
    const ivec3 base = clamp(ivec3(floor(grid)), ivec3(0), max(counts - 2, ivec3(0)));
    const vec3 alpha = clamp(grid - vec3(base), 0.0, 1.0);
    const float spacing = g.level[k].w;
    vec3 sum[4] = vec3[4](vec3(0.0), vec3(0.0), vec3(0.0), vec3(0.0));
    float total = 0.0;
    for (int i = 0; i < 8; ++i) {
        const ivec3 offset = ivec3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        const ivec3 world = base + offset + ivec3(g.level[k].xyz);
        const ivec3 st = kin_gi_wrap(world, counts);
        const ivec3 stored = ivec3(st.xy, st.z + k * counts.z);
        const vec4 data = texelFetch(uGiProbes, stored, 0);
        if (data.w < 0.5) {
            continue;
        }
        const vec3 at = vec3(world) * spacing + data.xyz;
        const vec3 trilinear = mix(1.0 - alpha, alpha, vec3(offset));
        const vec3 to_probe = at - p;
        const float facing = (dot(to_probe * inversesqrt(max(dot(to_probe, to_probe), 1e-12)), n) + 1.0) * 0.5;
        float weight = facing * facing + 0.2;
        const vec3 from_probe = biased - at;
        const float d = length(from_probe);
        const vec2 m = textureLod(uGiMoments, kin_gi_atlas(stored, from_probe / max(d, 1e-6), 16.0, g.counts.xyz,
                                                           g.bias.w), 0.0).rg;
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
        for (int j = 0; j < count; ++j) {
            sum[j] += weight * textureLod(uGiIrradiance, kin_gi_atlas(stored, ways[j], 8.0, g.counts.xyz, g.bias.w), 0.0).rgb;
        }
        total += weight;
    }
    for (int j = 0; j < 4; ++j) {
        light[j] = total > 0.0 ? sum[j] / total : vec3(0.0);
    }
    return total > 0.0;
}

// The light a surface at `p` facing `n` receives from the probes (irradiance over pi), read the ways `ways` (`count` of
// them, up to 4: `n` for its diffuse light), seen from `to_eye` (unit, toward the camera; zero where there is none):
// from the finest level whose probes surround it, blended into the next one up over the last probe cell of its
// window. Each level's: the eight probes round it, each weighted by how near it is (trilinear), by whether it is in
// front of the surface (a wrapped cosine) and by whether it can see the surface (Chebyshev's inequality on its
// distances that way: a wall between them keeps its light out). A probe asleep, in a wall or not yet traced (its
// data's w under 0.5) gives nothing; a level none of whose eight gives anything leaves it to the next. False where no
// level's probes have anything for `p`.
bool kin_gi_light(KinGiProbes g, vec3 p, vec3 n, vec3 to_eye, vec3 ways[4], int count, out vec3 light[4]) {
    light = vec3[4](vec3(0.0), vec3(0.0), vec3(0.0), vec3(0.0));
    const int levels = int(g.bias.w);
    const vec3 last = g.counts.xyz - 1.0;
    bool have = false;
    vec3 below[4] = light;
    float share = 1.0; // of the level below's light still to give
    for (int k = 0; k < levels; ++k) {
        const float spacing = g.level[k].w;
        const vec3 biased = p + (n * g.bias.x + to_eye * g.bias.y) * spacing;
        const vec3 grid = biased / spacing - g.level[k].xyz;
        if (any(lessThan(grid, vec3(0.0))) || any(greaterThan(grid, last))) {
            continue;
        }
        vec3 here[4];
        if (!kin_gi_level_light(g, k, p, biased, n, grid, ways, count, here)) {
            continue;
        }
        const vec3 edge = min(grid, last - grid);
        const float inside = clamp(min(min(edge.x, edge.y), edge.z), 0.0, 1.0);
        if (have) {
            // The level below faded into this one over its window's last cell.
            for (int j = 0; j < 4; ++j) {
                light[j] = mix(here[j], below[j], share);
            }
            return true;
        }
        if (inside >= 1.0) {
            light = here;
            return true;
        }
        // Over its window's last cell, into the next level up (none: its own all the way).
        have = true;
        below = here;
        share = inside;
    }
    light = below;
    return have;
}

// kin_gi_light read the one way `n`.
bool kin_gi_irradiance(KinGiProbes g, vec3 p, vec3 n, vec3 to_eye, out vec3 light) {
    vec3 got[4];
    const bool have = kin_gi_light(g, p, n, to_eye, vec3[4](n, n, n, n), 1, got);
    light = got[0];
    return have;
}
#endif
