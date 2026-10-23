#version 450 core
// The pre-pass when temporal anti-aliasing is on (docs/3d_plan.md, G2d):
// prepass.vert's placing and normal, and how far the point moved on the
// screen since the last frame. Each copy's matrix of the frame before is
// read from a buffer of its own, indexed as the instances are (the 128 B
// instance is not touched); the camera's two view-projections come in the
// uniforms, without the jitter. The position written is the jittered one,
// as the main pass draws it.
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aColor;

#include "kin_instance.inc.glsl"
// Last frame's model matrix of the copy at the same place in Instances. Its
// [0][3] (a zero in any placing) is 1 for a copy with no past: new, or
// matched to nothing believable.
layout(std430, set = 0, binding = 1) readonly buffer Previous {
    mat4 previous[];
};

layout(set = 1, binding = 0) uniform Draw {
    mat4 view_projection; // jittered: where the pixel is drawn
    uvec4 first;          // x: this draw's first instance
    mat4 current;         // without the jitter, this frame's camera
    mat4 prior;           // and the last frame's (the order of mesh_velocity.vert's block)
}
draw;

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec2 vUV;
layout(location = 2) out float vAlpha;
layout(location = 3) out vec3 vCurrent;  // clip x, y, w this frame
layout(location = 4) out vec3 vPrevious; // and the last
layout(location = 5) flat out float vFresh;
layout(location = 6) out vec3 vWorld;
layout(location = 7) out vec3 vOldInCurrent;

void main() {
    const uint at = draw.first.x + uint(gl_InstanceIndex);
    const Instance instance = instances[at];
    mat4 before = previous[at];
    vFresh = before[0][3];
    before[0][3] = 0.0;
    const vec4 world = instance.model * vec4(aPos, 1.0);
    vWorld = world.xyz;
    gl_Position = draw.view_projection * world;
    vCurrent = (draw.current * world).xyw;
    const vec4 previous_world = before * vec4(aPos, 1.0);
    vPrevious = (draw.prior * previous_world).xyw;
    vOldInCurrent = (draw.current * previous_world).xyw;
    vNormal = mat3(instance.normal0.xyz, instance.normal1.xyz, instance.normal2.xyz) * aNormal;
    vUV = aUV;
    vAlpha = instance.color.a * aColor.a;
}
