#version 450 core
// Morph targets on the GPU (Renderer3D::set_morph_weights): each vertex of a mesh made with morph targets is its rest
// position and normal plus each target's offsets times the target's weight, written straight into the mesh's vertex
// buffer (GpuMeshVertex: 9 words, position, normal, UV, then the colour's four bytes, copied as they are). Targets of
// weight 0 cost a test. The normal is made unit again where the targets move normals (else it is the rest one).
layout(local_size_x = 64) in;

layout(std430, set = 0, binding = 0) readonly buffer Base {
    uint base[]; // the mesh at rest, 9 words a vertex
};
layout(std430, set = 0, binding = 1) readonly buffer Deltas {
    float deltas[]; // target by target, 6 floats a vertex: position offset, normal offset
};
layout(std430, set = 1, binding = 0) writeonly buffer Vertices {
    uint vertices[];
};
layout(std140, set = 2, binding = 0) uniform Params {
    uvec4 counts;      // x: vertices, y: targets, z: 1 when targets move normals
    vec4 weights[64];  // four a vec4, by target
} P;

void main() {
    const uint v = gl_GlobalInvocationID.x;
    if (v >= P.counts.x)
        return;
    const uint at = v * 9u;
    vec3 position = vec3(uintBitsToFloat(base[at]), uintBitsToFloat(base[at + 1u]), uintBitsToFloat(base[at + 2u]));
    const vec3 rest = vec3(uintBitsToFloat(base[at + 3u]), uintBitsToFloat(base[at + 4u]), uintBitsToFloat(base[at + 5u]));
    vec3 normal = rest;
    for (uint t = 0u; t < P.counts.y; ++t) {
        const float w = P.weights[t >> 2u][t & 3u];
        if (w == 0.0)
            continue;
        const uint d = (t * P.counts.x + v) * 6u;
        position += w * vec3(deltas[d], deltas[d + 1u], deltas[d + 2u]);
        normal += w * vec3(deltas[d + 3u], deltas[d + 4u], deltas[d + 5u]);
    }
    if (P.counts.z != 0u) {
        const float l = length(normal);
        normal = l > 1e-12 ? normal / l : rest;
    }
    vertices[at] = floatBitsToUint(position.x);
    vertices[at + 1u] = floatBitsToUint(position.y);
    vertices[at + 2u] = floatBitsToUint(position.z);
    vertices[at + 3u] = floatBitsToUint(normal.x);
    vertices[at + 4u] = floatBitsToUint(normal.y);
    vertices[at + 5u] = floatBitsToUint(normal.z);
    vertices[at + 6u] = base[at + 6u];
    vertices[at + 7u] = base[at + 7u];
    vertices[at + 8u] = base[at + 8u];
}
