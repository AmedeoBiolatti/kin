#version 450 core
// kin's GI scene (docs/3d_plan.md, G5a): each voxel's distance to the nearest voxel a surface is in, by a jump flood
// (Rong and Tan, 2006). Three steps, one shader: `p.step.y` 0 seeds the flood (a voxel with an owner is its own
// nearest), 1 is a round at `p.step.x` voxels (each voxel takes the nearest of the seeds its 26 neighbours that far
// away hold), 2 writes the distances. A voxel's seed is its coordinates in ten bits each. Ties go to the lesser seed, so
// the answer does not depend on the order the GPU runs in.
layout(local_size_x = 64) in;

layout(std430, set = 0, binding = 0) readonly buffer Source {
    uint source[]; // the owners (step 0), else the last round's seeds; ~0: none
};
layout(set = 1, binding = 0, r16f) uniform writeonly image3D uDistance;
layout(std430, set = 1, binding = 1) writeonly buffer Target {
    uint target[];
};
layout(std140, set = 2, binding = 0) uniform Params {
    uvec4 size; // xyz: voxels a side
    ivec4 step; // x: the round's reach in voxels, y: 0 seed, 1 a round, 2 the distances
} p;

const uint none = 0xffffffffu;

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

void main() {
    const uint i = (gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x) * 64u + gl_LocalInvocationIndex;
    const ivec3 size = ivec3(p.size.xyz);
    if (i >= uint(size.x * size.y * size.z)) {
        return;
    }
    const ivec3 voxel = ivec3(int(i) % size.x, (int(i) / size.x) % size.y, int(i) / (size.x * size.y));
    if (p.step.y == 0) {
        target[i] = source[i] != none ? pack(voxel) : none;
        return;
    }
    uint best = source[i];
    float best_d = best != none ? squared(voxel, unpack(best)) : 1e30;
    if (p.step.y == 2) {
        imageStore(uDistance, voxel, vec4(best != none ? sqrt(best_d) : 4096.0));
        return;
    }
    for (int z = -1; z <= 1; ++z) {
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                const ivec3 q = voxel + ivec3(x, y, z) * p.step.x;
                if ((x | y | z) == 0 || any(lessThan(q, ivec3(0))) || any(greaterThanEqual(q, size))) {
                    continue;
                }
                const uint s = source[uint(q.x + size.x * (q.y + size.y * q.z))];
                if (s == none) {
                    continue;
                }
                const float d = squared(voxel, unpack(s));
                if (d < best_d || (d == best_d && s < best)) {
                    best = s;
                    best_d = d;
                }
            }
        }
    }
    target[i] = best;
}
