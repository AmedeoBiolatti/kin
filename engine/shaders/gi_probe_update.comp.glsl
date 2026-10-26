#version 450 core
// kin's GI probes (docs/3d_plan.md, G5b): each probe of the batch takes its rays into its tiles, a workgroup a probe
// and a thread a texel inside a tile (the borders copied after). Irradiance (8 x 8): each texel's direction, the
// cosine-weighted mean of the rays' light (the light a surface facing that way receives, over pi). Moments (16 x 16):
// the mean of the rays' distances and their squares, weighted by the cosine to the 50th (each texel sees a narrow
// cone). Each is blended with what the texel
// held, keeping `hysteresis` of it (none in a probe's first update). The border texels hold their octahedral
// neighbours, so that filtering across a tile's edge reads the other side of the sphere. A probe whose rays meet
// more than a quarter backs is inside something: it gives nothing until that changes.
layout(local_size_x = 16, local_size_y = 16) in;

layout(std430, set = 0, binding = 0) readonly buffer Rays {
    vec4 rays[];
};
layout(std430, set = 0, binding = 1) readonly buffer List {
    uint list[]; // the batch: a probe's stored index in its level, its level in the top 8
};
layout(set = 1, binding = 0, rgba16f) uniform image3D uIrradiance; // a: 1 once the probe has had an update
layout(set = 1, binding = 1, rg16f) uniform image3D uMoments;
layout(set = 1, binding = 2, rgba16f) uniform image3D uProbeData;

#include "kin_gi.inc.glsl"

layout(std140, set = 2, binding = 0) uniform Params {
    KinGiProbes probes;
    uvec4 batch; // y: the batch's probes, z: rays a probe
    mat4 rotation;
    vec4 misc;   // x: hysteresis, y: the farthest distance kept (in spacings)
} p;

shared vec4 found[256];
shared vec3 ways[256];
shared uint backs;
shared uint fresh;
shared vec2 moments_inside[196];   // the moments' tile inside its border, after this update
shared vec4 irradiance_inside[36]; // the irradiance's

vec3 fibonacci(uint i, uint n) {
    const float phi = 2.399963229728653 * float(i);
    const float z = 1.0 - (2.0 * float(i) + 1.0) / float(n);
    const float r = sqrt(max(0.0, 1.0 - z * z));
    return vec3(cos(phi) * r, sin(phi) * r, z);
}

// The texel of an n x n tile (a map inside a border of one) whose value texel `t` holds: itself inside, its octahedral
// neighbour on the border.
ivec2 source_of(ivec2 t, int n) {
    const int last = n - 1;
    const bool left = t.x == 0, right = t.x == last, top = t.y == 0, bottom = t.y == last;
    if ((left || right) && (top || bottom)) {
        return ivec2(left ? n - 2 : 1, top ? n - 2 : 1);
    }
    if (top) {
        return ivec2(last - t.x, 1);
    }
    if (bottom) {
        return ivec2(last - t.x, n - 2);
    }
    if (left) {
        return ivec2(1, last - t.y);
    }
    if (right) {
        return ivec2(n - 2, last - t.y);
    }
    return t;
}

// Border texel `i` of an n x n tile (4 (n - 1) of them): the top row, the bottom row, then the left and right columns
// between them.
ivec2 border_texel(int i, int n) {
    if (i < n) {
        return ivec2(i, 0);
    }
    if (i < 2 * n) {
        return ivec2(i - n, n - 1);
    }
    const int j = i - 2 * n;
    return j < n - 2 ? ivec2(0, 1 + j) : ivec2(n - 1, 1 + j - (n - 2));
}

vec3 direction_of(ivec2 texel, int n) {
    return kin_gi_direction((vec2(texel) - 0.5) / float(n - 2) * 2.0 - 1.0);
}

