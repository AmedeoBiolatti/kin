#version 450 core
// prepass.vert for a skinned copy (ambient occlusion, contact shadows): the vertex moved by its joints first, the normal
// turned by them as mesh_skinned.vert turns it. Binding 1 and 2 are the frame's joint matrices and each copy's first.
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aColor;

#include "kin_instance.inc.glsl"

#define KIN_SKIN_BINDING 1
#include "kin_skin.inc.glsl"

layout(set = 1, binding = 0) uniform Draw {
    mat4 view_projection;
    uvec4 first; // x: this draw's first instance
} draw;

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec2 vUV;
layout(location = 2) out float vAlpha; // the instance's and the vertex's
layout(location = 3) out vec3 vWorld;

void main() {
    const uint slot = draw.first.x + uint(gl_InstanceIndex);
    const Instance instance = instances[slot];
    const mat4 skin = skin_matrix(slot);
    const vec4 world = instance.model * (skin * vec4(aPos, 1.0));
    vWorld = world.xyz;
    gl_Position = draw.view_projection * world;
    vNormal = mat3(instance.normal0.xyz, instance.normal1.xyz, instance.normal2.xyz) * normalize(mat3(skin) * aNormal);
    vUV = aUV;
    vAlpha = instance.color.a * aColor.a;
}
