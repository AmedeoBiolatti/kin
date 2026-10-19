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
layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec4 vColor; // linear, straight alpha
layout(location = 2) in vec2 vUV;
layout(location = 3) in vec3 vWorld;

layout(set = 2, binding = 0) uniform sampler2D uBase;         // white when the material has none
layout(set = 2, binding = 1) uniform sampler2DArrayShadow uShadow; // depth from the sun, a layer a cascade, compared
layout(set = 2, binding = 2) uniform sampler2D uMetalRough;   // G roughness, B metallic: white without
layout(set = 2, binding = 3) uniform sampler2D uNormalMap;    // tangent space; any without (see maps.x)
layout(set = 2, binding = 4) uniform sampler2D uOcclusion;    // R: white without
layout(set = 2, binding = 5) uniform sampler2D uEmissive;     // any without (see maps.y)
layout(set = 2, binding = 6) uniform sampler2D uEnvSpecular;   // the environment's, a level a roughness (with frame.environment.w)
layout(set = 2, binding = 7) uniform sampler2D uEnvIrradiance; // and what a diffuse surface facing each way receives, over pi
layout(set = 2, binding = 8) uniform sampler2D uAmbientOcclusion; // R: the share of ambient light that reaches the view's pixels (white without)
layout(set = 2, binding = 9) uniform sampler2D uContactShadow;    // R: the share of the sun that gets through a short march toward it (white without)
#ifdef KIN_LIGHTS
// Spot and point lights' shadows and the moon's (P3b): only the shaders made with KIN_LIGHTS have them, so that every other
// shader, and its compiled code, is as it was.
layout(set = 2, binding = 10) uniform sampler2DArrayShadow uLocalShadow; // depth from a spot (a layer), or a face of a point light's cube (six layers), compared
layout(set = 2, binding = 11) uniform sampler2DArrayShadow uMoonShadow;  // depth from the fill light (the moon), a layer a cascade, compared
#endif

const float PI = 3.14159265358979;
#include "kin_material_uniforms.inc.glsl"

// Fog (Scene3D::fog): only the shaders made with KIN_FOG have it (mesh_fog.frag, mesh_velocity_fog.frag), so that every
// other shader, and its compiled code, is as it was. Its block takes the slot custom parameters have elsewhere.
#ifdef KIN_FOG
#ifndef KIN_NO_CUSTOM_PARAMS
#error "KIN_FOG takes the uniform slot custom parameters use"
#endif
#include "kin_fog.inc.glsl"
layout(set = 3, binding = 2) uniform FogParams {
    KinFog fog;
    KinSky sky;
    vec4 table[KIN_FOG_ROWS * 4]; // the sky's light for colour_from_sky (kin_fog_sky), made when the sky changes
} fog_params;
#endif

// Spot lights and the shadows of lights that are not the sun (P3b): KIN_LIGHTS (mesh_light.frag and its variants). Its block
// takes the slot after the fog's (or, without fog, the fog's own: custom parameters' too, as the two never meet in one draw).
#ifdef KIN_LIGHTS
#ifndef KIN_NO_CUSTOM_PARAMS
#error "KIN_LIGHTS takes the uniform slot custom parameters use"
#endif
#ifdef KIN_FOG
#define KIN_LIGHTS_BINDING 3
#else
#define KIN_LIGHTS_BINDING 2
#endif
const int MAX_SPOTS = 16;
const int MAX_LOCAL = 20; // layers: 8 spots' maps, then two cubes of six faces
layout(set = 3, binding = KIN_LIGHTS_BINDING) uniform LocalLights {
    vec4 counts;      // x: spot lights, y: the moon's cascades (0: no shadow), z: the moon's shadow strength, w: the sun's
    vec4 local;       // x: 1 / the local array's side (a texel in uv)
    vec4 moon;        // x: a texel of the moon's map (uv)
    mat4 moon_matrix[4];
    vec4 moon_bias[4]; // per cascade: x: depth bias, y: normal offset (world units)
    vec4 moon_end;     // the view depth where each cascade ends
    vec4 moon_blend;   // and where it starts to fade into the next
    vec4 spots[MAX_SPOTS * 4]; // per spot: position.xyz radius; colour.rgb size^2; direction.xyz cos(outer); x: 1 / (cos inner - cos outer), y: its layer (-1: no shadow)
    vec4 point_shadow[MAX_LIGHTS]; // per point light (as frame.lights): x: its cube's first layer (-1: no shadow)
    mat4 local_matrix[MAX_LOCAL];  // world to the layer's clip space
    vec4 local_a[MAX_LOCAL];       // x: the map's share of the array's side, y: a texel's world size per unit of depth, z: near, w: far
    vec4 local_b[MAX_LOCAL];       // x: bias (texels), y: normal offset (texels), z: filter spacing (texels), w: where the shadow has faded to nothing (distance)
} locals;
#endif

