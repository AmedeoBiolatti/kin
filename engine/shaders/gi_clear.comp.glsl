#version 450 core
// kin's GI scene (docs/3d_plan.md, G5a, G5c): the voxels of the blocks to be made again emptied (no owner, no faces, no
// surface): a block of 8 x 8 x 8 a list entry, a thread a voxel.
layout(local_size_x = 64) in;

layout(std430, set = 0, binding = 0) readonly buffer Blocks {
    uint blocks[]; // a stored block: x, y and z (8 bits each), and its level (the top 8)
};
layout(set = 1, binding = 0, rgba8) uniform writeonly image3D uAlbedo;
layout(set = 1, binding = 1, rgba8) uniform writeonly image3D uNormal;
layout(set = 1, binding = 2, rgba16f) uniform writeonly image3D uEmission;
layout(std430, set = 1, binding = 3) writeonly buffer Owners {
    uint owners[];
};
layout(std430, set = 1, binding = 4) writeonly buffer Sides {
    uint sides[];
};
layout(std140, set = 2, binding = 0) uniform Params {
    uvec4 size; // xyz: voxels a window; w: blocks in the list
} p;

void main() {
    const uint i = (gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x) * 64u + gl_LocalInvocationIndex;
    const uint entry = i >> 9u;
    if (entry >= p.size.w) {
        return;
    }
    const uint block = blocks[entry], v = i & 511u;
    const ivec3 voxel = ivec3(block & 255u, (block >> 8u) & 255u, (block >> 16u) & 255u) * 8 +
                        ivec3(v & 7u, (v >> 3u) & 7u, v >> 6u);
    const ivec3 stored = ivec3(voxel.xy, voxel.z + int(block >> 24u) * int(p.size.z));
    const uint at = uint(stored.x) + p.size.x * (uint(stored.y) + p.size.y * uint(stored.z));
    owners[at] = 0xffffffffu;
    sides[at] = 0u;
    imageStore(uAlbedo, stored, vec4(0.0));
    imageStore(uNormal, stored, vec4(0.0));
    imageStore(uEmission, stored, vec4(0.0));
}
