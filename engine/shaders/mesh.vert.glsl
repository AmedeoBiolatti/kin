#version 450 core
// kin's 3D meshes (kin/render3d): one draw is many instances of a mesh, each
// placed by its own matrix, read from a storage buffer.
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aColor; // sRGB

#include "kin_instance.inc.glsl"

layout(set = 1, binding = 0) uniform Draw {
    mat4 view_projection;
    uvec4 first; // x: this draw's first instance
} draw;

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec4 vColor;
layout(location = 2) out vec2 vUV;
layout(location = 3) out vec3 vWorld;
layout(location = 4) flat out uint vId; // the copy's pick id, plus one (0: none)

vec3 decode(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}

void main() {
    // gl_InstanceIndex counts from 0: SDL_GPU's first_instance is left at 0
    // (backends disagree whether it adds to it), so the draw says where it starts.
    Instance instance = instances[draw.first.x + uint(gl_InstanceIndex)];
    const vec4 world = instance.model * vec4(aPos, 1.0);
    vWorld = world.xyz;
    gl_Position = draw.view_projection * world;
    vNormal = mat3(instance.normal0.xyz, instance.normal1.xyz, instance.normal2.xyz) * aNormal;
    vColor = instance.color * vec4(decode(aColor.rgb), aColor.a);
    vUV = aUV;
    vId = floatBitsToUint(instance.normal0.w);
}
