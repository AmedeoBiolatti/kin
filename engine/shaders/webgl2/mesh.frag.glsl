#version 300 es
precision highp float;
precision highp int;
precision highp sampler2DArray;
// kin's mesh fragment shader for WebGL2. A surface shader's program (Material3D::surface: a material graph's) is this
// with KIN_SURFACE defined after #version and the surface's source where KIN_SURFACE_SOURCE stands; it then takes
// native's names (frame, material, kin_stock_surface and the other kin_* functions a surface calls: kin_material.inc.glsl's).
uniform sampler2D uTexture, uNormal, uMetalRough, uOcclusion, uEmission;
uniform bool uFlip[5];
uniform vec4 uColor;
uniform vec4 uUvTransform; // Material3D::uv_scale (xy) and uv_offset (zw)
uniform vec3 uSunDirection, uSunColor, uFillDirection, uFillColor, uAmbient, uGround;
uniform bool uUnlit, uHasNormal, uHasEmission;
uniform float uEmissive, uAlphaCutoff, uMetallic, uRoughness, uNormalScale, uOcclusionStrength;
uniform vec3 uEmissionColor;
uniform bool uIndependentEmission;
uniform bool uLinear; // an HDR view's float picture: linear light, not encoded
uniform float uSpecularAa; // Scene3D::specular_aa
uniform float uSingleScatter; // 1: Scene3D::specular_multiscatter off
uniform int uBlend, uPointCount, uSpotCount;
uniform vec4 uPoints[32];
// A spot a 4: position and radius, colour and size squared, axis and cos(outer), 1 / (cos inner - cos outer).
uniform vec4 uSpots[64];
#ifdef KIN_DECAL
// A decal (decal.vert's box) has no mesh: what it shades is the surface the view's depth and normals hold under the
// pixel, put here by main before the lighting reads them.
vec3 vWorld, vNormal, vLocal, vObject;
vec2 vUV;
vec4 vColor;
#else
in vec3 vWorld;
in vec3 vNormal;
in vec2 vUV;
in vec4 vColor;
in vec3 vLocal;       // the point in the copy's own space (posed when skinned)
flat in vec3 vObject; // the copy's origin in the world
#endif
layout(location = 0) out vec4 outColor;
// The surface's normal (the mesh's, before its normal map), for the decals drawn after the opaque meshes: xyz * 0.5 + 0.5.
layout(location = 1) out vec4 outNormal;
const float PI = 3.14159265358979;
#include "kin_brdf.inc.glsl"
#include "environment.inc.glsl"
#include "shadow.inc.glsl"

