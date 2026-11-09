// kin's 3D material shading (kin/render3d), shared by mesh.frag and custom
// material shaders (docs/rendering_3d.md, Custom shaders). A base colour (vertex, instance and
// material colours, times a texture), lit in linear light by a sun (through
// its shadow map), ambient light and up to 16 point lights, plus the
// material's own light; or unlit. The lighting is glTF's metallic-roughness
// model: a Lambert diffuse and a Cook-Torrance specular (GGX distribution,
// Smith visibility, Schlick Fresnel). Output premultiplied, as kin's targets are.
//
// Light colours are irradiance over pi (a white surface facing a light of
// colour c shows c), so a BRDF f meets a light as f * pi * c * cos.
#ifndef KIN_NO_CUSTOM_PARAMS
#define KIN_CUSTOM_MATERIAL
#define KIN_FOG
#define KIN_LIGHTS
#endif
layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec4 vColor; // linear, straight alpha
layout(location = 2) in vec2 vUV;
layout(location = 3) in vec3 vWorld;
#ifdef KIN_SURFACE
// The copy's origin in the world (kin_mesh_vertex.inc.glsl's, flat): a surface's object position (as the offset's, at
// a vertex: kin_surface_vertex.inc.glsl).
layout(location = 10) flat in vec3 vObject;
vec3 kin_object_position() {
    return vObject;
}
#endif

layout(set = 2, binding = 0) uniform sampler2D uBase;         // white when the material has none
layout(set = 2, binding = 1) uniform sampler2DArrayShadow uShadow; // depth from the sun, a layer a cascade, compared
layout(set = 2, binding = 2) uniform sampler2D uMetalRough;   // G roughness, B metallic: white without
layout(set = 2, binding = 3) uniform sampler2D uNormalMap;    // tangent space; any without (see maps.x)
layout(set = 2, binding = 4) uniform sampler2D uOcclusion;    // R: white without
layout(set = 2, binding = 5) uniform sampler2D uEmissive;     // any without (see maps.y)
#ifdef KIN_LIGHT_GRID
// The stock light variants': layer 0 the environment's, a level a roughness (with frame.environment.w); the layers
// after it the reflection probes' (frame.probe), panoramas the same size, in world space.
layout(set = 2, binding = 6) uniform sampler2DArray uEnvSpecular;
vec4 kin_env_specular(vec2 uv, float lod) {
    return textureLod(uEnvSpecular, vec3(uv, 0.0), lod);
}
#else
layout(set = 2, binding = 6) uniform sampler2D uEnvSpecular;   // the environment's, a level a roughness (with frame.environment.w)
vec4 kin_env_specular(vec2 uv, float lod) {
    return textureLod(uEnvSpecular, uv, lod);
}
#endif
layout(set = 2, binding = 7) uniform sampler2D uEnvIrradiance; // and what a diffuse surface facing each way receives, over pi
layout(set = 2, binding = 8) uniform sampler2D uAmbientOcclusion; // R: the share of ambient light that reaches the view's pixels (white without)
layout(set = 2, binding = 9) uniform sampler2D uContactShadow;    // R: the share of the sun that gets through a short march toward it (white without)
#ifdef KIN_LIGHTS
// Spot and point lights' shadows and the moon's (P3b): only the shaders made with KIN_LIGHTS have them, so that every other
// shader, and its compiled code, is as it was.
layout(set = 2, binding = 10) uniform sampler2DArrayShadow uLocalShadow; // depth from a spot (a layer), or a face of a point light's cube (six layers), compared
layout(set = 2, binding = 11) uniform sampler2DArrayShadow uMoonShadow;  // depth from the fill light (the moon), a layer a cascade, compared
#endif
#ifdef KIN_GI
// Global illumination (G5; docs/3d_plan.md): only the stock light variants (mesh_light*.frag) have it, so that every
// other shader, and custom ones, keep their samplers. The probes' light on what the pre-pass drew, read once a texel at
// half the view's size (gi_resolve.comp).
layout(set = 2, binding = 12) uniform sampler2D uGiLight; // rgb: the light facing the surface; a: its depth along the
                                                          // view (less: the probes have none there; 0: no surface)
layout(set = 2, binding = 13) uniform sampler2D uGiSlope; // the light facing way d is rgb * (w + xyz . d)
layout(set = 2, binding = 14) uniform sampler2D uGiCoverage; // share of probe lighting (.r; .a with frame.gi.y),
                                                             // the rest ambient; with frame.gi.y, rgb: the area
                                                             // lights' irradiance over pi (gi_area.comp)
#endif
#ifdef KIN_SURFACE
// A surface's maps past its five (Material3D::graph_maps: a material graph's, a layer each), the sampler after the
// variant's others: its storage buffers bind one later than the stock shader's (KIN_STORAGE_SHIFT).
#if defined(KIN_GI)
layout(set = 2, binding = 15) uniform sampler2DArray uGraphMaps;
#elif defined(KIN_LIGHTS)
layout(set = 2, binding = 12) uniform sampler2DArray uGraphMaps;
#else
layout(set = 2, binding = 10) uniform sampler2DArray uGraphMaps;
#endif
#define KIN_STORAGE_SHIFT 1
#else
#define KIN_STORAGE_SHIFT 0
#endif
#ifdef KIN_LIGHT_GRID
// The light grid (the stock light variants, with KIN_LIGHTS): every point and spot light in view, and for each cell of
// the view (frame.grid: tiles of pixels by slices of view depth) those whose reach touches it, so that a fragment
// loops over its cell's lights only; where frame.grid.x is 0 (the overlay) the fixed blocks' loops light it.
struct KinGridLight {
    vec4 at;     // position, radius
    vec4 colour; // linear rgb, the square of the light's size
    vec4 axis;   // a spot's way and the cosine of its outer angle (a point light: w is -2)
    vec4 cone;   // x: 1 / (cos inner - cos outer), y: its shadow's layer (a point light's cube's first; -1: none)
};
#ifdef KIN_GI
// The area lights' reflections (GlobalIllumination3D::area_lights; kin_area_specular): linearly transformed cosines'
// two GGX tables (64 x 64 each, rows by sqrt(1 - cos(view, normal)), roughness along), and the emitters, 5 vec4 each as
// gi_area.comp reads them but their radiance at this render's strength.
layout(std430, set = 2, binding = 17 + KIN_STORAGE_SHIFT) readonly buffer KinAreaLtc { vec4 kin_area_ltc[]; };
layout(std430, set = 2, binding = 18 + KIN_STORAGE_SHIFT) readonly buffer KinAreaShading { vec4 kin_area_shading[]; };
layout(std430, set = 2, binding = 15 + KIN_STORAGE_SHIFT) readonly buffer KinGridLights { KinGridLight grid_lights[]; };
#else
layout(std430, set = 2, binding = 14 + KIN_STORAGE_SHIFT) readonly buffer KinGridLights { KinGridLight grid_lights[]; };
#endif
// A cell (slice by row by column): the first of its list << 12 | its count; the lists after the cells.
#ifdef KIN_GI
layout(std430, set = 2, binding = 16 + KIN_STORAGE_SHIFT) readonly buffer KinGridCells { uint grid_cells[]; };
#else
layout(std430, set = 2, binding = 15 + KIN_STORAGE_SHIFT) readonly buffer KinGridCells { uint grid_cells[]; };
#endif
#endif

const float PI = 3.14159265358979;
#include "kin_material_uniforms.inc.glsl"

// Fog (Scene3D::fog): only the shaders made with KIN_FOG have it (mesh_fog.frag, mesh_velocity_fog.frag), so that every
// other shader, and its compiled code, is as it was. Its block takes the slot custom parameters have elsewhere.
#ifdef KIN_FOG
#include "kin_fog.inc.glsl"
#ifdef KIN_CUSTOM_MATERIAL
// Keep the legacy parameter prefix. The combined block is 3280 bytes, below
// SDL_GPU's 4 KiB per uniform slot; local lighting occupies slot 3.
layout(set = 3, binding = 2) uniform CustomParams {
    vec4 values[4];
    KinFog fog;
    KinSky sky;
    vec4 table[KIN_FOG_ROWS * 4];
} custom;
#define fog_params custom
#else
layout(set = 3, binding = 2) uniform FogParams {
    KinFog fog;
    KinSky sky;
    vec4 table[KIN_FOG_ROWS * 4]; // the sky's light for colour_from_sky (kin_fog_sky), made when the sky changes
} fog_params;
#endif
#endif

