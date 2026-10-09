// kin's 3D mesh vertex shader, shared by mesh.vert and mesh_velocity.vert (which defines KIN_VELOCITY first).
// kin's 3D meshes (kin/render3d): one draw is many instances of a mesh, each
// placed by its own matrix, read from a storage buffer.
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aColor; // sRGB

#include "kin_instance.inc.glsl"

#ifdef KIN_VELOCITY
// The velocity variant (mesh_velocity.vert): the same block with the two camera matrices after it, so a draw that
// pushes the longer block to a pipeline with the plain mesh.vert (a custom shader's) is read right by both.
// Each copy's matrix of the frame before is in a buffer of its own, indexed as the instances are; its [0][3]
// (a zero in any placing) is 1 for a copy with no past: new, or matched to nothing believable.
layout(std430, set = 0, binding = 1) readonly buffer Previous {
    mat4 previous[];
};
layout(set = 1, binding = 0) uniform Draw {
    mat4 view_projection; // jittered: where the pixel is drawn
    uvec4 first;          // x: this draw's first instance
    mat4 current;         // without the jitter, this frame's camera
    mat4 prior;           // and the last frame's
} draw;
layout(location = 5) out vec3 vCurrent;  // clip x, y, w this frame
layout(location = 6) out vec3 vPrevious; // and the last
layout(location = 7) flat out float vFresh;
#else
layout(set = 1, binding = 0) uniform Draw {
    mat4 view_projection;
    uvec4 first; // x: this draw's first instance
} draw;
#endif

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
#ifdef KIN_VELOCITY
    const uint at = draw.first.x + uint(gl_InstanceIndex);
    Instance instance = instances[at];
#else
    Instance instance = instances[draw.first.x + uint(gl_InstanceIndex)];
#endif
    const vec4 world = instance.model * vec4(aPos, 1.0);
    vWorld = world.xyz;
    gl_Position = draw.view_projection * world;
    vNormal = mat3(instance.normal0.xyz, instance.normal1.xyz, instance.normal2.xyz) * aNormal;
    vColor = instance.color * vec4(decode(aColor.rgb), aColor.a);
    vUV = aUV;
    vId = floatBitsToUint(instance.normal0.w);
#ifdef KIN_VELOCITY
    // The same expressions as prepass_velocity.vert's, so the velocity is the pre-pass's to the last bit.
    mat4 before = previous[at];
    vFresh = before[0][3];
    before[0][3] = 0.0;
    vCurrent = (draw.current * world).xyw;
    vPrevious = (draw.prior * (before * vec4(aPos, 1.0))).xyw;
#endif
}
