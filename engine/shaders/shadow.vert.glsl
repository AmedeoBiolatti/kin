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

layout(set = 1, binding = 0) uniform Draw {
    mat4 view_projection;
    uvec4 first; // x: this draw's first instance
} draw;

void main() {
    const mat4 model = instances[draw.first.x + uint(gl_InstanceIndex)].model;
    gl_Position = draw.view_projection * (model * vec4(aPos, 1.0));
}
