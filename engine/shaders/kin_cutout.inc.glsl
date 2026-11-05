// What the cut-out variants of the shadow, pick and pre-passes bind
// (Blend3D::Mask): the base colour, whose alpha is tested, and the cutoff.
//
// A surface's cut-out variants (KIN_SURFACE_ALPHA: a material graph's alpha, kin/render3d/material_graph3d.hpp) bind
// all five of the material's maps and their parameters, and test the alpha that function gives: a surface shader's
// `float kin_surface_alpha(vec2 uv, vec3 position, float vertex_alpha)` (the UV as the maps read it, the world
// position, the copy's and vertex's alpha), defined after main.
layout(set = 2, binding = 0) uniform sampler2D uBase;
#ifdef KIN_SURFACE_ALPHA
layout(set = 2, binding = 1) uniform sampler2D uMetalRough;
layout(set = 2, binding = 2) uniform sampler2D uNormalMap;
layout(set = 2, binding = 3) uniform sampler2D uOcclusion;
layout(set = 2, binding = 4) uniform sampler2D uEmissive;
#endif
layout(set = 3, binding = 0) uniform Cutoff {
    vec4 cutoff; // x: the alpha below which nothing is drawn; a surface's: y 1 to decode sRGB maps, z Scene3D::time
    vec4 uv;     // xy: the maps' scale, zw: their offset (Material3D::uv_scale, uv_offset)
#ifdef KIN_SURFACE_ALPHA
    vec4 surface[4]; // Material3D::shader_params
#endif
} material;

#ifdef KIN_SURFACE_ALPHA
vec4 kin_surface_param(int i) {
    return material.surface[i];
}
vec4 kin_graph_sample(sampler2D map, vec2 uv, bool srgb) {
    vec4 t = texture(map, uv);
    if (srgb && material.cutoff.y > 0.5) {
        t.rgb = mix(t.rgb / 12.92, pow((t.rgb + 0.055) / 1.055, vec3(2.4)), step(0.04045, t.rgb));
    }
    return t;
}
#include "kin_graph_noise.inc.glsl"
float KIN_SURFACE_ALPHA(vec2 uv, vec3 position, float vertex_alpha);
#endif

// The alpha a cut-out tests at `uv` (the mesh's), `position` (world) with the copy's and vertex's `vertex_alpha`: the
// base map's, or the surface's.
float kin_cutout_alpha(vec2 uv, vec3 position, float vertex_alpha) {
    const vec2 map_uv = uv * material.uv.xy + material.uv.zw;
#ifdef KIN_SURFACE_ALPHA
    return KIN_SURFACE_ALPHA(map_uv, position, vertex_alpha);
#else
    return vertex_alpha * texture(uBase, map_uv).a;
#endif
}
