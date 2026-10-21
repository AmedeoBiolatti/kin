#version 450 core
// prepass_velocity.vert for a skinned copy: the vertex posed by its joints now and by its joints of the frame before
// (binding 4 and 5, matched to the copy as its model matrix is), each placed by its own model matrix and camera.
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aColor;

#include "kin_instance.inc.glsl"
// Last frame's model matrix of the copy at the same place in Instances (its [0][3] is 1 for a copy with no past).
layout(std430, set = 0, binding = 1) readonly buffer Previous {
    mat4 previous[];
};

#define KIN_SKIN_BINDING 2
#define KIN_SKIN_PREVIOUS
#include "kin_skin.inc.glsl"

layout(set = 1, binding = 0) uniform Draw {
    mat4 view_projection; // jittered: where the pixel is drawn
    uvec4 first;          // x: this draw's first instance
    mat4 current;         // without the jitter, this frame's camera
    mat4 prior;           // and the last frame's
}
draw;

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec2 vUV;
layout(location = 2) out float vAlpha;
layout(location = 3) out vec3 vCurrent;  // clip x, y, w this frame
layout(location = 4) out vec3 vPrevious; // and the last
layout(location = 5) flat out float vFresh;
layout(location = 6) out vec3 vWorld;
layout(location = 7) out float vIndependent;

void main() {
    const uint at = draw.first.x + uint(gl_InstanceIndex);
    const Instance instance = instances[at];
    mat4 before = previous[at];
    vFresh = before[0][3];
    before[0][3] = 0.0;
    const mat4 skin = skin_matrix(at);
    const vec4 world = instance.model * (skin * vec4(aPos, 1.0));
    vWorld = world.xyz;
    gl_Position = draw.view_projection * world;
    vCurrent = (draw.current * world).xyw;
    const vec4 previous_world = before * (skin_matrix_previous(at, skin) * vec4(aPos, 1.0));
    vPrevious = (draw.prior * previous_world).xyw;
    vIndependent = length(world.xyz - previous_world.xyz);
    vNormal = mat3(instance.normal0.xyz, instance.normal1.xyz, instance.normal2.xyz) * normalize(mat3(skin) * aNormal);
    vUV = aUV;
    vAlpha = instance.color.a * aColor.a;
}
