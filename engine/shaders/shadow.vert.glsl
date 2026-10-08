#version 450 core
// kin's 3D shadow pass: mesh.vert's placing, and nothing else (no normals,
// colours or varyings: depth only).
layout(location = 0) in vec3 aPos;

// GpuInstance3D, as mesh.vert reads it; only the model matrix is used here.
struct Instance {
    mat4 model;
    vec4 normal0;
    vec4 normal1;
    vec4 normal2;
    vec4 color;
};
layout(std430, set = 0, binding = 0) readonly buffer Instances {
    Instance instances[];
};

// With cascades, a cascade's copies are a list of indices into the instances
// (the caster test is per cascade); without, they are one run from first.x.
layout(std430, set = 0, binding = 1) readonly buffer Casters {
    uint casters[];
};

layout(set = 1, binding = 0) uniform Draw {
    mat4 view_projection;
    uvec4 first; // x: this draw's first instance; y: where its list starts, plus one (0: no list)
} draw;

uint instance_index() {
    return draw.first.y != 0u ? casters[draw.first.y - 1u + uint(gl_InstanceIndex)] : draw.first.x + uint(gl_InstanceIndex);
}

void main() {
    const mat4 model = instances[instance_index()].model;
    gl_Position = draw.view_projection * (model * vec4(aPos, 1.0));
}