vec3 decode(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}
vec3 encode(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}
// The maps' UV (native kin_map_uv: the material's scale and offset), and a render target's rows flipped.
vec2 map_uv() { return vUV * uUvTransform.xy + uUvTransform.zw; }
vec2 uv_for(bool flip) { vec2 uv = map_uv(); return vec2(uv.x, flip ? 1.0 - uv.y : uv.y); }
// The face's normal toward the viewer, before any normal map.
vec3 geometric_normal() {
    float front = gl_FrontFacing ? 1.0 : -1.0;
    return dot(vNormal, vNormal) < 1e-20 ? normalize(cross(dFdx(vWorld), dFdy(vWorld))) : normalize(vNormal) * front;
}
vec3 normal_for_surface() {
    vec3 dx = dFdx(vWorld), dy = dFdy(vWorld);
    // GL framebuffer Y increases upward; this cross product faces the viewer.
    float front = gl_FrontFacing ? 1.0 : -1.0;
    vec3 n = dot(vNormal, vNormal) < 1e-20 ? normalize(cross(dx, dy)) : normalize(vNormal) * front;
    if (!uHasNormal) return n;
    vec2 uv = uv_for(uFlip[1]);
    vec2 du = dFdx(uv), dv = dFdy(uv);
    float det = du.x * dv.y - dv.x * du.y;
    if (abs(det) < 1e-20) return n;
    vec3 t = (dx * dv.y - dy * du.y) * sign(det);
    vec3 b = (dy * du.x - dx * dv.x) * sign(det);
    t = normalize(t - n * dot(n, t));
    b = normalize(b - n * dot(n, b));
    vec3 mapped = texture(uNormal, uv).xyz * 2.0 - 1.0;
    // A map of two channels (RG, BC5: blue reads 0) holds x and y: z makes the normal a unit one.
    if (mapped.z < 0.0) mapped.z = sqrt(max(1.0 - dot(mapped.xy, mapped.xy), 0.0));
    mapped.xy *= uNormalScale;
    return normalize(front * (t * mapped.x - b * mapped.y) + n * mapped.z);
}
// A light of size squared `r2` `d2` away is a sphere: its highlight no sharper than the angle it spans (kin_sphere_light).
Surface sphere_light(Surface s, float r2, float d2) {
    float spans = sqrt(clamp(r2 / max(d2, 1e-8), 0.0, 1.0));
    s.alpha = max(s.alpha, spans);
    s.coat_alpha = max(s.coat_alpha, spans);
    return s;
}
// The ambient light off a surface beneath a clear coat (native kin_coated).
vec3 coated(vec3 under, float coat, vec2 ab, float n_v, vec3 coat_above) {
    if (coat <= 0.0) return under;
    float fc = coat * fresnel(vec3(0.04), n_v).x;
    return under * (1.0 - fc) + coat * sky_reflectance(vec3(0.04), ab) * coat_above;
}
// What native kin_lit_surface lights (its hemisphere ambient branch: no environment, probes or GI here).
struct Lit {
    vec3 base, n;
    float metallic, roughness, occlusion;
    vec3 backlight;
    float coat, coat_roughness, specular, wrap;
    vec4 reflection;
};
Lit stock_lit(vec3 base, vec3 n, float metallic, float roughness, float occlusion) {
    return Lit(base, n, metallic, roughness, occlusion, vec3(0.0), 0.0, 0.045, 0.5, 0.0, vec4(0.0));
}
vec3 lighting(Lit L) {
    vec3 base = L.base, n = L.n;
    float metallic = L.metallic, roughness = L.roughness, coat_roughness = L.coat_roughness;
    float f0_dielectric = 0.08 * L.specular;
    // Specular anti-aliasing, as kin_lit's: the roughness raised by how much the normal turns across the pixel.
    if (uSpecularAa > 0.0) {
        vec3 dn_dx = dFdx(n), dn_dy = dFdy(n);
        float widen = min(2.0 * uSpecularAa * (dot(dn_dx, dn_dx) + dot(dn_dy, dn_dy)), 0.18);
        roughness = sqrt(min(1.0, roughness * roughness + widen));
        coat_roughness = sqrt(min(1.0, coat_roughness * coat_roughness + widen));
    }
    vec3 v = normalize(uEye - vWorld);
    Surface s;
    s.n = n;
    s.v = v;
    s.n_v = clamp(dot(n, v), 0.0, 1.0);
    s.alpha = roughness * roughness;
    s.diffuse = base * (1.0 - metallic);
    s.f0 = mix(vec3(f0_dielectric), base, metallic);
    vec2 ab = environment_brdf(roughness, s.n_v);
    bool multiscatter = uSingleScatter < 0.5;
    s.boost = multiscatter ? vec3(1.0) + s.f0 * (1.0 / max(ab.x + ab.y, 1e-3) - 1.0) : vec3(1.0);
    s.backlight = L.backlight;
    s.coat = L.coat;
    s.coat_alpha = coat_roughness * coat_roughness;
    s.dielectric = f0_dielectric;
    s.wrap = L.wrap;
    vec3 light = shade(s, uSunDirection, uSunColor*sunlight()) + shade(s, uFillDirection, uFillColor);
    for (int i=0; i<uPointCount; ++i) {
        vec4 at = uPoints[2*i], color = uPoints[2*i+1];
        vec3 to = at.xyz-vWorld;
        float d2 = dot(to,to), r2 = d2/(at.w*at.w);
        float window = clamp(1.0-r2*r2, 0.0, 1.0);
        float k = window*window / max(d2, color.w);
        if (k > 0.0) light += shade(sphere_light(s, color.w, d2), to*inversesqrt(max(d2,1e-8)), color.rgb*k);
    }
    for (int i=0; i<uSpotCount; ++i) {
        vec4 at = uSpots[4*i], color = uSpots[4*i+1], axis = uSpots[4*i+2];
        vec3 to = at.xyz-vWorld;
        float d2 = dot(to,to), r2 = d2/(at.w*at.w);
        float window = clamp(1.0-r2*r2, 0.0, 1.0);
        vec3 toward = to*inversesqrt(max(d2,1e-8));
        float cone = clamp((dot(-toward, axis.xyz)-axis.w)*uSpots[4*i+3].x, 0.0, 1.0);
        float k = window*window*cone*cone / max(d2, color.w);
        if (k > 0.0) light += shade(sphere_light(s, color.w, d2), toward, color.rgb*k);
    }
    vec3 reflected = normalize(mix(reflect(-v,n),n,s.alpha));
    vec3 coat_reflected = normalize(mix(reflect(-v,n),n,s.coat_alpha));
    vec3 around = mix(uGround,uAmbient,n.y*0.5+0.5);
    vec3 behind = mix(uGround,uAmbient,-n.y*0.5+0.5);
    vec3 above = mix(uGround,uAmbient,reflected.y*0.5+0.5);
    vec3 coat_above = mix(uGround,uAmbient,coat_reflected.y*0.5+0.5);
    if (L.reflection.a > 0.0) above = mix(above, L.reflection.rgb, L.reflection.a);
    if (L.wrap > 0.5) around = mix(around, 0.5 * (around + behind), 2.0 * L.wrap - 1.0);
    vec3 dielectric = multiscatter ? sky_reflectance(vec3(f0_dielectric),ab) : vec3(f0_dielectric * ab.x + ab.y);
    vec2 coat_ab = L.coat > 0.0 ? environment_brdf(coat_roughness, s.n_v) : vec2(0.0);
    vec3 reflectance = mix(dielectric, multiscatter ? sky_reflectance(base,ab) : base * ab.x + ab.y, metallic);
    return light + coated(s.diffuse*(vec3(1.0)-dielectric)*around + reflectance*above, L.coat, coat_ab, s.n_v,
                          coat_above)*L.occlusion + L.backlight*behind*L.occlusion;
}
vec4 base_color() {
    vec4 texel = texture(uTexture, uv_for(uFlip[0]));
    // Offscreen textures contain premultiplied display color, unlike uploaded RGBA.
    if (uFlip[0] && texel.a > 0.0) texel.rgb /= texel.a;
    return vec4(decode(texel.rgb)*vColor.rgb*decode(uColor.rgb), texel.a*vColor.a*uColor.a);
}
vec2 metallic_roughness() {
    vec3 mr = texture(uMetalRough,uv_for(uFlip[2])).rgb;
    return vec2(clamp(uMetallic*mr.b,0.0,1.0), clamp(uRoughness*mr.g,0.045,1.0));
}
float occlusion_of() {
    return mix(1.0,texture(uOcclusion,uv_for(uFlip[3])).r,uOcclusionStrength);
}
vec3 emission_of(vec3 base) {
    vec3 emission = uHasEmission ? decode(texture(uEmission,uv_for(uFlip[4])).rgb)
                                 : uIndependentEmission ? vec3(1.0) : base;
    if (uIndependentEmission) emission *= uEmissionColor;
    return emission * uEmissive;
}