// Spot lights and the shadows of lights that are not the sun (P3b): KIN_LIGHTS (mesh_light.frag and its variants). Its block
// takes the slot after fog (or, in stock shaders without fog, the fog slot).
// Custom shaders pack their parameter prefix with fog and always use slot 3 for lights.
#ifdef KIN_LIGHTS
#ifdef KIN_FOG
#define KIN_LIGHTS_BINDING 3
#else
#define KIN_LIGHTS_BINDING 2
#endif
#include "kin_local_lights.inc.glsl"
#endif

// Custom material shaders (Material3D::shader) include this file and write
// main() with the functions at its end; they declare nothing else. Their
// four vec4 of parameters (Material3D::shader_params) are kin_param(0..3);
// mesh.frag, which has none, defines KIN_NO_CUSTOM_PARAMS first.
#ifdef KIN_CUSTOM_MATERIAL
vec4 kin_param(int i) {
    return custom.values[i];
}
#endif
layout(location = 0) out vec4 FragColor;

#include "kin_srgb.inc.glsl"

// The normal a shadow map is read along (the receiver pushed off along it, a
// bias's slope taken to it), for the sun's, the moon's and the local lights'.
// Texture/bump normals change illumination, not the receiver geometry.
// Flat-shaded clusters have no vertex normal; n is already their face
// normal. Double-sided receivers offset toward the side being rendered.
vec3 shadow_offset_normal(vec3 n) {
    return dot(vNormal, vNormal) > 1e-20 ? normalize(vNormal) * (gl_FrontFacing ? 1.0 : -1.0) : n;
}

#include "kin_shadows.inc.glsl"

// Where the equirectangular environment shows a direction (render3d/environment3d.hpp).
vec2 environment_uv(vec3 d) {
    d = mat3(frame.environment_matrix) * d;
    return vec2(0.5 + atan(d.z, d.x) / (2.0 * PI), 0.5 - asin(clamp(d.y, -1.0, 1.0)) / PI);
}

#ifdef KIN_LIGHT_GRID
// The reflection probes' light at `p` along `r` (unit), at the level `lod`: every probe whose influence box holds `p`, in
// the renderer's order (by priority, then the smaller), each taking its weight of what the ones before left; .a the
// share they took (the rest is the environment's). A probe with a parallax box is read toward where the ray leaves it,
// from where it was captured (reflection_probe_direction in reflection_probes3d.cpp does the same on the CPU). `own`: the
// share taken by probes without parallax (a shiny thing's own, captured from its centre), which the screen-space
// reflections leave alone.
vec4 kin_probe_reflection(vec3 p, vec3 r, float lod, out float own) {
    vec3 sum = vec3(0.0);
    float share = 0.0;
    own = 0.0;
    const int count = int(frame.probes.x);
    for (int i = 0; i < count && share < 0.999; ++i) {
        const vec4 a0 = frame.probe[i * 6], a1 = frame.probe[i * 6 + 1], a2 = frame.probe[i * 6 + 2];
        const vec4 a3 = frame.probe[i * 6 + 3], a4 = frame.probe[i * 6 + 4], a5 = frame.probe[i * 6 + 5];
        const vec3 centre = vec3(a0.w, a1.w, a2.w);
        const vec3 q = p - centre;
        const vec3 local = vec3(dot(a0.xyz, q), dot(a1.xyz, q), dot(a2.xyz, q));
        const vec3 room = a3.xyz - abs(local);
        const float inside = min(min(room.x, room.y), room.z);
        if (inside <= 0.0)
            continue;
        const float weight = a3.w > 0.0 ? min(inside / a3.w, 1.0) : 1.0;
        vec3 way = r;
        const vec3 half_box = a4.xyz;
        if (half_box.x > 0.0 && all(lessThanEqual(abs(local), half_box))) {
            const vec3 d = vec3(dot(a0.xyz, r), dot(a1.xyz, r), dot(a2.xyz, r));
            // Where the ray leaves the box: the nearest of the faces it heads for.
            const vec3 face = mix(-half_box, half_box, step(0.0, d));
            vec3 leave = vec3(1e30);
            for (int k = 0; k < 3; ++k)
                if (abs(d[k]) > 1e-8)
                    leave[k] = (face[k] - local[k]) / d[k];
            const float t = min(min(leave.x, leave.y), leave.z);
            const vec3 hit = centre + mat3(a0.xyz, a1.xyz, a2.xyz) * (local + d * t);
            const vec3 toward = hit - a5.xyz;
            way = dot(toward, toward) > 1e-12 ? normalize(toward) : r;
        }
        const vec2 uv = vec2(0.5 + atan(way.z, way.x) / (2.0 * PI), 0.5 - asin(clamp(way.y, -1.0, 1.0)) / PI);
        const float take = weight * (1.0 - share);
        sum += textureLod(uEnvSpecular, vec3(uv, a5.w), lod).rgb * a4.w * take;
        share += take;
        own += half_box.x > 0.0 ? 0.0 : take;
    }
    return vec4(sum, share);
}
#endif

#if defined(KIN_LIGHT_GRID) && defined(KIN_GI)
// Screen-space reflections (ssr_trace.comp, Scene3D::ssr): the last storage buffer of the light variants: three words a
// texel of a half-size grid (how far its reflection meets something, its share as a half, the view depth traced), then
// the previous frame's picture as a pyramid (ssr_pyramid.comp: two words a texel).
#if defined(KIN_DRAW_MATERIALS) && defined(KIN_SURFACE)
#define KIN_SSR_BINDING (21 + KIN_STORAGE_SHIFT)
#elif defined(KIN_DRAW_MATERIALS)
#define KIN_SSR_BINDING (20 + KIN_STORAGE_SHIFT)
#else
#define KIN_SSR_BINDING (19 + KIN_STORAGE_SHIFT)
#endif
layout(std430, set = 2, binding = KIN_SSR_BINDING) readonly buffer KinScreenReflections {
    uint kin_ssr[];
};
// A texel of the previous frame's picture at `level` of its pyramid (ssr_pyramid.comp), clamped to the level.
vec3 kin_ssr_texel(int level, ivec2 q) {
    const ivec2 grid = ivec2(frame.ssr.yz);
    uint first = uint(frame.ssr_pyramid.z);
    ivec2 size = grid;
    for (int k = 0; k < level; ++k) {
        first += uint(size.x * size.y) * 2u;
        size = max(size / 2, ivec2(1));
    }
    q = clamp(q, ivec2(0), size - 1);
    const uint at = first + uint(q.y * size.x + q.x) * 2u;
    return vec3(unpackHalf2x16(kin_ssr[at]), unpackHalf2x16(kin_ssr[at + 1u]).x);
}
// The picture at `uv` (0..1, top row first), bilinear in a level, mixed between two levels.
vec3 kin_ssr_picture(vec2 uv, float level) {
    vec3 c[2];
    for (int i = 0; i < 2; ++i) {
        const int l = min(int(level) + i, int(frame.ssr_pyramid.x) - 1);
        const ivec2 size = max(ivec2(frame.ssr.yz) >> l, ivec2(1));
        const vec2 at = uv * vec2(size) - 0.5;
        const ivec2 base = ivec2(floor(at));
        const vec2 f = at - vec2(base);
        c[i] = mix(mix(kin_ssr_texel(l, base), kin_ssr_texel(l, base + ivec2(1, 0)), f.x),
                   mix(kin_ssr_texel(l, base + ivec2(0, 1)), kin_ssr_texel(l, base + ivec2(1, 1)), f.x), f.y);
    }
    return mix(c[0], c[1], fract(level));
}
// The reflection at `p` (world) along `r` (this fragment's own: its normal map's) and its share (a). The four grid
// texels round the pixel that traced this fragment's surface (by their depth: an edge's other side, or what glass is in
// front of, counts for nothing) say how far the reflection meets something; the reflection followed that far is looked
// up in the previous frame's picture, at the level as wide as the reflection's lobe is there (wider as the surface is
// rougher and the hit further): blurred for a rough surface.
vec4 kin_ssr_reflection(float depth, vec3 p, vec3 r, float roughness) {
    const ivec2 grid = ivec2(frame.ssr.yz);
    const vec2 at = gl_FragCoord.xy * 0.5 - 0.5;
    const ivec2 base = ivec2(floor(at));
    const vec2 f = at - vec2(base);
    float distance = 0.0, share = 0.0, weights = 0.0;
    for (int k = 0; k < 4; ++k) {
        const ivec2 o = ivec2(k & 1, k >> 1);
        const ivec2 q = clamp(base + o, ivec2(0), grid - 1);
        const uint i = uint(q.y * grid.x + q.x) * 3u;
        const float traced = uintBitsToFloat(kin_ssr[i + 2u]);
        const float near = abs(traced - depth) < 0.02 * depth + 0.05 ? 1.0 : 0.0;
        const float w = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y) * near;
        const float s = unpackHalf2x16(kin_ssr[i + 1u]).x;
        distance += uintBitsToFloat(kin_ssr[i]) * s * w;
        share += s * w;
        weights += w;
    }
    if (weights <= 1e-4 || share <= 1e-4)
        return vec4(0.0);
    distance /= share;
    share /= weights;
    const vec4 clip = frame.ssr_previous * vec4(p + r * distance, 1.0);
    if (clip.w <= 1e-4)
        return vec4(0.0);
    const vec2 ndc = clip.xy / clip.w;
    const vec2 uv = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    const vec2 edge = min(uv, 1.0 - uv);
    share *= smoothstep(0.0, 0.08, min(edge.x, edge.y));
    if (share <= 1e-4)
        return vec4(0.0);
    // The lobe's width where it meets what it reflects (GGX's, about alpha a unit away), in the grid's pixels there.
    const float alpha = roughness * roughness;
    const float width = 1.5 * alpha * distance * frame.ssr_pyramid.y / clip.w * 0.5 * frame.ssr.z;
    const float level = clamp(log2(max(width, 1.0)), 0.0, frame.ssr_pyramid.x - 1.0);
    return vec4(kin_ssr_picture(uv, level), share);
}
#endif

