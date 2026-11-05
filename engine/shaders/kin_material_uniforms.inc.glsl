#define KIN_MATERIAL_INTERFACE_VERSION 2
// Shared ABI for stock and unlit material shaders: FrameUniforms and MaterialUniforms.
#include "kin_frame_uniforms.inc.glsl"
// The draw's.
#ifdef KIN_DRAW_MATERIALS
struct KinDrawMaterial {
    vec4 params;
    vec4 pbr;
    vec4 maps;
    vec4 emission;
};
layout(location = 9) flat in uint vMaterial;
#ifdef KIN_LIGHT_GRID
#define KIN_DRAW_MATERIAL_BINDING 17
#else
#define KIN_DRAW_MATERIAL_BINDING 10
#endif
layout(std430, set = 2, binding = KIN_DRAW_MATERIAL_BINDING) readonly buffer KinDrawMaterials {
    KinDrawMaterial draw_materials[];
};
#define material draw_materials[vMaterial]
#else
layout(set = 3, binding = 1) uniform Material {
    vec4 params; // x: emissive, y: unlit, z: receives the sun's shadow, w: alpha cutoff (0: none)
    vec4 pbr;    // x: metallic, y: roughness, z: normal scale, w: occlusion strength
    vec4 maps;   // x: has a normal map, y: has an emissive map, z: opaque (it is in the occlusion pre-pass), w: additive
    vec4 emission; // xyz: linear RGB factor, w: independent emission (otherwise legacy base-relative)
} material;
#endif
