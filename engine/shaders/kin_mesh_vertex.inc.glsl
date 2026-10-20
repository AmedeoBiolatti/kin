// kin's 3D mesh vertex shader, shared by mesh.vert and mesh_velocity.vert (which defines KIN_VELOCITY first).
// kin's 3D meshes (kin/render3d): one draw is many instances of a mesh, each
// placed by its own matrix, read from a storage buffer.
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aColor; // sRGB

#include "kin_instance.inc.glsl"

#ifdef KIN_SKIN
// The skinned variants (mesh_skinned.vert, mesh_skinned_velocity.vert): the joints' matrices and each copy's first one
// follow the instances (binding 1 and 2). The velocity variant also has the Previous buffer at 1, so they start at 2, and
// the joints of the frame before and where each copy's start in them follow (4 and 5).
#ifdef KIN_VELOCITY
#define KIN_SKIN_BINDING 2
#define KIN_SKIN_PREVIOUS
#else
#define KIN_SKIN_BINDING 1
#endif
#include "kin_skin.inc.glsl"
#endif

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

#include "kin_srgb.inc.glsl"

void main() {
    // gl_InstanceIndex counts from 0: SDL_GPU's first_instance is left at 0
    // (backends disagree whether it adds to it), so the draw says where it starts.
    // The one exception is the GPU cut's indirect draws (cluster_args.comp), whose
    // arguments carry the copy's slot as the first instance with first.x 0: that
    // relies on the instance index including it, which Vulkan and Metal do and
    // Direct3D 12 does not, so the renderer keeps the cut on the CPU there.
#ifdef KIN_VELOCITY
    const uint at = instance_at(draw.first.x + uint(gl_InstanceIndex));
    Instance instance = instances[at];
#else
    Instance instance = instances[instance_at(draw.first.x + uint(gl_InstanceIndex))];
#endif
#ifdef KIN_SKIN
    // The vertex posed by its joints, in the model's space; the copy's transform places that.
    const mat4 skin = skin_matrix(instance_at(draw.first.x + uint(gl_InstanceIndex)));
    const vec4 world = instance.model * (skin * vec4(aPos, 1.0));
#else
    const vec4 world = instance.model * vec4(aPos, 1.0);
#endif
    vWorld = world.xyz;
    gl_Position = draw.view_projection * world;
#ifdef KIN_SKIN
    vNormal = mat3(instance.normal0.xyz, instance.normal1.xyz, instance.normal2.xyz) * normalize(mat3(skin) * aNormal);
#else
    vNormal = mat3(instance.normal0.xyz, instance.normal1.xyz, instance.normal2.xyz) * aNormal;
#endif
    vColor = instance.color * vec4(decode(aColor.rgb), aColor.a);
    vUV = aUV;
    vId = floatBitsToUint(instance.normal0.w);
#ifdef KIN_VELOCITY
#ifdef KIN_SKIN
    // The copy's model matrix of the frame before times its pose of the frame before (the same expressions as
    // prepass_velocity_skinned.vert's). A copy with no past pose is posed as now (camera-only motion); one with no past at all
    // is fresh, as a plain copy is.
    mat4 before = previous[at];
    vFresh = before[0][3];
    before[0][3] = 0.0;
    vCurrent = (draw.current * world).xyw;
    vPrevious = (draw.prior * (before * (skin_matrix_previous(at, skin) * vec4(aPos, 1.0)))).xyw;
#else
    // The same expressions as prepass_velocity.vert's, so the velocity is the pre-pass's to the last bit.
    mat4 before = previous[at];
    vFresh = before[0][3];
    before[0][3] = 0.0;
    vCurrent = (draw.current * world).xyw;
    vPrevious = (draw.prior * (before * vec4(aPos, 1.0))).xyw;
#endif
#endif
}