// Custom material shaders (Material3D::shader) include this file and write
// main() with the functions at its end; they declare nothing else. Their
// four vec4 of parameters (Material3D::shader_params) are kin_param(0..3);
// mesh.frag, which has none, defines KIN_NO_CUSTOM_PARAMS first.
#ifndef KIN_NO_CUSTOM_PARAMS
layout(set = 3, binding = 2) uniform CustomParams {
    vec4 values[4];
} custom;
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

// The share of the sun one cascade lets through: 3x3 comparisons against its
// layer of the shadow map, from a point pushed a little off the surface (no
// self-shadowing acne). Reverse Z: the map keeps the depth nearest the sun,
// the greatest. -1 where the point is outside the cascade's map.
float cascade_light(int k, vec3 n, vec3 world_dx, vec3 world_dy) {
    const bool receiver_plane = frame.shadow.y > 0.5;
    const vec3 offset_normal = shadow_offset_normal(n);
    const vec4 clip = frame.shadow_matrix[k] * vec4(vWorld + offset_normal * frame.cascade_bias[k].y, 1.0);
    const vec3 ndc = clip.xyz / clip.w;
    const vec2 uv = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0 || ndc.z < 0.0 || ndc.z > 1.0) {
        return -1.0;
    }
    vec2 gradient = vec2(0.0);
    if (receiver_plane) {
        // Directional maps are orthographic. Transform world derivatives with
        // the selected matrix rather than differentiating across cascade IDs.
        const vec3 dx = (frame.shadow_matrix[k] * vec4(world_dx, 0.0)).xyz;
        const vec3 dy = (frame.shadow_matrix[k] * vec4(world_dy, 0.0)).xyz;
        const vec2 ux = dx.xy * vec2(0.5, -0.5), uy = dy.xy * vec2(0.5, -0.5);
        const float determinant = ux.x * uy.y - ux.y * uy.x;
        if (abs(determinant) > 1e-12) {
            gradient = vec2(dx.z * uy.y - dy.z * ux.y, ux.x * dy.z - uy.x * dx.z) / determinant;
        }
    }
    // Bilinear comparisons span two texel centers on each axis. Their reach
    // depends on subtexel phase and can approach a full texel, not just half.
    // All integer-offset PCF taps share this phase. Bound the greatest plane
    // depth at their four centers without lifting the whole receiver.
    const vec2 phase = fract(uv / frame.shadow.z - 0.5);
    const float filter_bias = frame.shadow.z * (dot(max(gradient, vec2(0.0)), 1.0 - phase)
                                               + dot(max(-gradient, vec2(0.0)), phase));
    const float ref = ndc.z + frame.cascade_bias[k].x + filter_bias;
    float lit = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            const vec2 tap = vec2(x, y) * frame.shadow.z;
            lit += texture(uShadow, vec4(uv + tap, float(k), ref + dot(gradient, tap)));
        }
    }
    return lit / 9.0;
}

