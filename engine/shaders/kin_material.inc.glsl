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
#ifdef KIN_GI
// Global illumination (G5; docs/3d_plan.md): only the stock light variants (mesh_light*.frag) have it, so that every
// other shader, and custom ones, keep their samplers. The probes' light on what the pre-pass drew, read once a texel at
// half the view's size (gi_resolve.comp).
layout(set = 2, binding = 12) uniform sampler2D uGiLight; // rgb: the light facing the surface; a: its depth along the
                                                          // view (less: the probes have none there; 0: no surface)
layout(set = 2, binding = 13) uniform sampler2D uGiSlope; // the light facing way d is rgb * (w + xyz . d)
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
    // No normal of the map points into the surface: where z would (every texel of a map of two channels, BC5, whose
    // blue reads 0), the map holds x and y only, and z is what makes the normal a unit one.
    if (m.z < 0.0) {
        m.z = sqrt(max(1.0 - dot(m.xy, m.xy), 0.0));
    }
    m.xy *= material.pbr.z;
    return normalize(front * (t * m.x - b * m.y) + n * m.z);
}

#include "kin_brdf.inc.glsl"

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
#ifdef KIN_GI
// The probes' light here, as gi_resolve.comp read it on what the pre-pass drew, from the four texels round this pixel
// (at half its size), filtered: on what the pre-pass drew, those within `near` of this fragment's `depth` along the
// view (all four, most often), or where none is (a thin thing between texels) the one nearest in depth; on what it did
// not draw (glass, in front of what it shows) all of them with light. Weighed, not picked, so that a texel coming or
// going as the view's jitter moves does not make the light jump. For the way `n` (this surface's normal, its normal
// map's) for the diffuse (`probed`) and `r` for the reflections (`traced`). False where the probes have no light for it
// (the sky's stands).
bool kin_gi_resolved(vec3 n, vec3 r, float depth, float near, out vec3 probed, out vec3 traced) {
    const ivec2 size = textureSize(uGiLight, 0);
    const vec2 uv = gl_FragCoord.xy * 0.5 / vec2(size);
    // x, y, z, w: the texels at (0, 1), (1, 1), (1, 0) and (0, 0) from the footprint's first.
    const vec4 depths = textureGather(uGiLight, uv, 3);
    const bool drawn = material.maps.z > 0.5;
    vec4 light, slope;
    if (all(greaterThan(depths, vec4(0.0))) && (!drawn || all(lessThanEqual(abs(depths - depth), vec4(near))))) {
        light = textureLod(uGiLight, uv, 0.0);
        slope = textureLod(uGiSlope, uv, 0.0);
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
                total += weight[i];
            }
        }
        if (total > 0.0) {
            light /= total;
            slope /= total;
        } else if (nearest_one >= 0 && depths[nearest_one] > 0.0) {
            const ivec2 q = clamp(base + offsets[nearest_one], ivec2(0), size - 1);
            light = texelFetch(uGiLight, q, 0);
            slope = texelFetch(uGiSlope, q, 0);
        } else {
            return false;
        }
    }
    probed = light.rgb * clamp(slope.w + dot(slope.xyz, n), 0.0, 4.0);
    traced = light.rgb * clamp(slope.w + dot(slope.xyz, r), 0.0, 4.0);
    return true;
}
#endif

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
            // On top of the shadow map's; only the sun's term, and faded with it by sun_shadow_strength (without the
            // lights variant the strength is 1: a strength of 0 turns the contact pass off before the shader).
#ifdef KIN_LIGHTS
            lit *= contact_shadow() * locals.counts.w + (1.0 - locals.counts.w); // exact at 0 and 1, mix() may not be
#else
            lit *= contact_shadow();
#endif
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
#ifdef KIN_GI
    // With global illumination's probes the diffuse is theirs: the sky's light where they see it, the light of every
    // surface they see, kept out by walls. The reflections too, in so far as a rough surface's are its surroundings'
    // light along the reflection, blurred: what the probes see that way; a smooth surface keeps the sky's sharp
    // reflection, dimmed by how much of the sky's light the probes see that way (none in a closed room).
    if (frame.gi.x > 0.5) {
        const vec3 r = normalize(mix(reflect(-v, n), n, s.alpha));
        const float depth = dot(vWorld - frame.eye.xyz, frame.cascade_forward.xyz);
        const float near = 0.02 * depth + 4.0 * fwidth(depth);
        vec3 probed, traced;
        if (kin_gi_resolved(n, r, depth, near, probed, traced)) {
            around = probed;
            const vec3 open = frame.environment.w > 0.5
                                  ? textureLod(uEnvIrradiance, environment_uv(r), 0.0).rgb * frame.environment.x
                                  : mix(frame.ground_ambient.rgb, frame.ambient.rgb, r.y * 0.5 + 0.5);
            const vec3 luma = vec3(0.2126, 0.7152, 0.0722);
            const float seen = clamp(dot(traced, luma) / max(dot(open, luma), 1e-6), 0.0, 1.0);
            const float smooth_share = (1.0 - roughness) * (1.0 - roughness);
            above = mix(traced, above * seen, smooth_share);
        }
    }
#endif
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
    return mix(colour, kin_fog_sun(fog_params.fog, fog_colour, dir, frame.sun_direction.xyz, frame.sun_color.rgb), opacity);
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