// Where a fragment reads the material's maps: its UV times the material's scale, plus its offset (one for all the maps,
// as glTF's KHR_texture_transform has it when they agree; identity: the UV as it is, exactly).
vec2 kin_map_uv() {
    return vUV * material.uv.xy + material.uv.zw;
}

// The normal bent by the map. The tangent frame comes from how the world
// position and the UVs change across the pixel, so meshes need no tangents:
// the directions u and v increase in, solved from the two derivatives (the
// same on either way the screen's y runs). glTF's maps have green up and v
// runs down. `front` is -1 seen from behind, where the whole frame turns.
vec3 mapped_normal(vec3 n, float front) {
    const vec3 dp1 = dFdx(vWorld), dp2 = dFdy(vWorld);
    const vec2 uv = kin_map_uv();
    const vec2 duv1 = dFdx(uv), duv2 = dFdy(uv);
    const float det = duv1.x * duv2.y - duv2.x * duv1.y;
    if (abs(det) < 1e-20) {
        return n; // no UVs to go by here
    }
    vec3 t = (dp1 * duv2.y - dp2 * duv1.y) * sign(det);
    vec3 b = (dp2 * duv1.x - dp1 * duv2.x) * sign(det);
    t = normalize(t - n * dot(n, t));
    b = normalize(b - n * dot(n, b));
    vec3 m = texture(uNormalMap, uv).xyz * 2.0 - 1.0;
    // No normal of the map points into the surface: where z would (every texel of a map of two channels, BC5, whose
    // blue reads 0), the map holds x and y only, and z is what makes the normal a unit one.
    if (m.z < 0.0) {
        m.z = sqrt(max(1.0 - dot(m.xy, m.xy), 0.0));
    }
    m.xy *= material.pbr.z;
    return normalize(front * (t * m.x - b * m.y) + n * m.z);
}

#include "kin_brdf.inc.glsl"
#include "kin_light_response.inc.glsl"

// The ambient occlusion of the pixel (1: none, or this surface has none).
// Only what the pre-pass drew has it: a blended surface would read what is behind it.
float screen_occlusion() {
    if (frame.occlusion.x < 0.5 || material.maps.z < 0.5) {
        return 1.0;
    }
    return texture(uAmbientOcclusion, gl_FragCoord.xy * frame.occlusion.zw).r;
}

// The share of the sun the contact shadows let through (1: all, or this
// surface has none: as for the ambient occlusion, only what the pre-pass
// drew, and only what receives shadows).
float contact_shadow() {
    if (frame.occlusion.y < 0.5 || material.maps.z < 0.5 || material.params.z < 0.5) {
        return 1.0;
    }
    return textureLod(uContactShadow, gl_FragCoord.xy * frame.occlusion.zw, 0.0).r;
}

// --- What a custom material shader calls ---

// The base colour in linear light and its alpha: the vertex, instance and
// material colours times the base texture.
vec4 kin_base_color() {
    vec4 tex = texture(uBase, kin_map_uv());
    if (frame.flags.y > 0.5) {
        tex.rgb = decode(tex.rgb);
    }
    return vec4(vColor.rgb * tex.rgb, vColor.a * tex.a);
}

// The unit normal of a flat face from the derivatives of its world position along the screen's x and y. SDL_GPU's
// framebuffer rows run down while its clip space goes up, so their cross product points away from the viewer:
// turned round here. (Calibrated on Vulkan; SDL_GPU gives the other backends the same convention.)
vec3 kin_screen_normal(vec3 dp1, vec3 dp2) {
    const vec3 n = cross(dp1, dp2);
    const float l = length(n);
    return l > 0.0 ? n * (-1.0 / l) : vec3(0.0, 1.0, 0.0);
}

// The unit normal on the side the viewer sees (a double-sided face turns it
// round from behind), bent by the material's normal map when it has one.
vec3 kin_normal() {
    // Flat LOD/cluster vertices carry zero normals. Reconstruct the face normal, then retain the UV-derived normal
    // map frame exactly as for supplied normals. Flat shading does not imply an absent normal map.
    const vec3 dp1 = dFdx(vWorld), dp2 = dFdy(vWorld);
    const float front = gl_FrontFacing ? 1.0 : -1.0;
    const vec3 n = dot(vNormal, vNormal) < 1e-20 ? kin_screen_normal(dp1, dp2) : normalize(vNormal) * front;
    return material.maps.x > 0.5 ? mapped_normal(n, front) : n;
}

// Unit, from the surface to the camera.
vec3 kin_view() {
    return normalize(frame.eye.xyz - vWorld);
}

// The surface's world position, and its UV.
vec3 kin_position() {
    return vWorld;
}
vec2 kin_uv() {
    return kin_map_uv(); // as the maps read it
}

// The material's metallic (x) and roughness (y), the factors times the map.
vec2 kin_metallic_roughness() {
    const vec3 mr = texture(uMetalRough, kin_map_uv()).rgb;
    return vec2(clamp(material.pbr.x * mr.b, 0.0, 1.0),
                // A floor on the roughness: a perfect mirror would be a single bright pixel.
                clamp(material.pbr.y * mr.g, 0.045, 1.0));
}

// How much ambient light the occlusion map lets through.
float kin_occlusion() {
    return mix(1.0, texture(uOcclusion, kin_map_uv()).r, material.pbr.w);
}

// The material's own light for a surface of colour `base`: its share of the
// colour, or its emissive map, times the material's emissive.
vec3 kin_emissive(vec3 base) {
    vec3 own = material.emission.w > 0.5 ? vec3(1.0) : base;
    if (material.maps.y > 0.5) {
        own = texture(uEmissive, kin_map_uv()).rgb;
        if (frame.flags.y > 0.5) {
            own = decode(own);
        }
    }
    if (material.emission.w > 0.5) {
        own *= material.emission.rgb;
    }
    return own * material.params.x;
}

// --- Surfaces (docs/material_graph_plan.md) ---