// How much of the sun reaches here. With cascades, the one whose slice of
// the view holds this depth, faded into the next over the last tenth of its
// range (so the border isn't a visible seam); one map is just cascade 0.
float sunlight(vec3 n, vec3 world_dx, vec3 world_dy) {
    if (frame.flags.z < 0.5 || material.params.z < 0.5) {
        return 1.0;
    }
    const int count = int(frame.shadow.x);
    if (count <= 1) {
        const float one = cascade_light(0, n, world_dx, world_dy);
        return one < 0.0 ? 1.0 : one;
    }
    const float depth = dot(vWorld - frame.eye.xyz, frame.cascade_forward.xyz);
    int k = 0;
    for (int i = 0; i < count - 1; ++i) {
        k += depth > frame.cascade_end[i] ? 1 : 0;
    }
    float lit = cascade_light(k, n, world_dx, world_dy);
    if (k < count - 1 && depth > frame.cascade_blend[k]) {
        const float t = clamp((depth - frame.cascade_blend[k]) / max(frame.cascade_end[k] - frame.cascade_blend[k], 1e-4), 0.0, 1.0);
        const float next = cascade_light(k + 1, n, world_dx, world_dy);
        // Outside one's map (its fit stops at the area), the other's word stands.
        lit = lit < 0.0 ? next : next < 0.0 ? lit : mix(lit, next, t);
    }
    return lit < 0.0 ? 1.0 : lit;
}

// Custom surface shaders can sample sunlight directly. Call outside varying
// branches so receiver-plane derivatives are defined across the pixel quad.
float sunlight(vec3 n) {
    vec3 world_dx = vec3(0.0), world_dy = vec3(0.0);
    if (frame.shadow.y > 0.5) { world_dx = dFdx(vWorld); world_dy = dFdy(vWorld); }
    return sunlight(n, world_dx, world_dy);
}

#ifdef KIN_LIGHTS
// The share of a local light's shadow map that lets this surface point's light through (a spot's layer, or a face of a
// point light's cube): nine comparisons in the layer's corner of the array, from a point pushed a little off the surface,
// the reference moved toward the light by a bias of texels that grows with the surface's slope to it. Reverse Z, a
// perspective depth: depth(d) = near (far / d - 1) / (far - near). -1 where the point is outside the map's view.
// `offset_normal` is shadow_offset_normal's: the push and the slope are the mesh's own, not a normal map's.
float local_light(int layer, vec3 offset_normal, vec3 l) {
    const vec4 a = locals.local_a[layer];
    const vec4 b = locals.local_b[layer];
    const float texel_world = (locals.local_matrix[layer] * vec4(vWorld, 1.0)).w * a.y;
    const vec4 clip = locals.local_matrix[layer] * vec4(vWorld + offset_normal * (b.y * texel_world), 1.0);
    if (clip.w <= a.z) {
        return -1.0;
    }
    const vec2 ndc = clip.xy / clip.w;
    const vec2 uv = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0 || clip.w > a.w) {
        return -1.0;
    }
    const float n_l = clamp(dot(offset_normal, l), 0.0, 1.0);
    const float slope = min(sqrt(max(1.0 - n_l * n_l, 0.0)) / max(n_l, 0.05), 12.0);
    const float d = max(clip.w - texel_world * b.x * (1.0 + slope), a.z);
    const float ref = a.z * (a.w / d - 1.0) / (a.w - a.z);
    const float texel = locals.local.x;
    const vec2 base = uv * a.x;
    const vec2 lo = vec2(0.5 * texel), hi = vec2(a.x - 0.5 * texel);
    float lit = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            const vec2 q = clamp(base + vec2(x, y) * (b.z * texel), lo, hi);
            lit += texture(uLocalShadow, vec4(q, float(layer), ref));
        }
    }
    return lit / 9.0;
}

// Its share of the light, with the shadow faded out toward `b.w` from the light (1: no shadow, or not in the map's view).
float local_shadow(int layer, vec3 offset_normal, vec3 l, float distance) {
    float lit = local_light(layer, offset_normal, l);
    if (lit < 0.0) {
        return 1.0;
    }
    const float end = locals.local_b[layer].w;
    return mix(lit, 1.0, smoothstep(0.8 * end, end, distance));
}

// A point light's: the face its direction from the light falls in (the axis it is longest along), its layer.
float point_light_shadow(int i, vec3 offset_normal, vec3 toward, float distance) {
    const float first = locals.point_shadow[i].x;
    if (first < 0.0) {
        return 1.0;
    }
    const vec3 from_light = -toward;
    const vec3 m = abs(from_light);
    int face;
    if (m.x >= m.y && m.x >= m.z) {
        face = from_light.x < 0.0 ? 1 : 0;
    } else if (m.y >= m.z) {
        face = from_light.y < 0.0 ? 3 : 2;
    } else {
        face = from_light.z < 0.0 ? 5 : 4;
    }
    return local_shadow(int(first) + face, offset_normal, toward, distance);
}