void main() {
    const uint b = gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x;
    if (b >= p.batch.y) {
        return;
    }
    const uint entry = list[b];
    const int k = int(entry >> 24u), index = int(entry & 0x7fffffu);
    const ivec3 counts = ivec3(p.probes.counts.xyz);
    const ivec3 st = ivec3(index % counts.x, (index / counts.x) % counts.y, index / (counts.x * counts.y));
    const ivec3 probe = ivec3(st.xy, st.z + k * counts.z);
    const float farthest = p.misc.y * p.probes.level[k].w;
    const vec4 data = imageLoad(uProbeData, probe);
    if (data.w == 0.0) {
        return; // asleep (the whole workgroup)
    }
    const uint local = gl_LocalInvocationIndex;
    const uint n = p.batch.z;
    if (local == 0u) {
        backs = 0u;
        fresh = imageLoad(uIrradiance, ivec3(probe.xy * 8 + 1, probe.z)).a > 0.5 ? 0u : 1u;
    }
    barrier();
    for (uint r = local; r < n; r += 256u) {
        const vec4 v = rays[b * n + r];
        found[r] = v;
        ways[r] = normalize(mat3(p.rotation) * fibonacci(r, n));
        if (v.w < 0.0) {
            atomicAdd(backs, 1u);
        }
    }
    memoryBarrierShared();
    barrier();
    const float keep = fresh != 0u ? 0.0 : p.misc.x;

    // Each thread one texel inside a tile: the moments' 14 x 14 first, then the irradiance's 6 x 6, each one walk over
    // the rays; then the borders copy them.
    if (local < 196u) {
        const ivec2 t = ivec2(1 + int(local % 14u), 1 + int(local / 14u));
        const vec3 dir = direction_of(t, 16);
        vec2 sum = vec2(0.0);
        float total = 0.0;
        for (uint r = 0u; r < n; ++r) {
            // The cosine to the 50th, as e^(50 (c - 1)) (the same near 1, where it is not nothing).
            const float w = exp2(72.13475 * (dot(dir, ways[r]) - 1.0));
            const float d = min(abs(found[r].w), farthest);
            sum += w * vec2(d, d * d);
            total += w;
        }
        const ivec3 at = ivec3(probe.xy * 16 + t, probe.z);
        vec2 value = imageLoad(uMoments, at).rg;
        if (total > 1e-6) {
            value = mix(sum / total, value, keep);
            imageStore(uMoments, at, vec4(value, 0.0, 0.0));
        }
        moments_inside[local] = value;
    } else if (local < 232u) {
        const uint i = local - 196u;
        const ivec2 t = ivec2(1 + int(i % 6u), 1 + int(i / 6u));
        const vec3 dir = direction_of(t, 8);
        vec3 sum = vec3(0.0);
        float total = 0.0;
        for (uint r = 0u; r < n; ++r) {
            const float w = max(0.0, dot(dir, ways[r]));
            sum += w * found[r].rgb;
            total += w;
        }
        const ivec3 at = ivec3(probe.xy * 8 + t, probe.z);
        vec4 value = imageLoad(uIrradiance, at);
        if (total > 1e-6) {
            value = vec4(mix(sum / total, value.rgb, keep), 1.0);
            imageStore(uIrradiance, at, value);
        }
        irradiance_inside[i] = value;
    }
    memoryBarrierShared();
    barrier();
    // The borders: 60 of the moments' tile, 28 of the irradiance's.
    if (local < 60u) {
        const ivec2 t = border_texel(int(local), 16);
        const ivec2 from = source_of(t, 16) - 1;
        imageStore(uMoments, ivec3(probe.xy * 16 + t, probe.z), vec4(moments_inside[from.y * 14 + from.x], 0.0, 0.0));
    } else if (local < 88u) {
        const ivec2 t = border_texel(int(local - 60u), 8);
        const ivec2 from = source_of(t, 8) - 1;
        imageStore(uIrradiance, ivec3(probe.xy * 8 + t, probe.z), irradiance_inside[from.y * 6 + from.x]);
    }
    if (local == 0u) {
        imageStore(uProbeData, probe, vec4(data.xyz, float(backs) > 0.25 * float(n) ? -1.0 : 1.0));
    }
}
