#version 450 core
// kin's GI scene (docs/3d_plan.md, G5a): every voxel emptied before a build (no owner, no surface).
layout(local_size_x = 64) in;

layout(set = 1, binding = 0, rgba8) uniform writeonly image3D uAlbedo;
layout(set = 1, binding = 1, rgba8) uniform writeonly image3D uNormal;
layout(set = 1, binding = 2, rgba16f) uniform writeonly image3D uEmission;
layout(std430, set = 1, binding = 3) writeonly buffer Owners {
    uint owners[];
};
layout(std140, set = 2, binding = 0) uniform Params {
    uvec4 size; // xyz: voxels a side
} p;

void main() {
    const uint i = (gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x) * 64u + gl_LocalInvocationIndex;
    const uint count = p.size.x * p.size.y * p.size.z;
    if (i >= count) {
        return;
    }
    const ivec3 voxel = ivec3(i % p.size.x, (i / p.size.x) % p.size.y, i / (p.size.x * p.size.y));
    owners[i] = 0xffffffffu;
    imageStore(uAlbedo, voxel, vec4(0.0));
    imageStore(uNormal, voxel, vec4(0.0));
    imageStore(uEmission, voxel, vec4(0.0));
}