// One of the moon's cascades, as cascade_light is the sun's; -1 outside its map.
float moon_cascade_light(int k, vec3 offset_normal) {
    const vec4 clip = locals.moon_matrix[k] * vec4(vWorld + offset_normal * locals.moon_bias[k].y, 1.0);
    const vec3 ndc = clip.xyz / clip.w;
    const vec2 uv = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0 || ndc.z < 0.0 || ndc.z > 1.0) {
        return -1.0;
    }
    const float ref = ndc.z + locals.moon_bias[k].x;
    float lit = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            lit += texture(uMoonShadow, vec4(uv + vec2(x, y) * locals.moon.x, float(k), ref));
        }
    }
    return lit / 9.0;
}

// How much of the moon reaches here: its cascade for this depth, faded into the next as the sun's are, then by its strength.
float moonlight(vec3 offset_normal) {
    const int count = int(locals.counts.y);
    if (count < 1 || material.params.z < 0.5) {
        return 1.0;
    }
    const float depth = dot(vWorld - frame.eye.xyz, frame.cascade_forward.xyz);
    int k = 0;
    for (int i = 0; i < count - 1; ++i) {
        k += depth > locals.moon_end[i] ? 1 : 0;
    }
    float lit = moon_cascade_light(k, offset_normal);
    if (k < count - 1 && depth > locals.moon_blend[k]) {
        const float t = clamp((depth - locals.moon_blend[k]) / max(locals.moon_end[k] - locals.moon_blend[k], 1e-4), 0.0, 1.0);
        const float next = moon_cascade_light(k + 1, offset_normal);
        lit = lit < 0.0 ? next : next < 0.0 ? lit : mix(lit, next, t);
    }
    return lit < 0.0 ? 1.0 : mix(1.0, lit, locals.counts.z);
}
#endif

// Where the equirectangular environment shows a direction (render3d/environment.hpp).
vec2 environment_uv(vec3 d) {
    d = mat3(frame.environment_matrix) * d;
    return vec2(0.5 + atan(d.z, d.x) / (2.0 * PI), 0.5 - asin(clamp(d.y, -1.0, 1.0)) / PI);
}

// The normal bent by the map. The tangent frame comes from how the world
// position and the UVs change across the pixel, so meshes need no tangents:
// the directions u and v increase in, solved from the two derivatives (the
// same on either way the screen's y runs). glTF's maps have green up and v
// runs down. `front` is -1 seen from behind, where the whole frame turns.
vec3 mapped_normal(vec3 n, float front) {
    const vec3 dp1 = dFdx(vWorld), dp2 = dFdy(vWorld);
    const vec2 duv1 = dFdx(vUV), duv2 = dFdy(vUV);
    const float det = duv1.x * duv2.y - duv2.x * duv1.y;
    if (abs(det) < 1e-20) {
        return n; // no UVs to go by here
    }
    vec3 t = (dp1 * duv2.y - dp2 * duv1.y) * sign(det);
    vec3 b = (dp2 * duv1.x - dp1 * duv2.x) * sign(det);
    t = normalize(t - n * dot(n, t));
    b = normalize(b - n * dot(n, b));
    vec3 m = texture(uNormalMap, vUV).xyz * 2.0 - 1.0;
    m.xy *= material.pbr.z;
    return normalize(front * (t * m.x - b * m.y) + n * m.z);
}

