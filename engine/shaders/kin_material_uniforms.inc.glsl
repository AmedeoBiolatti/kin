#define KIN_MATERIAL_INTERFACE_VERSION 2
// Shared ABI for stock and unlit material shaders: FrameUniforms and MaterialUniforms.
#include "kin_frame_uniforms.inc.glsl"
// The draw's.
layout(set = 3, binding = 1) uniform Material {
    vec4 params; // x: emissive, y: unlit, z: receives the sun's shadow, w: alpha cutoff (0: none)
    vec4 pbr;    // x: metallic, y: roughness, z: normal scale, w: occlusion strength
    vec4 maps;   // x: has a normal map, y: has an emissive map, z: opaque (it is in the occlusion pre-pass), w: additive
    vec4 emission; // xyz: linear RGB factor, w: independent emission (otherwise legacy base-relative)
} material;