// What kin shades at a fragment. kin_stock_surface() makes it from the material and its maps, as kin's own shaders
// do; a surface shader (KIN_SURFACE, kin_mesh_main.inc.glsl) changes it before kin lights it. `normal` is unit, in
// world space, on the viewer's side; `roughness` within [0.045, 1] (kin's floor: a perfect mirror is a lone pixel).
struct KinSurface {
    vec3 base_color; // linear: a dielectric's diffuse colour, a metal's reflection
    float alpha;     // straight; Blend3D::Mask discards below the material's cutoff
    vec3 normal;
    float metallic;
    float roughness;
    float occlusion; // the share of ambient light let through
    vec3 emission;   // linear light of its own, added after the lighting
    vec3 backlight;  // a thin surface's (a leaf's) colour for the light it lets through from behind (Unreal's
                     // two-sided foliage, Godot's backlight); 0, none, but in a surface shader's
    float clearcoat; // a clear coat's strength over it (glTF's KHR_materials_clearcoat, Unreal's Clear Coat); 0: none
    float clearcoat_roughness; // the coat's, within [0.045, 1]
    float specular;     // a dielectric's reflectance head on, 0.08 times it (0.5: 4%, kin's own; Unreal's Specular)
    float diffuse_wrap; // how far the diffuse light wraps round: 0 Lambert's (kin's own), 0.5 and 1 Unreal's volume
                        // lighting of translucency, directional and not (wrapped_diffuse); no reflection with it
    vec4 reflection;    // rgb: a reflection of its own (linear light: a mirror's picture, kin_planar_reflection); a: how
                        // much of the reflection it is, in place of the environment's or the probes' (0: none, kin's own)
};

#ifdef KIN_SURFACE
// A surface shader's parameters (Material3D::shader_params, four at a time, 16 vec4s): the draw's material block's, or, for
// merged draws (KIN_DRAW_MATERIALS), its copy's in the draws' table.
vec4 kin_surface_param(int i) {
#ifdef KIN_DRAW_MATERIALS
    return kin_surface_params[vMaterial * 16u + uint(i)];
#else
    return material.surface[i];
#endif
}
#endif

// --- Mirrors and portals (Camera3D's mirror_view and portal_view; docs/rendering_3d.md) ---

// Where `world` shows in a planar view's picture (0..1, rows down): `projector` is its PlanarView3D::projector.
vec2 kin_planar_uv(mat4 projector, vec3 world) {
    const vec4 clip = projector * vec4(world, 1.0);
    const vec2 ndc = clip.xy / clip.w;
    return vec2(0.5 + 0.5 * ndc.x, 0.5 - 0.5 * ndc.y); // (SDL_GPU's clip space goes up, its rows down)
}

// A picture of kin's (a View3D's texture) at `uv`, in linear light: decoded as the material's maps are.
vec3 kin_planar_read(sampler2D picture, vec2 uv) {
    vec3 c = textureLod(picture, uv, 0.0).rgb;
    if (frame.flags.y > 0.5) {
        c = decode(c);
    }
    return c;
}

// The face's own normal, on the viewer's side, without the normal map (kin_normal() is bent by it).
vec3 kin_geometric_normal() {
    const float front = gl_FrontFacing ? 1.0 : -1.0;
    return dot(vNormal, vNormal) < 1e-20 ? kin_screen_normal(dFdx(vWorld), dFdy(vWorld)) : normalize(vNormal) * front;
}

// Where this fragment reads a planar view's picture: its point moved by `distortion` (world units) times how far the
// surface's normal bends from the face's (a normal map's ripples), so that the picture wavers with them.
vec2 kin_planar_point_uv(mat4 projector, vec3 normal, float distortion) {
    const vec3 bend = normal - kin_geometric_normal();
    return kin_planar_uv(projector, vWorld + bend * distortion);
}

// A mirror, still water, a polished floor: `picture` (a mirror_view's View3D texture, read through `projector`) is the
// surface's reflection, `strength` of it, in place of the environment's or the probes'. The surface's Fresnel, roughness
// and specular occlusion weigh it as ever (water reflects little head on and much at a glance; a metal all of it); its
// highlights stay. A rough surface reads it blurred, and past a roughness of 0.4 leaves it, by 0.7, to the environment's.
void kin_planar_reflect(inout KinSurface s, sampler2D picture, mat4 projector, float distortion, float strength) {
    const vec2 uv = kin_planar_point_uv(projector, s.normal, distortion); // (before any branch: it may take derivatives)
    const float share = strength * (1.0 - smoothstep(0.4, 0.7, s.roughness));
    if (share <= 0.0) {
        return;
    }
    vec3 seen = kin_planar_read(picture, uv);
    if (s.roughness > 0.1) {
        // Four more reads round it, as far as the roughness spreads a reflection (a cheap blur: the picture has no
        // levels of detail).
        const vec2 r = vec2(0.04 * s.roughness * s.roughness) * vec2(1.0, float(textureSize(picture, 0).x) /
                                                                        float(textureSize(picture, 0).y));
        seen = (seen + kin_planar_read(picture, uv + vec2(r.x, 0.0)) + kin_planar_read(picture, uv - vec2(r.x, 0.0)) +
                kin_planar_read(picture, uv + vec2(0.0, r.y)) + kin_planar_read(picture, uv - vec2(0.0, r.y))) * 0.2;
    }
    s.reflection = vec4(seen, share);
}

// A portal, a window onto a view, a security camera's screen seen in place: `picture` (a portal_view's View3D texture,
// read through `projector`) is the surface's colour, `strength` of it. For an unlit material (Material3D::unlit), which
// shows its colour as it is.
void kin_planar_show(inout KinSurface s, sampler2D picture, mat4 projector, float distortion, float strength) {
    const vec3 seen = kin_planar_read(picture, kin_planar_point_uv(projector, s.normal, distortion));
    s.base_color = mix(s.base_color, seen, strength);
}

#ifdef KIN_SURFACE
// The same with a planar-view surface's parameters (planar_view_params: the projector, then distortion and strength)
// and the picture in the material's emissive map (Material3D::emissive_texture, with `emissive` 0): what kin's own
// planar_reflection.surface.glsl and planar_window.surface.glsl are.
mat4 kin_planar_projector() {
    return mat4(kin_surface_param(0), kin_surface_param(1), kin_surface_param(2), kin_surface_param(3));
}
void kin_planar_reflection(inout KinSurface s) {
    const vec4 settings = kin_surface_param(4);
    kin_planar_reflect(s, uEmissive, kin_planar_projector(), settings.x, settings.y);
}
void kin_planar_window(inout KinSurface s) {
    const vec4 settings = kin_surface_param(4);
    kin_planar_show(s, uEmissive, kin_planar_projector(), settings.x, settings.y);
}
#endif

KinSurface kin_stock_surface() {
    KinSurface s;
    const vec4 base = kin_base_color();
    s.base_color = base.rgb;
    s.alpha = base.a;
    s.normal = kin_normal();
    const vec2 mr = kin_metallic_roughness();
    s.metallic = mr.x;
    s.roughness = mr.y;
    s.occlusion = kin_occlusion();
    s.emission = kin_emissive(base.rgb);
    s.backlight = vec3(0.0);
    s.clearcoat = 0.0;
    s.clearcoat_roughness = 0.045;
    s.specular = 0.5;
    s.diffuse_wrap = 0.0;
    s.reflection = vec4(0.0);
    return s;
}

