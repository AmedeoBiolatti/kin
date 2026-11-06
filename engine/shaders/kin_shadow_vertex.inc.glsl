// kin's 3D shadow pass (shadow.vert, shadow_mask.vert with KIN_SHADOW_MASK): mesh.vert's placing, and nothing else (no
// normals, colours or varyings: depth only); a cut-out's (Blend3D::Mask) also the UV and alpha shadow_mask.frag tests.
layout(location = 0) in vec3 aPos;
#if defined(KIN_SHADOW_MASK) || defined(KIN_SURFACE_VERTEX)
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aColor;
#endif
#ifdef KIN_SURFACE_VERTEX
layout(location = 1) in vec3 aNormal; // (a surface's World Position Offset reads it)
#endif

// The model matrix (and a cut-out's colour) are used here.
#include "kin_instance.inc.glsl"

// With cascades, a cascade's copies are a list of indices into the instances
// (the caster test is per cascade); without, they are one run from first.x.
layout(std430, set = 0, binding = 1 + KIN_VS_SHIFT) readonly buffer Casters {
    uint casters[];
};

layout(set = 1, binding = 0) uniform Draw {
    mat4 view_projection;
    uvec4 first; // x: this draw's first instance; y: where its list starts, plus one (0: no list)
} draw;

uint instance_index() {
    return draw.first.y != 0u ? casters[draw.first.y - 1u + uint(gl_InstanceIndex)] : draw.first.x + uint(gl_InstanceIndex);
}

#ifdef KIN_SHADOW_MASK
layout(location = 0) out vec2 vUV;
layout(location = 1) out float vAlpha; // the instance's and the vertex's
layout(location = 2) out vec3 vWorld;  // for a surface's cut-out (kin_cutout.inc.glsl)
#endif

void main() {
    const Instance instance = instances[instance_index()];
    vec4 world = instance.model * vec4(aPos, 1.0);
#ifdef KIN_SURFACE_VERTEX
    // A surface's World Position Offset (kin_surface_vertex.inc.glsl), at this frame's time.
    world.xyz = kin_vertex_offset(world.xyz, mat3(instance.normal0.xyz, instance.normal1.xyz, instance.normal2.xyz) * aNormal,
                                  aUV, instance.color * vec4(decode(aColor.rgb), aColor.a), kin_vs.time.x);
#endif
    gl_Position = draw.view_projection * world;
#ifdef KIN_SHADOW_MASK
    vWorld = world.xyz;
    vUV = aUV;
    vAlpha = instance.color.a * aColor.a;
#endif
}
