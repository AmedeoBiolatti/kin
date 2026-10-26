#version 450 core
// kin's GI probes (docs/3d_plan.md, G5b, G5c): where the probes of a list stand, after the GI scene round them changed.
// A probe more than `misc.y` voxels from every surface sleeps (no surface ever reads it: a surface reads the eight
// probes of its cell, all nearer it than that); one nearer than 1.8 voxels to a surface moves, within two fifths of
// the spacing, to the place of the most room about it (the nearest of those with 1.8, else the roomiest; ties to the
// first tried), and one with no room anywhere there stays, inside the wall, giving nothing. A probe new to its window
// (the list's bit 23) has its tiles emptied first: it gives nothing until it has been traced.
layout(local_size_x = 64) in;

layout(set = 0, binding = 0) uniform sampler3D uGiDistance;
layout(std430, set = 0, binding = 1) readonly buffer List {
    uint list[]; // a probe: its stored index in its level, bit 23 new, its level in the top 8
};
layout(set = 1, binding = 0, rgba16f) uniform image3D uProbeData; // xyz: the offset (world), w: 1 awake and traced,
                                                                  // 0.25 awake, 0 asleep, -1 in a wall
layout(set = 1, binding = 1, rgba16f) uniform image3D uIrradiance; // a: 1 once traced
layout(set = 1, binding = 2, rg16f) uniform writeonly image3D uMoments;

#include "kin_gi.inc.glsl"

layout(std140, set = 2, binding = 0) uniform Params {
    KinGiScene scene;
    KinGiProbes probes;
    uvec4 count; // x: the list's length
    vec4 misc;   // y: the distance past which a probe sleeps (voxels)
} p;

float room(int k, vec3 q) {
    const ivec3 local = ivec3(floor(q));
    if (any(lessThan(local, ivec3(0))) || any(greaterThanEqual(local, ivec3(p.scene.size.xyz)))) {
        return 8.0;
    }
    return texelFetch(uGiDistance, kin_gi_stored(p.scene, k, local), 0).r;
}

void main() {
    const uint i = (gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x) * 64u + gl_LocalInvocationIndex;
    if (i >= p.count.x) {
        return;
    }
    const uint entry = list[i];
    const int k = int(entry >> 24u);
    const bool fresh = (entry & (1u << 23u)) != 0u;
    const int index = int(entry & 0x7fffffu);
    const ivec3 counts = ivec3(p.probes.counts.xyz);
    const ivec3 st = ivec3(index % counts.x, (index / counts.x) % counts.y, index / (counts.x * counts.y));
    const ivec3 stored = ivec3(st.xy, st.z + k * counts.z);
    const ivec3 first = ivec3(p.probes.level[k].xyz);
    const ivec3 world = first + (st - first + counts * 4096) % counts;
    const float cell = p.scene.level[k].w, spacing = p.probes.level[k].w / cell; // in voxels
    const vec3 base = vec3(world) * spacing - p.scene.level[k].xyz;
    const float here = room(k, base);
    vec3 best = vec3(0.0);
    float state = 1.0;
    if (here > p.misc.y) {
        state = 0.0;
    } else if (here < 1.8) {
        float best_room = here, best_length = 0.0;
        for (int z = -2; z <= 2; ++z) {
            for (int y = -2; y <= 2; ++y) {
                for (int x = -2; x <= 2; ++x) {
                    const vec3 offset = vec3(x, y, z) * (0.2 * spacing);
                    const float r = room(k, base + offset), len = length(offset);
                    const bool enough = r >= 1.8, had = best_room >= 1.8;
                    if ((enough && (!had || len < best_length)) || (!enough && !had && r > best_room)) {
                        best = offset;
                        best_room = r;
                        best_length = len;
                    }
                }
            }
        }
        state = best_room >= 0.5 ? 1.0 : -1.0;
    }
    bool traced = !fresh && imageLoad(uIrradiance, ivec3(stored.xy * 8 + 1, stored.z)).a > 0.5;
    if (fresh) {
        for (int t = 0; t < 64; ++t) {
            imageStore(uIrradiance, ivec3(stored.xy * 8 + ivec2(t % 8, t / 8), stored.z), vec4(0.0));
        }
        for (int t = 0; t < 256; ++t) {
            imageStore(uMoments, ivec3(stored.xy * 16 + ivec2(t % 16, t / 16), stored.z), vec4(0.0));
        }
    }
    if (state == 1.0 && !traced) {
        state = 0.25;
    }
    imageStore(uProbeData, stored, vec4(best * cell, state));
}