// GGX's share of the microfacets facing the half vector, alpha = roughness^2.
float distribution(float n_h, float alpha) {
    const float a2 = alpha * alpha;
    const float d = n_h * n_h * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

// Smith's visibility, height-correlated: the geometry term over 4 n.l n.v.
float visibility(float n_l, float n_v, float alpha) {
    const float a2 = alpha * alpha;
    const float v = n_l * sqrt(n_v * n_v * (1.0 - a2) + a2);
    const float l = n_v * sqrt(n_l * n_l * (1.0 - a2) + a2);
    return 0.5 / max(v + l, 1e-5);
}

vec3 fresnel(vec3 f0, float v_h) {
    const float k = pow(1.0 - v_h, 5.0);
    return f0 + (vec3(1.0) - f0) * k;
}

// The specular albedo of a surface lit from everywhere, as the scale and the
// offset of its F0 (the split-sum integral of the BRDF against a white sky),
// in Karis' fit for mobile.
vec2 environment_brdf(float roughness, float n_v) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    const vec4 r = roughness * c0 + c1;
    const float a004 = min(r.x * r.x, exp2(-9.28 * n_v)) * r.x + r.y;
    return vec2(-1.04, 1.04) * a004 + r.zw;
}

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

// What a surface of reflectance `f0` reflects of a white sky, a rough one's
// second bounce included (Fdez-Aguera's multiple-scattering fit): a white
// metal gives all of it back, as a furnace test wants.
vec3 sky_reflectance(vec3 f0, vec2 ab) {
    const float single = ab.x + ab.y;
    const vec3 one = f0 * ab.x + ab.y;
    const vec3 average = f0 + (vec3(1.0) - f0) / 21.0;
    return one + (1.0 - single) * one * average / (vec3(1.0) - (1.0 - single) * average);
}

// One light from `l` (toward it, length 1) of `radiance` (kin's colour).
struct Surface {
    vec3 n;
    vec3 v;
    float n_v;
    float alpha;
    vec3 diffuse; // the colour it scatters
    vec3 f0;      // what it reflects head on
    vec3 boost;   // the light a rough specular loses to a second bounce, put back
};

vec3 shade(Surface s, vec3 l, vec3 radiance) {
    const float n_l = dot(s.n, l);
    if (n_l <= 0.0) {
        return vec3(0.0);
    }
    const vec3 h = normalize(l + s.v);
    const float n_h = clamp(dot(s.n, h), 0.0, 1.0);
    const float v_h = clamp(dot(s.v, h), 0.0, 1.0);
    const vec3 f = fresnel(s.f0, v_h);
    // What the specular reflects the diffuse does not: kd takes the rest, by
    // a dielectric's Fresnel (the diffuse of a metal is already gone from
    // `diffuse`; between, glTF mixes the two materials).
    const vec3 specular = s.boost * f * (distribution(n_h, s.alpha) * visibility(n_l, max(s.n_v, 1e-4), s.alpha) * PI);
    return (s.diffuse * (1.0 - fresnel(vec3(0.04), v_h)) + specular) * radiance * n_l;
}

// --- What a custom material shader calls ---

// The base colour in linear light and its alpha: the vertex, instance and
// material colours times the base texture.
vec4 kin_base_color() {
    vec4 tex = texture(uBase, vUV);
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
    // A flat-shaded mesh with clusters has no normal in its vertices (they are zero): the face's comes from how the
    // world position changes across the screen, on the side the viewer sees (so no `front`, and no normal map).
    const vec3 dp1 = dFdx(vWorld), dp2 = dFdy(vWorld);
    if (dot(vNormal, vNormal) < 1e-20) {
        return kin_screen_normal(dp1, dp2);
    }
    const float front = gl_FrontFacing ? 1.0 : -1.0;
    const vec3 n = normalize(vNormal) * front;
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
    return vUV;
}

// The material's metallic (x) and roughness (y), the factors times the map.
vec2 kin_metallic_roughness() {
    const vec3 mr = texture(uMetalRough, vUV).rgb;
    return vec2(clamp(material.pbr.x * mr.b, 0.0, 1.0),
                // A floor on the roughness: a perfect mirror would be a single bright pixel.
                clamp(material.pbr.y * mr.g, 0.045, 1.0));
}

// How much ambient light the occlusion map lets through.
float kin_occlusion() {
    return mix(1.0, texture(uOcclusion, vUV).r, material.pbr.w);
}

// The material's own light for a surface of colour `base`: its share of the
// colour, or its emissive map, times the material's emissive.
vec3 kin_emissive(vec3 base) {
    vec3 own = base;
    if (material.maps.y > 0.5) {
        own = texture(uEmissive, vUV).rgb;
        if (frame.flags.y > 0.5) {
            own = decode(own);
        }
    }
    return own * material.params.x;
}