#ifdef KIN_SURFACE
// --- What a surface's source calls (native kin_material.inc.glsl and kin_mesh_surface.inc.glsl's names) ---
#define uBase uTexture
#define uNormalMap uNormal
#define uEmissive uEmission
uniform highp sampler2DArray uGraphMaps; // Material3D::graph_maps
struct KinFrame {
    vec4 time;            // x Scene3D::time, y 1 with specular_multiscatter off
    vec4 eye;             // the camera
    vec4 occlusion;       // zw: 1 / the target's size
    vec4 cascade_forward; // the camera's forward
    vec4 flags;           // y: 1, maps are decoded from sRGB here
    vec4 environment;     // w: 0, no image environment
    vec4 ambient, ground_ambient;
};
uniform KinFrame frame;
struct KinMaterial {
    vec4 pbr;      // metallic, roughness, normal scale, occlusion strength
    vec4 emission; // its colour, w 1 when independent of the base colour
    vec4 params;   // x emissive, y unlit, z receives shadow, w alpha cutoff
    vec4 uv;       // the maps' scale and offset
    vec4 cutoff;
    vec4 maps;     // x has a normal map, y has an emissive map
    vec4 surface[16];
};
uniform KinMaterial material;
vec4 kin_vertex_color() { return vColor * vec4(decode(uColor.rgb), uColor.a); }
#define vColor kin_vertex_color()
vec4 kin_env_specular(vec2 uv, float lod) { return vec4(0.0); }
vec2 environment_uv(vec3 d) { return vec2(0.0); }
vec3 kin_object_position() { return vObject; }
vec3 kin_local_position() { return vLocal; }
vec2 kin_map_uv() { return map_uv(); }
vec2 kin_uv() { return map_uv(); }
vec3 kin_position() { return vWorld; }
vec3 kin_view() { return normalize(frame.eye.xyz - vWorld); }
// The screen position, 0..1 with rows down as native's (GL's run up).
vec2 kin_screen() { vec2 p = gl_FragCoord.xy * frame.occlusion.zw; return vec2(p.x, 1.0 - p.y); }
vec3 kin_screen_normal(vec3 dp1, vec3 dp2) {
    vec3 n = cross(dp1, dp2);
    float l = length(n);
    return l > 0.0 ? n / l : vec3(0.0, 1.0, 0.0);
}
vec4 kin_base_color() { return base_color(); }
vec3 kin_normal() { return normal_for_surface(); }
vec2 kin_metallic_roughness() { return metallic_roughness(); }
float kin_occlusion() { return occlusion_of(); }
vec3 kin_emissive(vec3 base) { return emission_of(base); }
vec4 kin_surface_param(int i) { return material.surface[i]; }
struct KinSurface {
    vec3 base_color;
    float alpha;
    vec3 normal;
    float metallic;
    float roughness;
    float occlusion;
    vec3 emission;
    vec3 backlight;
    float clearcoat;
    float clearcoat_roughness;
    float specular;
    float diffuse_wrap;
    vec4 reflection;
};
KinSurface kin_stock_surface() {
    KinSurface s;
    vec4 base = base_color();
    s.base_color = base.rgb;
    s.alpha = base.a;
    s.normal = normal_for_surface();
    vec2 mr = metallic_roughness();
    s.metallic = mr.x;
    s.roughness = mr.y;
    s.occlusion = occlusion_of();
    s.emission = emission_of(base.rgb);
    s.backlight = vec3(0.0);
    s.clearcoat = 0.0;
    s.clearcoat_roughness = 0.045;
    s.specular = 0.5;
    s.diffuse_wrap = 0.0;
    s.reflection = vec4(0.0);
    return s;
}
// KIN_SURFACE_SOURCE
#endif

