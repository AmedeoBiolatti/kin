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
    vec4 uv; // xy: the maps' scale, zw: their offset (Material3D::uv_scale, uv_offset)
};
layout(location = 9) flat in uint vMaterial;
#if defined(KIN_LIGHT_GRID) && defined(KIN_GI)
#define KIN_DRAW_MATERIAL_BINDING 19 // after the light grid's two and the area lights' two
#elif defined(KIN_LIGHT_GRID)
#define KIN_DRAW_MATERIAL_BINDING 17
#else
#define KIN_DRAW_MATERIAL_BINDING 10
#endif
layout(std430, set = 2, binding = KIN_DRAW_MATERIAL_BINDING) readonly buffer KinDrawMaterials {
    KinDrawMaterial draw_materials[];
};
#define material draw_materials[vMaterial]
#ifdef KIN_SURFACE
// A surface shader's parameters, a draw's copy's four after its record's place (Material3D::shader_params).
layout(std430, set = 2, binding = KIN_DRAW_MATERIAL_BINDING + 1) readonly buffer KinSurfaceParams {
    vec4 kin_surface_params[];
};
#endif
#else
layout(set = 3, binding = 1) uniform Material {
    vec4 params; // x: emissive, y: unlit, z: receives the sun's shadow, w: alpha cutoff (0: none)
    vec4 pbr;    // x: metallic, y: roughness, z: normal scale, w: occlusion strength
    vec4 maps;   // x: has a normal map, y: has an emissive map, z: opaque (it is in the occlusion pre-pass), w: additive
    vec4 emission; // xyz: linear RGB factor, w: independent emission (otherwise legacy base-relative)
    vec4 uv;       // xy: the maps' scale, zw: their offset (Material3D::uv_scale, uv_offset)
#ifdef KIN_SURFACE
    vec4 surface[4]; // a surface shader's: Material3D::shader_params (kin_surface_param)
#endif
} material;
#endif
