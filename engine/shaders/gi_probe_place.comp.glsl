#version 450 core
// kin's GI probes (docs/3d_plan.md, G5b, G5c): where the probes of a list stand, after the GI scene round them changed.
// A probe more than `misc.y` voxels from every surface sleeps (no surface ever reads it: a surface reads the eight
// probes of its cell, all nearer it than that); one nearer than 1.8 voxels to a surface moves, within two fifths
// of the spacing on each axis, to the nearest candidate with that clearance, else the roomiest (nearest on ties).
// In a surface cell, a single-sided face permits only moves toward its front: an empty cell behind it can be inside
// a closed mesh. Either side of a double-sided face is valid. Placed again because geometry changed, a probe stays
// where it stood while that still has room: moved, it would light the place with what it saw from elsewhere until
// its next updates. One its rays found inside something (gi_probe_update.comp: more than a quarter of them met
// backs) stays so until they say otherwise: placed
// again because something changed nearby, it would give its light for the renders until its next update, every time.
// A probe new to its window (the list's bit 23) has its tiles emptied first: it gives nothing until it has been
// traced.
layout(local_size_x = 64) in;

layout(set = 0, binding = 0) uniform sampler3D uGiDistance;
layout(set = 0, binding = 1) uniform sampler3D uNormal;
layout(std430, set = 0, binding = 2) readonly buffer List {
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

// The distance field describes surface cells, including those of closed meshes; an empty cell alone does not
// establish that a relocation destination is outside a solid.
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
    const ivec3 world = first + kin_gi_wrap(st - first, counts);
    const float cell = p.scene.level[k].w, spacing = p.probes.level[k].w / cell; // in voxels
    const vec3 base = vec3(world) * spacing - p.scene.level[k].xyz;
    const float here = room(k, base);
    const vec4 before = imageLoad(uProbeData, stored);
    const vec3 kept = before.xyz / cell; // where it stood (voxels from its grid point)
    vec3 best = vec3(0.0);
    float state = 1.0;
    if (here > p.misc.y) {
        state = 0.0;
    } else if (!fresh && before.w != 0.0 && room(k, base + kept) >= 1.8) {
        best = kept; // it stays: where it stands still has room
    } else if (here < 1.8) {
        const vec4 surface = here == 0.0
                                 ? texelFetch(uNormal, kin_gi_stored(p.scene, k, ivec3(floor(base))), 0)
                                 : vec4(0.0, 0.0, 0.0, 1.0);
        const vec3 normal = surface.xyz * 2.0 - 1.0;
        float best_room = here, best_length = 0.0;
        for (int z = -2; z <= 2; ++z) {
            for (int y = -2; y <= 2; ++y) {
                for (int x = -2; x <= 2; ++x) {
                    const vec3 offset = vec3(x, y, z) * (0.2 * spacing);
                    const float len = length(offset);
                    // A nearest empty cell can still be inside a closed mesh. Move toward a single-sided face's
                    // front; the dot-product tolerance covers the normal's 8-bit quantization near tangency.
                    if (surface.a <= 0.75 && dot(offset, normal) <= len * (2.0 / 255.0)) {
                        continue;
                    }
                    const float r = room(k, base + offset);
                    const bool enough = r >= 1.8, had = best_room >= 1.8;
                    if ((enough && (!had || len < best_length)) ||
                        (!enough && !had && (r > best_room || (r == best_room && len < best_length)))) {
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
    if (!fresh && state > 0.0 && before.w < 0.0) {
        state = -1.0;
    }
    imageStore(uProbeData, stored, vec4(best * cell, state));
}
