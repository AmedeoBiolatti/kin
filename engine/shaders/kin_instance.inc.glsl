// A mesh instance as the 3D vertex shaders read it, and the buffer of them
// (set 0, binding 0). Mirrors GpuInstance3D in gpu_renderer3d.hpp: change
// them together.
//
// The model matrix, the normals' (its cofactors: they turn normals as the
// model turns the shape, scaled; the first column's w holds the copy's pick
// id, plus one, as bits; the second's how the clip plane meets the copy, its
// DrawClip3D), and the colour (linear, the material's times the tint, straight
// alpha).
struct Instance {
    mat4 model;
    vec4 normal0;
    vec4 normal1;
    vec4 normal2;
    vec4 color;
};
// (A surface's vertex variants bind its maps as the vertex samplers first: their storage bindings follow those.)
#ifndef KIN_VS_SHIFT
#define KIN_VS_SHIFT 0
#endif
layout(std430, set = 0, binding = 0 + KIN_VS_SHIFT) readonly buffer Instances {
    Instance instances[];
};

// gl_ClipDistance[0] of a copy's point `world`, by the camera's clip plane `clip` (Camera3D::clip_plane; (0,0,0,1):
// none) and the copy's DrawClip3D `keep` (normal1.w): 0 cut by the plane; 1 never cut; 2 never cut, and drawn only
// by cameras that have a clip plane. A copy whose pick id (normal0.w's bits, plus one) is the camera's `hidden` one
// (Camera3D::hidden_id plus one; 0: none) is not drawn at all.
float kin_clip_distance(vec4 clip, vec3 world, float keep, uint hidden, float id) {
    if (hidden != 0u && floatBitsToUint(id) == hidden)
        return -1.0;
    if (keep < 0.5)
        return dot(clip.xyz, world) + clip.w;
    return keep > 1.5 && clip == vec4(0.0, 0.0, 0.0, 1.0) ? -1.0 : 1.0;
}

// Retained records keep their stable slot; each view uploads only compact indices.
#ifdef KIN_INDEXED
layout(std430, set = 0, binding = 1 + KIN_VS_SHIFT) readonly buffer VisibleInstances { uint instance_indices[]; };
uint instance_at(uint i) { return instance_indices[i]; }
#else
uint instance_at(uint i) { return i; }
#endif
