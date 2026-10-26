#version 450 core
// kin's GI probes (docs/3d_plan.md, G5b): where each probe stands, after each build of the GI scene. A probe whose
// place is more than `sleep` voxels from every surface sleeps (no surface ever reads it: a surface reads the eight
// probes of its cell, all nearer it than that); one nearer than 1.8 voxels to a surface moves, within two fifths of
// the spacing, to the place of the most room about it (the nearest of those with 1.8, else the roomiest; ties to the
// first tried), and one with no room anywhere there stays, inside the wall, giving nothing. With `clear`, the probes'
// tiles are emptied first (a new grid: no light yet).
layout(local_size_x = 64) in;

layout(set = 0, binding = 0) uniform sampler3D uGiDistance; // filtered: the distance between voxels' centres
layout(set = 1, binding = 0, rgba16f) uniform writeonly image3D uProbeData; // xyz: the offset (world), w: 1 awake, 0 asleep, -1 in a wall
layout(set = 1, binding = 1, rgba16f) uniform writeonly image3D uIrradiance;
layout(set = 1, binding = 2, rg16f) uniform writeonly image3D uMoments;

#include "kin_gi.inc.glsl"

layout(std140, set = 2, binding = 0) uniform Params {
    vec4 origin; // xyz: voxel (0, 0, 0)'s low corner (world); w: voxels a unit
    vec4 size;   // xyz: voxels a side
    KinGiProbes probes;
    vec4 misc;   // x: 1 to empty the tiles, y: the distance past which a probe sleeps (voxels)
} p;

float room(vec3 q) {
    return textureLod(uGiDistance, q / p.size.xyz, 0.0).r;
}

void main() {
    const uint i = (gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x) * 64u + gl_LocalInvocationIndex;
    const ivec3 counts = ivec3(p.probes.counts.xyz);
    if (i >= uint(counts.x * counts.y * counts.z)) {
        return;
    }
    const ivec3 probe = ivec3(int(i) % counts.x, (int(i) / counts.x) % counts.y, int(i) / (counts.x * counts.y));
    const float spacing = p.probes.origin.w * p.origin.w; // voxels
    const vec3 base = (p.probes.origin.xyz + vec3(probe) * p.probes.origin.w - p.origin.xyz) * p.origin.w;
    const float here = room(base);
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
                    const float r = room(base + offset), len = length(offset);
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
    imageStore(uProbeData, probe, vec4(best / p.origin.w, state));
    if (p.misc.x > 0.5) {
        for (int t = 0; t < 64; ++t) {
            imageStore(uIrradiance, ivec3(probe.xy * 8 + ivec2(t % 8, t / 8), probe.z), vec4(0.0));
        }
        for (int t = 0; t < 256; ++t) {
            imageStore(uMoments, ivec3(probe.xy * 16 + ivec2(t % 16, t / 16), probe.z), vec4(0.0));
        }
    }
}