// The light of the scene on a surface (linear, what leaves it toward the
// camera): the sun through its shadow, the fill, the point lights, and the
// ambient light or environment. Not the surface's own light (kin_emissive).
vec3 kin_lit(vec3 base, vec3 n, float metallic, float roughness, float occlusion) {
    const vec3 v = kin_view();
    Surface s;
    s.n = n;
    s.v = v;
    s.n_v = clamp(dot(n, v), 0.0, 1.0);
    s.alpha = roughness * roughness;
    s.diffuse = base * (1.0 - metallic);
    s.f0 = mix(vec3(0.04), base, metallic);
    // A rough microsurface lights itself with a bounce that single
    // scattering leaves out, most of all a metal's (Fdez-Aguera's
    // multiple-scattering fit): put back in the specular, taken from the diffuse.
    const vec2 ab = environment_brdf(roughness, s.n_v);
    const float single = ab.x + ab.y;
    s.boost = vec3(1.0) + s.f0 * (1.0 / max(single, 1e-3) - 1.0);

    vec3 light = vec3(0.0);
    // Compute derivatives before the varying sun-facing branch as well as
    // cascade selection/blending and map-edge exits.
    vec3 world_dx = vec3(0.0), world_dy = vec3(0.0);
    if (frame.shadow.y > 0.5) { world_dx = dFdx(vWorld); world_dy = dFdy(vWorld); }
#ifdef KIN_LIGHTS
    const vec3 offset_normal = shadow_offset_normal(n); // the moon's and the local lights' shadows, as the sun's
#endif
    const float sun = dot(n, frame.sun_direction.xyz);
    if (sun > 0.0) {
        float lit = sunlight(n, world_dx, world_dy);
#ifdef KIN_LIGHTS
        lit = mix(1.0, lit, locals.counts.w);
#endif
        if (frame.occlusion.y > 0.5) {
            lit *= contact_shadow(); // on top of the shadow map's; only the sun's term
        }
        light += shade(s, frame.sun_direction.xyz, frame.sun_color.rgb * lit);
    }
    if (dot(frame.fill_color.rgb, vec3(1.0)) > 0.0) {
#ifdef KIN_LIGHTS
        light += shade(s, frame.fill_direction.xyz, frame.fill_color.rgb * moonlight(offset_normal));
#else
        light += shade(s, frame.fill_direction.xyz, frame.fill_color.rgb);
#endif
    }
    const int count = int(frame.flags.w);
    for (int i = 0; i < count; ++i) {

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
#ifdef KIN_LIGHTS
        const vec3 toward = to * inversesqrt(max(d2, 1e-8));
        float seen = 1.0;
        if (k > 0.0 && material.params.z > 0.5 && dot(n, toward) > 0.0) {
            seen = point_light_shadow(i, offset_normal, toward, sqrt(d2));
        }
        light += shade(s, toward, frame.lights[2 * i + 1].rgb * (k * seen));
#else
        light += shade(s, to * inversesqrt(max(d2, 1e-8)), frame.lights[2 * i + 1].rgb * k);
#endif
    }
#ifdef KIN_LIGHTS
    // Spot lights: a point light's falloff inside a cone (glTF's punctual spot: the square of the linear step of the
    // cosine between the inner and the outer angle), its shadow from its layer of the local array.
    const int spot_count = int(locals.counts.x);
    for (int i = 0; i < spot_count; ++i) {
        const vec4 at = locals.spots[4 * i];
        const vec4 colour = locals.spots[4 * i + 1];
        const vec4 axis = locals.spots[4 * i + 2];
        const vec4 more = locals.spots[4 * i + 3];
        const vec3 to = at.xyz - vWorld;
        const float d2 = dot(to, to);
        const float r2 = d2 / (at.w * at.w);
        const float window = clamp(1.0 - r2 * r2, 0.0, 1.0);
        const vec3 toward = to * inversesqrt(max(d2, 1e-8));
        const float step_in_cone = clamp((dot(-toward, axis.xyz) - axis.w) * more.x, 0.0, 1.0);
        const float k = window * window * step_in_cone * step_in_cone / max(d2, colour.w);
        if (k <= 0.0) {
            continue;
        }
        float seen = 1.0;
        if (more.y >= 0.0 && material.params.z > 0.5 && dot(n, toward) > 0.0) {
            seen = local_shadow(int(more.y), offset_normal, toward, sqrt(d2));
        }
        light += shade(s, toward, colour.rgb * (k * seen));
    }
#endif

    // Ambient, from the sky above to the ground below by which way a
    // direction looks: the diffuse by the normal, the specular by the
    // reflection (turned toward the normal as the surface roughens: a
    // rough one sees a wide cone of the sky); or from the environment: the
    // diffuse from its irradiance, the specular from its level for the roughness.
    vec3 around, above;
    if (frame.environment.w > 0.5) {
        around = textureLod(uEnvIrradiance, environment_uv(n), 0.0).rgb * frame.environment.x;
        above = textureLod(uEnvSpecular, environment_uv(reflect(-v, n)), roughness * frame.environment.y).rgb * frame.environment.x;
    } else {
        const vec3 reflected = normalize(mix(reflect(-v, n), n, s.alpha));
        around = mix(frame.ground_ambient.rgb, frame.ambient.rgb, n.y * 0.5 + 0.5);
        above = mix(frame.ground_ambient.rgb, frame.ambient.rgb, reflected.y * 0.5 + 0.5);
    }
    const vec3 dielectric = sky_reflectance(vec3(0.04), ab);
    const vec3 reflectance = mix(dielectric, sky_reflectance(base, ab), metallic);
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
        return light + (diffuse_light + specular_light) * occlusion;
    }
    return light + (s.diffuse * (vec3(1.0) - dielectric) * around + reflectance * above) * occlusion;
}