// The light of the scene on a surface (linear, what leaves it toward the
// camera): the sun through its shadow, the fill, the point lights, and the
// ambient light or environment. Not the surface's own light (kin_emissive).
#ifdef KIN_GI
// The probes' light here, as gi_resolve.comp read it on what the pre-pass drew, from the four texels round this pixel
// (at half its size), filtered: on what the pre-pass drew, those within `near` of this fragment's `depth` along the
// view (all four, most often), or where none is (a thin thing between texels) the one nearest in depth; on what it did
// not draw (glass, in front of what it shows) all of them with light. Weighed, not picked, so that a texel coming or
// going as the view's jitter moves does not make the light jump. For the way `n` (this surface's normal, its normal
// map's) for the diffuse (`probed`) and `r` for the reflections (`traced`). False where the probes have no light for it
// (the sky's stands).
// With area lights (frame.gi.y), `area`: their irradiance over pi facing the surface (else 0).
bool kin_gi_resolved(vec3 n, vec3 r, float depth, float near, out vec3 probed, out vec3 traced, out float coverage,
                     out vec3 area) {
    const ivec2 size = textureSize(uGiLight, 0);
    const vec2 uv = gl_FragCoord.xy * 0.5 / vec2(size);
    // x, y, z, w: the texels at (0, 1), (1, 1), (1, 0) and (0, 0) from the footprint's first.
    const vec4 depths = textureGather(uGiLight, uv, 3);
    const bool drawn = material.maps.z > 0.5;
    vec4 light, slope, shares;
    coverage = 0.0;
    area = vec3(0.0);
    if (all(greaterThan(depths, vec4(0.0))) && (!drawn || all(lessThanEqual(abs(depths - depth), vec4(near))))) {
        light = textureLod(uGiLight, uv, 0.0);
        slope = textureLod(uGiSlope, uv, 0.0);
        shares = textureLod(uGiCoverage, uv, 0.0);
    } else {
        const vec2 at = uv * vec2(size) - 0.5;
        const vec2 f = fract(at);
        const vec4 weight = vec4((1.0 - f.x) * f.y, f.x * f.y, f.x * (1.0 - f.y), (1.0 - f.x) * (1.0 - f.y));
        const ivec2 offsets[4] = ivec2[4](ivec2(0, 1), ivec2(1, 1), ivec2(1, 0), ivec2(0, 0));
        const ivec2 base = ivec2(floor(at));
        int nearest_one = -1;
        float nearest = 1e30, total = 0.0;
        light = vec4(0.0);
        slope = vec4(0.0);
        shares = vec4(0.0);
        for (int i = 0; i < 4; ++i) {
            if (depths[i] == 0.0) {
                continue; // no surface
            }
            const float off = abs(abs(depths[i]) - depth);
            if (off < nearest) {
                nearest = off;
                nearest_one = i;
            }
            if (depths[i] > 0.0 && (!drawn || off <= near)) {
                const ivec2 q = clamp(base + offsets[i], ivec2(0), size - 1);
                light += weight[i] * texelFetch(uGiLight, q, 0);
                slope += weight[i] * texelFetch(uGiSlope, q, 0);
                shares += weight[i] * texelFetch(uGiCoverage, q, 0);
                total += weight[i];
            }
        }
        if (total > 0.0) {
            light /= total;
            slope /= total;
            shares /= total;
        } else if (nearest_one >= 0 && depths[nearest_one] > 0.0) {
            const ivec2 q = clamp(base + offsets[nearest_one], ivec2(0), size - 1);
            light = texelFetch(uGiLight, q, 0);
            slope = texelFetch(uGiSlope, q, 0);
            shares = texelFetch(uGiCoverage, q, 0);
        } else {
            return false;
        }
    }
    coverage = frame.gi.y > 0.5 ? shares.a : shares.r;
    // The area lights' light fades out with the probes' toward their windows' edges (nothing found past them).
    area = frame.gi.y > 0.5 ? shares.rgb * clamp(shares.a, 0.0, 1.0) : vec3(0.0);
    probed = light.rgb * clamp(slope.w + dot(slope.xyz, n), 0.0, 4.0);
    traced = light.rgb * clamp(slope.w + dot(slope.xyz, r), 0.0, 4.0);
    return true;
}
#endif

// Whether the surface lets light through from behind (Surface::backlight): lit, and shadowed, from either side.
bool kin_backlit(Surface s) {
    return dot(s.backlight, vec3(1.0)) > 0.0;
}

#ifdef KIN_LIGHTS
// A point light's light on the surface: the inverse square, windowed to nothing at the radius (Karis' fall-off, as
// glTF's punctual lights), no brighter than at the light's own size (colour.w: its square), so a surface inside a
// flash is lit as at its edge, not without bound; its shadow from its cube (`first`: the cube's first layer, -1:
// none). Zero attenuation contributes nothing, including to the specular BRDF.
vec3 kin_point_light(Surface s, vec3 n, vec3 offset_normal, vec4 at, vec4 colour, float first) {
    const vec3 to = at.xyz - vWorld;
    const float d2 = dot(to, to);
    const float r2 = d2 / (at.w * at.w);
    const float window = clamp(1.0 - r2 * r2, 0.0, 1.0);
    const float k = window * window / max(d2, colour.w);
    if (k <= 0.0) {
        return vec3(0.0);
    }
    const vec3 toward = to * inversesqrt(max(d2, 1e-8));
    float seen = 1.0;
    // (A back-lit thin surface's shadow is its lit side's: from a point pushed off the other way.)
    const bool behind = dot(n, toward) <= 0.0;
    const vec3 through_offset = behind && kin_backlit(s) ? -offset_normal : offset_normal;
    if (material.params.z > 0.5
#ifndef KIN_DIRECT_LIGHT
        && (!behind || kin_backlit(s))
#endif
    ) {
        seen = point_light_shadow_from(first, through_offset, toward, sqrt(d2));
    }
#ifdef KIN_DIRECT_LIGHT
    return kin_direct_light(s, toward, colour.rgb * k, seen, KIN_LIGHT_POINT, at.xyz, sqrt(d2));
#else
    return shade(s, toward, colour.rgb * (k * seen));
#endif
}

// A spot light's: a point light's falloff inside a cone (glTF's punctual spot: the square of the linear step of the
// cosine between the inner and the outer angle; axis.w the outer's, more.x 1 / (cos inner - cos outer)), its shadow
// from its layer of the local array (more.y, -1: none).
vec3 kin_spot_light(Surface s, vec3 n, vec3 offset_normal, vec4 at, vec4 colour, vec4 axis, vec4 more) {
    const vec3 to = at.xyz - vWorld;
    const float d2 = dot(to, to);
    const float r2 = d2 / (at.w * at.w);
    const float window = clamp(1.0 - r2 * r2, 0.0, 1.0);
    const vec3 toward = to * inversesqrt(max(d2, 1e-8));
    const float step_in_cone = clamp((dot(-toward, axis.xyz) - axis.w) * more.x, 0.0, 1.0);
    const float k = window * window * step_in_cone * step_in_cone / max(d2, colour.w);
    if (k <= 0.0) {
        return vec3(0.0);
    }
    float seen = 1.0;
    const bool behind = dot(n, toward) <= 0.0;
    const vec3 through_offset = behind && kin_backlit(s) ? -offset_normal : offset_normal;
    if (more.y >= 0.0 && material.params.z > 0.5
#ifndef KIN_DIRECT_LIGHT
        && (!behind || kin_backlit(s))
#endif
    ) {
        seen = local_shadow(int(more.y), through_offset, toward, sqrt(d2));
    }
#ifdef KIN_DIRECT_LIGHT
    return kin_direct_light(s, toward, colour.rgb * k, seen, KIN_LIGHT_SPOT, at.xyz, sqrt(d2));
#else
    return shade(s, toward, colour.rgb * (k * seen));
#endif
}
#endif

