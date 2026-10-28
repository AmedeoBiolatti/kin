#version 300 es
precision highp float;
precision highp int;
uniform sampler2D uTexture, uNormal, uMetalRough, uOcclusion, uEmission;
uniform bool uFlip[5];
uniform vec4 uColor;
uniform vec3 uSunDirection, uSunColor, uFillDirection, uFillColor, uAmbient, uGround;
uniform bool uUnlit, uHasNormal, uHasEmission;
uniform float uEmissive, uAlphaCutoff, uMetallic, uRoughness, uNormalScale, uOcclusionStrength;
uniform vec3 uEmissionColor;
uniform bool uIndependentEmission;
uniform float uSpecularAa; // Scene3D::specular_aa
uniform int uBlend, uPointCount;
uniform vec4 uPoints[32];
in vec3 vWorld;
in vec3 vNormal;
in vec2 vUV;
in vec4 vColor;
out vec4 outColor;
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
vec2 uv_for(bool flip) { return vec2(vUV.x, flip ? 1.0 - vUV.y : vUV.y); }
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
vec3 lighting(vec3 base, vec3 n, float metallic, float roughness, float occlusion) {
    // Specular anti-aliasing, as kin_lit's: the roughness raised by how much the normal turns across the pixel.
    if (uSpecularAa > 0.0) {
        vec3 dn_dx = dFdx(n), dn_dy = dFdy(n);
        float widen = min(2.0 * uSpecularAa * (dot(dn_dx, dn_dx) + dot(dn_dy, dn_dy)), 0.18);
        roughness = sqrt(min(1.0, roughness * roughness + widen));
    }
    vec3 v = normalize(uEye - vWorld);
    Surface s;
    s.n = n;
    s.v = v;
    s.n_v = clamp(dot(n, v), 0.0, 1.0);
    s.alpha = roughness * roughness;
    s.diffuse = base * (1.0 - metallic);
    s.f0 = mix(vec3(0.04), base, metallic);
    vec2 ab = environment_brdf(roughness, s.n_v);
    s.boost = vec3(1.0) + s.f0 * (1.0 / max(ab.x + ab.y, 1e-3) - 1.0);
    vec3 light = shade(s, uSunDirection, uSunColor*sunlight()) + shade(s, uFillDirection, uFillColor);
    for (int i=0; i<uPointCount; ++i) {
        vec4 at = uPoints[2*i], color = uPoints[2*i+1];
        vec3 to = at.xyz-vWorld;
        float d2 = dot(to,to), r2 = d2/(at.w*at.w);
        float window = clamp(1.0-r2*r2, 0.0, 1.0);
        float k = window*window / max(d2, color.w);
        if (k > 0.0) light += shade(s, to*inversesqrt(max(d2,1e-8)), color.rgb*k);
    }
    vec3 reflected = normalize(mix(reflect(-v,n),n,s.alpha));
    vec3 around = mix(uGround,uAmbient,n.y*0.5+0.5);
    vec3 above = mix(uGround,uAmbient,reflected.y*0.5+0.5);
    vec3 dielectric = sky_reflectance(vec3(0.04),ab);
    vec3 reflectance = mix(dielectric,sky_reflectance(base,ab),metallic);
    return light + (s.diffuse*(vec3(1.0)-dielectric)*around + reflectance*above)*occlusion;
}
void main() {
    vec4 texel = texture(uTexture, uv_for(uFlip[0]));
    // Offscreen textures contain premultiplied display color, unlike uploaded RGBA.
    if (uFlip[0] && texel.a > 0.0) texel.rgb /= texel.a;
    vec4 base = vec4(decode(texel.rgb)*vColor.rgb*decode(uColor.rgb), texel.a*vColor.a*uColor.a);
    if (base.a < uAlphaCutoff) discard;
    if (uShadowPass) { outColor=vec4(0.0); return; }
    vec3 mr = texture(uMetalRough,uv_for(uFlip[2])).rgb;
    float metallic = clamp(uMetallic*mr.b,0.0,1.0), roughness = clamp(uRoughness*mr.g,0.045,1.0);
    float occlusion = mix(1.0,texture(uOcclusion,uv_for(uFlip[3])).r,uOcclusionStrength);
    vec3 emission = uHasEmission ? decode(texture(uEmission,uv_for(uFlip[4])).rgb)
                                : uIndependentEmission ? vec3(1.0) : base.rgb;
    if (uIndependentEmission) emission *= uEmissionColor;
    vec3 color = (uUnlit ? base.rgb : lighting(base.rgb,normal_for_surface(),metallic,roughness,occlusion))+emission*uEmissive;
    // The view stores display-encoded, premultiplied color for the gamma 2D frame.
    float alpha = uBlend == 1 || uBlend == 2 ? base.a : 1.0;
    color=fogged(color,vWorld,uSunDirection,uSunColor,uBlend==2);
    outColor = vec4(encode(color)*alpha,alpha);
}