// What a shader writes: `colour` (linear) encoded for the target, with its
// alpha, premultiplied as kin's targets are.
#ifdef KIN_FOG
// The sky's light along `dir` for the fog's colour: kin_sky_scatter's, read from the fog table (two rows, linearly) rather
// than integrated at each fragment: the integral's part that depends on the angle to the light is its phase functions,
// worked out here; the rest depends on the ray's elevation alone, and is the table's.
vec3 kin_fog_sky(vec3 dir) {
    const vec3 v = normalize(vec3(dir.x, max(dir.y, 0.0), dir.z) + vec3(1e-6, 0.0, 0.0)); // as kin_sky_scatter takes it
    const float x = kin_fog_row(v.y);
    const int i = min(int(x), KIN_FOG_ROWS - 2);
    const float w = x - float(i);
    const int k = i * 4;
    const float mu = dot(v, fog_params.sky.sun.xyz);
    vec3 light = kin_rayleigh_phase(mu) * mix(fog_params.table[k].rgb, fog_params.table[k + 4].rgb, w) +
                 kin_mie_phase(mu, fog_params.sky.shape.y) * mix(fog_params.table[k + 1].rgb, fog_params.table[k + 5].rgb, w);
    if (fog_params.sky.shape.w > 0.0) {
        const float mu_moon = dot(v, fog_params.sky.moon.xyz);
        light += kin_rayleigh_phase(mu_moon) * mix(fog_params.table[k + 2].rgb, fog_params.table[k + 6].rgb, w) +
                 kin_mie_phase(mu_moon, fog_params.sky.shape.y) * mix(fog_params.table[k + 3].rgb, fog_params.table[k + 7].rgb, w);
    }
    return light + fog_params.sky.night.rgb;
}

// `colour` as seen through the fog from the camera to this fragment: mixed toward the fog's colour by its opacity (a
// blended surface fades the same way, its alpha applied after), or, for an additive one, only dimmed (what it lies over
// has the fog already).
vec3 kin_fog_apply(vec3 colour) {
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
    return mix(colour, kin_fog_sun(fog_params.fog, fog_colour, dir, frame.sun_direction.xyz, frame.sun_color.rgb), opacity);
}
#endif

vec4 kin_output(vec3 colour, float alpha) {
#ifdef KIN_FOG
    colour = kin_fog_apply(colour);
#endif
    if (frame.flags.x > 0.5) {
        colour = encode(colour);
    }
    return vec4(colour * alpha, alpha);
}