#if defined(KIN_GI) && defined(KIN_LIGHT_GRID)
// An area light's reflection (Heitz et al.'s linearly transformed cosines, three.js's horizon-clipped form): the
// tables' bilinear fetch, an edge's integral, and a polygon's form from its corners' unit directions (in the
// surface's frame through LTC's inverse for the GGX lobe; in world space for the diffuse form factor). Wound
// counter-clockwise as seen from the fragment.
vec4 kin_ltc_fetch(int table, float roughness, float n_v) {
    const vec2 at = vec2(clamp(roughness, 0.0, 1.0), sqrt(1.0 - clamp(n_v, 0.0, 1.0))) * 63.0;
    const ivec2 i = ivec2(floor(at)), j = min(i + 1, ivec2(63));
    const vec2 f = at - vec2(i);
    const int base = table * 4096;
    const vec4 a = kin_area_ltc[base + i.y * 64 + i.x], b = kin_area_ltc[base + i.y * 64 + j.x];
    const vec4 c = kin_area_ltc[base + j.y * 64 + i.x], d = kin_area_ltc[base + j.y * 64 + j.x];
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
vec3 kin_ltc_edge(vec3 v1, vec3 v2) {
    const float x = dot(v1, v2), y = abs(x);
    const float a = 0.8543985 + (0.4965155 + 0.0145206 * y) * y, b = 3.4175940 + (4.1616724 + y) * y;
    const float v = a / b;
    const float theta_sintheta = x > 0.0 ? v : 0.5 * inversesqrt(max(1.0 - x * x, 1e-7)) - v;
    return cross(v1, v2) * theta_sintheta;
}
// The horizon-clipped form of the polygon whose corners are seen from the fragment along the unit directions c0..c3
// (c3 unused for a triangle), `up` the horizon's normal in their space.
float kin_ltc_form(vec3 c0, vec3 c1, vec3 c2, vec3 c3, bool triangle, vec3 up) {
    vec3 f = kin_ltc_edge(c0, c1) + kin_ltc_edge(c1, c2);
    if (triangle) {
        f += kin_ltc_edge(c2, c0);
    } else {
        f += kin_ltc_edge(c2, c3) + kin_ltc_edge(c3, c0);
    }
    const float l = length(f);
    return max((l * l + dot(f, up)) / (l + 1.0), 0.0);
}
// The area lights' reflection at the fragment: each emitter's GGX by its LTC, unshadowed, times the share of the
// diffuse light the exact pass found unshadowed (`shadowed`, gi_area.comp's, over the same lights' unshadowed form
// factors): one visibility for every emitter, exact where one light dominates.
vec3 kin_area_specular(Surface s, vec3 n, float roughness, vec3 shadowed) {
    const vec3 p = vWorld;
    vec3 t1 = s.v - n * dot(s.v, n);
    t1 = dot(t1, t1) > 1e-12 ? normalize(t1) : normalize(abs(n.y) < 0.9 ? cross(n, vec3(0, 1, 0)) : cross(n, vec3(1, 0, 0)));
    const vec3 t2 = -cross(n, t1);
    const mat3 frame_to = transpose(mat3(t1, t2, n));
    const vec4 m1 = kin_ltc_fetch(0, roughness, s.n_v), m2 = kin_ltc_fetch(1, roughness, s.n_v);
    // LTC's inverse transform in the surface's frame, with that frame's turn: one matrix for the corners.
    const mat3 to_ltc = mat3(vec3(m1.x, 0, m1.y), vec3(0, 1, 0), vec3(m1.z, 0, m1.w)) * frame_to;
    const vec3 fresnel = s.f0 * m2.x + (vec3(1.0) - s.f0) * m2.y;
    vec3 diffuse = vec3(0.0), specular = vec3(0.0);
    const uint emitters = uint(frame.gi.z + 0.5);
    for (uint e = 0u; e < emitters; ++e) {
        const vec4 E0 = kin_area_shading[e * 5u], E1 = kin_area_shading[e * 5u + 1u], E2 = kin_area_shading[e * 5u + 2u];
        const vec4 EN = kin_area_shading[e * 5u + 3u], ER = kin_area_shading[e * 5u + 4u];
        const bool triangle = E0.w > 0.5;
        if (EN.w < 0.5 && dot(p - E0.xyz, EN.xyz) <= 0.0) {
            continue; // behind a one-sided emitter
        }
        vec3 q0 = E0.xyz, q1 = E0.xyz + E1.xyz, q2 = E0.xyz + E1.xyz + E2.xyz, q3 = E0.xyz + E2.xyz;
        if (dot(cross(E1.xyz, E2.xyz), p - q0) < 0.0) {
            // Counter-clockwise from p: turn it round.
            const vec3 t = q1;
            q1 = triangle ? q2 : q3;
            if (triangle) {
                q2 = t;
            } else {
                q3 = t;
            }
        }
        if (triangle) {
            q3 = q2; // (a triangle has three corners)
        }
        // Wholly below the horizon: neither form sees any of it.
        if (max(max(dot(q0 - p, n), dot(q1 - p, n)), max(dot(q2 - p, n), dot(q3 - p, n))) <= 0.0) {
            continue;
        }
        const vec3 d0 = normalize(q0 - p), d1 = normalize(q1 - p), d2 = normalize(q2 - p), d3 = normalize(q3 - p);
        // The form factor, in world space: the surface's frame (t1, t2, n) is left-handed, so its z is -n here.
        diffuse += ER.rgb * kin_ltc_form(d0, d1, d2, d3, triangle, -n);
        specular += ER.rgb * kin_ltc_form(normalize(to_ltc * d0), normalize(to_ltc * d1), normalize(to_ltc * d2),
                                          normalize(to_ltc * d3), triangle, vec3(0.0, 0.0, 1.0));
    }
    const vec3 luma = vec3(0.2126, 0.7152, 0.0722);
    const float seen = clamp(dot(shadowed, luma) / max(dot(diffuse * frame.gi.w, luma), 1e-6), 0.0, 1.0);
    return specular * fresnel * s.boost * (frame.gi.w * seen);
}
#endif

// The light of the scene on a surface (kin_lit's), with its backlight and clear coat.
// The ambient light off a surface (`under`) beneath a clear coat of strength `coat` (none at 0): dimmed by what the
// coat reflects, plus that (its sky reflectance, by `ab`: environment_brdf at its roughness), as glTF's clear coat.
vec3 kin_coated(vec3 under, float coat, vec2 ab, float n_v, vec3 coat_above) {
    if (coat <= 0.0) {
        return under;
    }
    const float fc = coat * fresnel(vec3(0.04), n_v).x;
    return under * (1.0 - fc) + coat * sky_reflectance(vec3(0.04), ab) * coat_above;
}

vec3 kin_lit_surface(KinSurface surface) {
    const vec3 base = surface.base_color, n = surface.normal, backlight = surface.backlight;
    const float metallic = surface.metallic, occlusion = surface.occlusion, coat = surface.clearcoat;
    const float wrap = surface.diffuse_wrap, f0_dielectric = 0.08 * surface.specular;
    float roughness = surface.roughness, coat_roughness = surface.clearcoat_roughness;
    // Specular anti-aliasing (Scene3D::specular_aa; Tokuyoshi and Kaplanyan's, as Godot's roughness limiter): where
    // the normal turns within the pixel, a highlight can be thinner than the pixel, very bright where a sample hits it
    // and missed by the next; the roughness is raised by how much the normal turns across the pixel (at most to an
    // alpha of 0.18 more), spreading the highlight over it.
    if (frame.environment.z > 0.0) {
        const vec3 dn_dx = dFdx(n), dn_dy = dFdy(n);
        const float widen = min(2.0 * frame.environment.z * (dot(dn_dx, dn_dx) + dot(dn_dy, dn_dy)), 0.18);
        roughness = sqrt(min(1.0, roughness * roughness + widen));
        coat_roughness = sqrt(min(1.0, coat_roughness * coat_roughness + widen));
    }
    const vec3 v = kin_view();
    Surface s;
    s.n = n;
    s.v = v;
    s.n_v = clamp(dot(n, v), 0.0, 1.0);
    s.alpha = roughness * roughness;
    s.diffuse = base * (1.0 - metallic);
    s.f0 = mix(vec3(f0_dielectric), base, metallic);
    // A rough microsurface lights itself with a bounce that single
    // scattering leaves out, most of all a metal's (Fdez-Aguera's
    // multiple-scattering fit): put back in the specular, taken from the diffuse.
    const vec2 ab = environment_brdf(roughness, s.n_v);
    const float single = ab.x + ab.y;
    // (Not with Scene3D::specular_multiscatter off: frame.time.y.)
    const bool multiscatter = frame.time.y < 0.5;
    s.boost = multiscatter ? vec3(1.0) + s.f0 * (1.0 / max(single, 1e-3) - 1.0) : vec3(1.0);
    s.backlight = backlight;
    s.coat = coat;
    s.coat_alpha = coat_roughness * coat_roughness;
    s.dielectric = f0_dielectric;
    s.wrap = wrap;
    const bool backlit = kin_backlit(s);

    vec3 light = vec3(0.0);
    // Compute derivatives before the varying sun-facing branch as well as
    // cascade selection/blending and map-edge exits.
    vec3 world_dx = vec3(0.0), world_dy = vec3(0.0);
    if (frame.shadow.y > 0.5) { world_dx = dFdx(vWorld); world_dy = dFdy(vWorld); }
#ifdef KIN_LIGHTS
    const vec3 offset_normal = shadow_offset_normal(n); // the moon's and the local lights' shadows, as the sun's
#endif
    const float sun = dot(n, frame.sun_direction.xyz);
#ifdef KIN_DIRECT_LIGHT
    if (dot(frame.sun_color.rgb, vec3(1.0)) > 0.0) {
#else
    if (sun > 0.0 || backlit || wrap > 0.0) {
#endif
        // (A back-lit thin surface's shadow from its side facing the sun: the point pushed off that way.)
        float lit = sunlight(sun > 0.0 || !backlit ? n : -n, world_dx, world_dy);
#ifdef KIN_LIGHTS
        lit = mix(1.0, lit, locals.counts.w);
#endif
        if (frame.occlusion.y > 0.5) {
            // On top of the shadow map's; only the sun's term, and faded with it by sun_shadow_strength (without the
            // lights variant the strength is 1: a strength of 0 turns the contact pass off before the shader).
#ifdef KIN_LIGHTS
            lit *= contact_shadow() * locals.counts.w + (1.0 - locals.counts.w); // exact at 0 and 1, mix() may not be
#else
            lit *= contact_shadow();
#endif
        }
#ifdef KIN_DIRECT_LIGHT
        light += kin_direct_light(s, frame.sun_direction.xyz, frame.sun_color.rgb, lit, KIN_LIGHT_SUN, vec3(0.0), 0.0);
#else
        light += shade(s, frame.sun_direction.xyz, frame.sun_color.rgb * lit);
#endif
    }
    if (dot(frame.fill_color.rgb, vec3(1.0)) > 0.0) {
#ifdef KIN_DIRECT_LIGHT
        light += kin_direct_light(s, frame.fill_direction.xyz, frame.fill_color.rgb, moonlight(offset_normal), KIN_LIGHT_FILL, vec3(0.0), 0.0);
#elif defined(KIN_LIGHTS)
        light += shade(s, frame.fill_direction.xyz, frame.fill_color.rgb * moonlight(offset_normal));
#else
        light += shade(s, frame.fill_direction.xyz, frame.fill_color.rgb);
#endif
    }
#ifdef KIN_LIGHT_GRID
    if (frame.grid.x > 0.5) {
        // The cell: the fragment's tile, and its slice of the view depth (as the cascades measure it).
        const uvec3 size = uvec3(frame.grid_size.xyz);
        const uvec2 tile = min(uvec2(gl_FragCoord.xy / frame.grid.y), size.xy - 1u);
        const float depth = max(dot(vWorld - frame.eye.xyz, frame.cascade_forward.xyz), frame.grid.z);
        const uint slice = min(uint(log(depth / frame.grid.z) * frame.grid.w), size.z - 1u);
        const uint cell = grid_cells[(slice * size.y + tile.y) * size.x + tile.x];
        const uint first = uint(frame.grid_size.w) + (cell >> 12);
        const uint count = cell & 0xfffu;
        for (uint j = 0u; j < count; ++j) {
            const KinGridLight g = grid_lights[grid_cells[first + j]];
            light += g.axis.w < -1.5 ? kin_point_light(s, n, offset_normal, g.at, g.colour, g.cone.y)
                                     : kin_spot_light(s, n, offset_normal, g.at, g.colour, g.axis, g.cone);
        }
    } else {
#endif
    const int count = int(frame.flags.w);
    for (int i = 0; i < count; ++i) {
#ifdef KIN_LIGHTS
        light += kin_point_light(s, n, offset_normal, frame.lights[2 * i], frame.lights[2 * i + 1],
                                 locals.point_shadow[i].x);
#else
        const vec4 at = frame.lights[2 * i];
        const vec3 to = at.xyz - vWorld;
        const float d2 = dot(to, to);
        // The inverse square, windowed to nothing at the radius (Karis'
        // fall-off, as glTF's punctual lights); no brighter than at the
        // light's own size (w: its square), so a surface inside a flash
        // is lit as at its edge, not without bound.
        const float r2 = d2 / (at.w * at.w);
        const float window = clamp(1.0 - r2 * r2, 0.0, 1.0);
        const float k = window * window / max(d2, frame.lights[2 * i + 1].w);
        // Zero attenuation contributes nothing, including to the specular BRDF.
        if (k <= 0.0) {
            continue;
        }
        light += shade(s, to * inversesqrt(max(d2, 1e-8)), frame.lights[2 * i + 1].rgb * k);
#endif
    }
#ifdef KIN_LIGHTS
    // Spot lights (the fixed block's): kin_spot_light.
    const int spot_count = int(locals.counts.x);
    for (int i = 0; i < spot_count; ++i) {
        light += kin_spot_light(s, n, offset_normal, locals.spots[4 * i], locals.spots[4 * i + 1],
                                locals.spots[4 * i + 2], locals.spots[4 * i + 3]);
    }
#endif
#ifdef KIN_LIGHT_GRID
    }
#endif

    // Ambient, from the sky above to the ground below by which way a
    // direction looks: the diffuse by the normal, the specular by the
    // reflection (turned toward the normal as the surface roughens: a
    // rough one sees a wide cone of the sky); or from the environment: the
    // diffuse from its irradiance, the specular from its level for the roughness.
    // A back-lit surface's light from behind (`behind`, by -n) goes through it by its backlight, as Unreal's foliage's.
    // A clear coat's reflection (`coat_above`) by its own roughness.
    vec3 around, above, behind = vec3(0.0), coat_above = vec3(0.0);
    if (frame.environment.w > 0.5) {
        around = textureLod(uEnvIrradiance, environment_uv(n), 0.0).rgb * frame.environment.x;
        if (backlit || wrap > 0.5) {
            behind = textureLod(uEnvIrradiance, environment_uv(-n), 0.0).rgb * frame.environment.x;
        }
        if (coat > 0.0) {
            coat_above = kin_env_specular(environment_uv(reflect(-v, n)), coat_roughness * frame.environment.y).rgb *
                         frame.environment.x;
        }
        above = kin_env_specular(environment_uv(reflect(-v, n)), roughness * frame.environment.y).rgb * frame.environment.x;
    } else {
        const vec3 reflected = normalize(mix(reflect(-v, n), n, s.alpha));
        around = mix(frame.ground_ambient.rgb, frame.ambient.rgb, n.y * 0.5 + 0.5);
        behind = mix(frame.ground_ambient.rgb, frame.ambient.rgb, -n.y * 0.5 + 0.5);
        const vec3 coat_reflected = normalize(mix(reflect(-v, n), n, s.coat_alpha));
        coat_above = mix(frame.ground_ambient.rgb, frame.ambient.rgb, coat_reflected.y * 0.5 + 0.5);
        above = mix(frame.ground_ambient.rgb, frame.ambient.rgb, reflected.y * 0.5 + 0.5);
    }
#ifdef KIN_GI
    vec3 area_light = vec3(0.0); // the area lights' (frame.gi.y), a direct light's: not occluded, not the probes'
    // With global illumination's probes the diffuse is theirs: the sky's light where they see it, the light of every
    // surface they see, kept out by walls. The reflections too, in so far as a rough surface's are its surroundings'
    // light along the reflection, blurred: what the probes see that way; a smooth surface keeps the sky's sharp
    // reflection, dimmed by how much of the sky's light the probes see that way (none in a closed room).
    if (frame.gi.x > 0.5) {
        const vec3 r = normalize(mix(reflect(-v, n), n, s.alpha));
        const float depth = dot(vWorld - frame.eye.xyz, frame.cascade_forward.xyz);
        const float near = 0.02 * depth + 4.0 * fwidth(depth);
        vec3 probed, traced;
        float coverage;
        if (kin_gi_resolved(n, r, depth, near, probed, traced, coverage, area_light)) {
            around = mix(around, probed, coverage);
            behind = mix(behind, probed, coverage); // (the probes' for the front: none from the sky through a roof)
            const vec3 open = frame.environment.w > 0.5
                                  ? textureLod(uEnvIrradiance, environment_uv(r), 0.0).rgb * frame.environment.x
                                  : mix(frame.ground_ambient.rgb, frame.ambient.rgb, r.y * 0.5 + 0.5);
            const vec3 luma = vec3(0.2126, 0.7152, 0.0722);
            const float seen = clamp(dot(traced, luma) / max(dot(open, luma), 1e-6), 0.0, 1.0);
            const float smooth_share = (1.0 - roughness) * (1.0 - roughness);
            above = mix(above, mix(traced, above * seen, smooth_share), coverage);
            const float coat_smooth = (1.0 - coat_roughness) * (1.0 - coat_roughness);
            coat_above = mix(coat_above, mix(traced, coat_above * seen, coat_smooth), coverage);
        }
    }
#endif
#ifdef KIN_LIGHT_GRID
    // The reflection probes', in place of the environment's (and the GI's) where their boxes reach.
    float own_probe = 0.0; // the share a thing's own probe took (the screen-space reflections leave it)
    if (frame.probes.x > 0.5) {
        const vec3 r = reflect(-v, n);
        const vec4 probed = kin_probe_reflection(vWorld, r, roughness * frame.probes.y, own_probe);
        above = probed.rgb + above * (1.0 - probed.a);
        if (coat > 0.0) {
            float coat_own;
            const vec4 coat_probed = kin_probe_reflection(vWorld, r, coat_roughness * frame.probes.y, coat_own);
            coat_above = coat_probed.rgb + coat_above * (1.0 - coat_probed.a);
        }
    }
#endif
#if defined(KIN_LIGHT_GRID) && defined(KIN_GI)
    // Screen-space reflections, where the trace found something on screen, in place of the probes' or the environment's
    // (none past the roughness Scene3D::ssr says).
    // Where the surface's own shape turns fast across the screen (a small curved thing) its neighbours' hits do not
    // stand for its own: those keep the probes' (a shiny ball's own). Its normal map's turns do not count (the reflection
    // follows them). (Measured outside the branch: derivatives need every pixel of the quad.)
    const float bend = length(fwidth(dot(vNormal, vNormal) > 1e-20 ? normalize(vNormal) : vec3(0.0)));
    const vec3 flat_normal = kin_geometric_normal();
    if (frame.ssr.x > 0.5 && roughness < frame.ssr.w) {
        const float depth = dot(vWorld - frame.eye.xyz, frame.cascade_forward.xyz);
        const vec3 r = reflect(-v, n);
        const vec4 screen = kin_ssr_reflection(depth, vWorld, r, roughness);
        // The hit's distance was found along the face's own reflection: followed along one the normal map turned much
        // further (past about 20 degrees) it is no guide, and those keep the probes'.
        const float follows = smoothstep(0.8, 0.95, dot(r, reflect(-v, flat_normal)));
        const float share = screen.a * (1.0 - smoothstep(0.75 * frame.ssr.w, frame.ssr.w, roughness)) *
                            (1.0 - smoothstep(0.02, 0.08, bend)) * (1.0 - own_probe) * follows;
        above = mix(above, screen.rgb, share);
    }
#endif
    // A reflection of the surface's own (a mirror's picture: kin_planar_reflect), in place of the environment's or the
    // probes' by its share. The Fresnel, the roughness and the specular occlusion weigh it as they would theirs.
    if (surface.reflection.a > 0.0) {
        above = mix(above, surface.reflection.rgb, surface.reflection.a);
    }
    // A diffuse wrapped past 0.5 sees the ambient light less by its normal: toward the mean of the two sides'.
    if (wrap > 0.5) {
        around = mix(around, 0.5 * (around + behind), 2.0 * wrap - 1.0);
    }
    const vec3 dielectric = multiscatter ? sky_reflectance(vec3(f0_dielectric), ab) : vec3(f0_dielectric * ab.x + ab.y);
    const vec2 coat_ab = coat > 0.0 ? environment_brdf(coat_roughness, s.n_v) : vec2(0.0);
    const vec3 reflectance = mix(dielectric, multiscatter ? sky_reflectance(base, ab) : base * ab.x + ab.y, metallic);
#ifdef KIN_GI
    light += s.diffuse * (vec3(1.0) - dielectric) * area_light;
#ifdef KIN_LIGHT_GRID
    if (frame.gi.y > 0.5 && frame.gi.z > 0.5) {
        light += kin_area_specular(s, n, roughness, area_light);
    }
#endif
#endif
    if (frame.occlusion.x > 0.5 && material.maps.z > 0.5) {
        // Ambient occlusion: the diffuse light by Jimenez et al.'s multi-bounce
        // fit (light bounces between nearby surfaces, the more the brighter
        // they are, so a pale crease is not as dark as a dark one), the
        // reflections by Lagarde's specular occlusion (a smooth surface seen
        // head on is hidden less than a rough one at a slant). Returned from
        // here, so the arithmetic without it is the same as it always was.
        const float ao = screen_occlusion();
        const vec3 a = 2.0404 * base - vec3(0.3324);
        const vec3 b = -4.7951 * base + vec3(0.6417);
        const vec3 c = 2.7552 * base + vec3(0.6903);
        const vec3 diffuse_light = s.diffuse * (vec3(1.0) - dielectric) * around * max(vec3(ao), ((ao * a + b) * ao + c) * ao);
        const vec3 specular_light = reflectance * above * clamp(pow(s.n_v + ao, exp2(-16.0 * roughness - 1.0)) - 1.0 + ao, 0.0, 1.0);
        return light + kin_coated(diffuse_light + specular_light, coat, coat_ab, s.n_v, coat_above) * occlusion +
               backlight * behind * (ao * occlusion);
    }
    return light + kin_coated(s.diffuse * (vec3(1.0) - dielectric) * around + reflectance * above, coat, coat_ab, s.n_v,
                              coat_above) * occlusion +
           backlight * behind * occlusion;
}

vec3 kin_lit(vec3 base, vec3 n, float metallic, float roughness, float occlusion) {
    KinSurface s;
    s.base_color = base;
    s.alpha = 1.0;
    s.normal = n;
    s.metallic = metallic;
    s.roughness = roughness;
    s.occlusion = occlusion;
    s.emission = vec3(0.0);
    s.backlight = vec3(0.0);
    s.clearcoat = 0.0;
    s.clearcoat_roughness = 0.045;
    s.specular = 0.5;
    s.diffuse_wrap = 0.0;
    s.reflection = vec4(0.0);
    return kin_lit_surface(s);
}

// What a shader writes: `colour` (linear) encoded for the target, with its
// alpha, premultiplied as kin's targets are.
#ifdef KIN_FOG
// The sky's light along `dir` for the fog's colour: kin_sky_scatter's, read from the fog table (two rows, linearly) rather
// than integrated at each fragment: the integral's part that depends on the angle to the light is its phase functions,
// worked out here; the rest depends on the ray's elevation alone, and is the table's.
KIN_FOG_TABLE_READER(kin_fog_sky, fog_params.sky, fog_params.table)

// `colour` as seen through the fog from the camera to this fragment: mixed toward the fog's colour by its opacity (a
// blended surface fades the same way, its alpha applied after), or, for an additive one, only dimmed (what it lies over
// has the fog already).
vec3 kin_fog_apply(vec3 colour) {
#ifdef KIN_CUSTOM_MATERIAL
    if (fog_params.fog.view.y < 0.5)
        return colour;
#endif
    const vec3 to = vWorld - frame.eye.xyz;
    float dist = length(to);
    vec3 dir = dist > 1e-6 ? to / dist : vec3(0.0, 0.0, -1.0);
    if (fog_params.fog.view.x > 0.5) {
        dir = frame.cascade_forward.xyz;
        dist = dot(to, dir);
    }
    const float tau = kin_fog_optical_depth(fog_params.fog, frame.eye.xyz, dir, dist, false);
    const float opacity = kin_fog_opacity(fog_params.fog, tau);
    if (opacity <= 0.0) {
        return colour;
    }
    if (material.maps.w > 0.5) {
        return colour * (1.0 - opacity);
    }
    vec3 fog_colour = fog_params.fog.colour.rgb;
    if (fog_params.fog.colour.w > 0.5) {
        fog_colour = kin_fog_sky(dir);
    }
    // With Fog3D::sun_shadows (fog.view.z) the sun's in-scattering is added after the main pass, through the shadows
    // (fog_sun_apply.frag), once a pixel.
    const vec3 sun = fog_params.fog.view.z > 0.5
                         ? vec3(0.0)
                         : kin_fog_sun_light(fog_params.fog, dir, frame.sun_direction.xyz, frame.sun_color.rgb);
    return mix(colour, fog_colour + sun, opacity);
}
#endif

vec4 kin_output(vec3 colour, float alpha) {
#ifdef KIN_CUSTOM_MATERIAL
    if (material.params.w > 0.0) {
        if (alpha < material.params.w) discard;
        alpha = 1.0;
    }
#endif
#ifdef KIN_FOG
    colour = kin_fog_apply(colour);
#endif
    if (frame.flags.x > 0.5) {
        colour = encode(colour);
    }
    return vec4(colour * alpha, alpha);
}

#ifdef KIN_CUSTOM_MATERIAL
// Explicitly bypass fog while retaining the target encoding and cutout contract.
vec4 kin_output_unfogged(vec3 colour, float alpha) {
    if (material.params.w > 0.0) {
        if (alpha < material.params.w) discard;
        alpha = 1.0;
    }
    if (frame.flags.x > 0.5) colour = encode(colour);
    return vec4(colour * alpha, alpha);
}
#endif