#ifdef KIN_DECAL
uniform highp sampler2D uDecalDepth; // the view's depth (reverse Z: 0 is the sky)
uniform sampler2D uDecalNormals;     // and its surfaces' normals
uniform mat4 uToWorld;
// The decal's (decal.vert's instance): the world to its box, its colour, surface and fades, its box's axes.
flat in mat4 dToDecal;
flat in vec4 dColour, dSurface, dFade;
flat in vec3 dAxisX, dAxisY, dAxisZ;
#define uDecalToDecal dToDecal
#define uDecalColour dColour
#define uDecalSurface dSurface
#define uDecalFade dFade
#define uDecalAxisX dAxisX
#define uDecalAxisY dAxisY
#define uDecalAxisZ dAxisZ
uniform vec2 uTargetInv;
// The world position the view's depth holds at pixel `p`, and the depth.
vec3 decal_world(ivec2 p, out float depth) {
    depth = texelFetch(uDecalDepth, p, 0).r;
    vec4 w = uToWorld * vec4((vec2(p) + 0.5) * uTargetInv * 2.0 - 1.0, depth, 1.0);
    return w.xyz / w.w;
}
#endif

void main() {
#ifdef KIN_DECAL
    // Native decal.frag: the picture projected along the box's -Y onto the surface under the pixel, lit as a material.
    ivec2 size = textureSize(uDecalDepth, 0);
    ivec2 p = clamp(ivec2(gl_FragCoord.xy), ivec2(0), size - 1);
    float depth;
    vec3 world = decal_world(p, depth);
    if (depth <= 0.0) discard;
    vec3 local = (uDecalToDecal * vec4(world, 1.0)).xyz;
    if (any(greaterThan(abs(local), vec3(0.5)))) discard;
    // The texture's gradients from the neighbours on the same surface (the nearer in depth of each pair).
    float d0, d1, d2, d3;
    vec3 right = decal_world(min(p + ivec2(1, 0), size - 1), d0), left = decal_world(max(p - ivec2(1, 0), 0), d1);
    vec3 above = decal_world(min(p + ivec2(0, 1), size - 1), d2), below = decal_world(max(p - ivec2(0, 1), 0), d3);
    vec3 dx = abs(d0 - depth) < abs(depth - d1) ? right - world : world - left;
    vec3 dy = abs(d2 - depth) < abs(depth - d3) ? above - world : world - below;
    vec2 uv = local.xz + 0.5;
    vec2 uv_dx = (mat3(uDecalToDecal) * dx).xz, uv_dy = (mat3(uDecalToDecal) * dy).xz;
    vec3 n = normalize(texelFetch(uDecalNormals, p, 0).xyz * 2.0 - 1.0);
    float facing = dot(n, uDecalAxisY);
    float fade = facing > 0.0 ? (uDecalFade.x > 0.0 ? clamp(facing / uDecalFade.x, 0.0, 1.0) : 1.0) : 0.0;
    if (uDecalFade.y > 0.0) fade *= clamp((0.5 - abs(local.y)) / (0.5 * uDecalFade.y), 0.0, 1.0);
    vec4 base = textureGrad(uTexture, uv, uv_dx, uv_dy);
    base.rgb = decode(base.rgb);
    base *= uDecalColour;
    float alpha = base.a * fade;
    if (alpha < 1.0 / 512.0) discard;
    vWorld = world;
    vNormal = n;
    vColor = vec4(1.0);
    vUV = uv;
    if (uDecalSurface.z > 0.0) {
        // The picture's frame on the surface: u along the box's X, the map's green up toward the picture's top (-Z).
        vec3 t = normalize(uDecalAxisX - n * dot(n, uDecalAxisX));
        vec3 b = normalize(-uDecalAxisZ - n * dot(n, -uDecalAxisZ));
        vec3 m = textureGrad(uNormal, uv, uv_dx, uv_dy).xyz * 2.0 - 1.0;
        if (m.z < 0.0) m.z = sqrt(max(1.0 - dot(m.xy, m.xy), 0.0));
        m.xy *= uDecalSurface.z;
        n = normalize(t * m.x + b * m.y + n * m.z);
    }
    vec3 color = lighting(stock_lit(base.rgb, n, uDecalSurface.y, clamp(uDecalSurface.x, 0.045, 1.0), 1.0)) +
                 base.rgb * uDecalSurface.w;
    color = fogged(color, vWorld, uSunDirection, uSunColor, false);
    outColor = uLinear ? vec4(max(color, vec3(0.0)) * alpha, alpha) : vec4(encode(color) * alpha, alpha);
    outNormal = vec4(0.0);
#else
    outNormal = vec4(geometric_normal() * 0.5 + 0.5, 1.0);
#ifdef KIN_SURFACE
    KinSurface s = kin_stock_surface();
    kin_surface(s);
    vec4 base = vec4(s.base_color, s.alpha);
    if (base.a < uAlphaCutoff) discard;
    if (uShadowPass) { outColor=vec4(0.0); return; }
    vec3 color = uUnlit ? s.base_color
                        : lighting(Lit(s.base_color, normalize(s.normal), clamp(s.metallic, 0.0, 1.0),
                                       clamp(s.roughness, 0.045, 1.0), s.occlusion, s.backlight, s.clearcoat,
                                       clamp(s.clearcoat_roughness, 0.045, 1.0), s.specular, s.diffuse_wrap,
                                       s.reflection));
    color += s.emission;
#else
    vec4 base = base_color();
    if (base.a < uAlphaCutoff) discard;
    if (uShadowPass) { outColor=vec4(0.0); return; }
    vec2 mr = metallic_roughness();
    vec3 color = (uUnlit ? base.rgb : lighting(stock_lit(base.rgb,normal_for_surface(),mr.x,mr.y,occlusion_of())))
                 + emission_of(base.rgb);
#endif
    // An SDR view stores display-encoded, premultiplied color for the gamma 2D frame; an HDR one (uLinear) linear
    // light, premultiplied, that its output exposes and tonemaps.
    float alpha = uBlend == 1 || uBlend == 2 ? base.a : 1.0;
    color=fogged(color,vWorld,uSunDirection,uSunColor,uBlend==2);
    outColor = uLinear ? vec4(max(color, vec3(0.0))*alpha,alpha) : vec4(encode(color)*alpha,alpha);
#endif
}
