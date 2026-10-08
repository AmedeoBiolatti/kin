#version 450 core
// kin's 3D ambient occlusion pre-pass: mesh.vert's placing, and the world
// normal (and, for cut-outs, the UV and alpha prepass_mask.frag tests). No
// colours or world position: only depth and a normal come out of it.
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aColor;

// GpuInstance3D, as mesh.vert reads it.
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

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec2 vUV;
layout(location = 2) out float vAlpha; // the instance's and the vertex's

void main() {
    const Instance instance = instances[draw.first.x + uint(gl_InstanceIndex)];
    gl_Position = draw.view_projection * (instance.model * vec4(aPos, 1.0));
    vNormal = mat3(instance.normal0.xyz, instance.normal1.xyz, instance.normal2.xyz) * aNormal;
    vUV = aUV;
    vAlpha = instance.color.a * aColor.a;
}
