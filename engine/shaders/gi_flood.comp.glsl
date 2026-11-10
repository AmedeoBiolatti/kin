#version 450 core
// kin's GI scene (docs/3d_plan.md, G5a, G5c): each voxel's distance to the nearest voxel a surface is in, kept to 8, by
// a jump flood (Rong and Tan, 2006) over a box of one level's window (all of it, or what changed with a margin). A
// dispatch a round at `p.mode.y` voxels: each voxel takes the nearest of the seeds it and its 26 neighbours that far
// away hold (in the box). The first round (`p.mode.x` bit 0) reads the seeds from the voxels (one a surface is in is
// its own nearest); the last (bit 1) writes the distances of the voxels in the write box (the box less a block's
// margin, whose seeds may be outside it). A seed is its window coordinates in ten bits each. Ties go to the lesser
// seed, so the answer does not depend on the order the GPU runs in.
layout(local_size_x = 64) in;

layout(set = 0, binding = 0) uniform sampler3D uGiAlbedo; // a: 1 where a surface is
layout(std430, set = 0, binding = 1) readonly buffer Source {
    uint source[]; // the last round's seeds; ~0: none (not read by the first)
};
layout(set = 1, binding = 0, r16f) uniform writeonly image3D uDistance;
layout(std430, set = 1, binding = 1) writeonly buffer Target {
    uint target[];
};
layout(std140, set = 2, binding = 0) uniform Params {
    uvec4 size;   // xyz: voxels a window; w: the level
    ivec4 window; // xyz: the window's first voxel (in the level's voxels)
    ivec4 box;    // xyz: the box's first voxel (window coordinates)
    ivec4 extent; // xyz: its voxels a side
    ivec4 write;  // xyz: the write box's first voxel, w: 0
    ivec4 write_extent;
    ivec4 mode;   // x: bit 0 the first round, bit 1 the last; y: the round's reach
} p;

const uint none = 0xffffffffu;
const float most = 8.0;

#include "kin_gi_address.inc.glsl"

uint pack(ivec3 v) {
    return uint(v.x) | (uint(v.y) << 10u) | (uint(v.z) << 20u);
}
ivec3 unpack(uint s) {
    return ivec3(s & 1023u, (s >> 10u) & 1023u, s >> 20u);
}
float squared(ivec3 a, ivec3 b) {
    const vec3 d = vec3(a - b);
    return dot(d, d);
}
// The voxel's own seed: itself where a surface is in it.
uint seed_at(ivec3 stored, ivec3 local) {
    return texelFetch(uGiAlbedo, stored, 0).a > 0.0 ? pack(local) : none;
}

uint index_of(ivec3 local, out ivec3 stored) {
    const ivec3 size = ivec3(p.size.xyz);
    const ivec3 st = kin_gi_wrap(local + p.window.xyz, size);
    stored = ivec3(st.xy, st.z + int(p.size.w) * size.z);
    return uint(stored.x) + p.size.x * (uint(stored.y) + p.size.y * uint(stored.z));
}

void main() {
    const uint i = (gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x) * 64u + gl_LocalInvocationIndex;
    const ivec3 extent = p.extent.xyz;
    if (i >= uint(extent.x * extent.y * extent.z)) {
        return;
    }
    const ivec3 local =
        p.box.xyz + ivec3(int(i) % extent.x, (int(i) / extent.x) % extent.y, int(i) / (extent.x * extent.y));
    ivec3 stored;
    const uint at = index_of(local, stored);
    const bool first = (p.mode.x & 1) != 0;
    uint best = first ? seed_at(stored, local) : source[at];
    float best_d = best != none ? squared(local, unpack(best)) : 1e30;
    const int reach = p.mode.y;
    for (int z = -1; z <= 1; ++z) {
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                const ivec3 q = local + ivec3(x, y, z) * reach;
                if ((x | y | z) == 0 || any(lessThan(q, p.box.xyz)) || any(greaterThanEqual(q, p.box.xyz + extent))) {
                    continue;
                }
                ivec3 there;
                const uint qi = index_of(q, there);
                const uint s = first ? seed_at(there, q) : source[qi];
                if (s == none) {
                    continue;
                }
                const float d = squared(local, unpack(s));
                if (d < best_d || (d == best_d && s < best)) {
                    best = s;
                    best_d = d;
                }
            }
        }
    }
    target[at] = best;
    if ((p.mode.x & 2) != 0 && all(greaterThanEqual(local, p.write.xyz)) &&
        all(lessThan(local, p.write.xyz + p.write_extent.xyz))) {
        imageStore(uDistance, stored, vec4(min(best != none ? sqrt(best_d) : most, most)));
    }
}
