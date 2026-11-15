#version 300 es
// Volumetric fog (Fog3D::volumetric) in WebGL2: native fog_volume.comp's light of each cell and
// fog_volume_integrate.comp's sum along each ray, as fragments (no compute here). A draw fills four slices of a 3D
// texture, one an attachment; a fragment is a column of cells (tiles of the view across, rows up as GL's). Each cell is
// lit at its middle (no jitter or history) and every cell is lit (no reach culling); the lights' maps and the sun's
// and the moon's cascades shadow the air with one comparison each, as native's.
precision highp float;
precision highp int;
precision highp sampler3D;
precision highp sampler2DArray;
#include "kin_fog.inc.glsl"
uniform int uMode;   // 0 light the cells, 1 sum them along each ray
uniform int uFirst;  // the first slice this draw fills
uniform vec4 uCells; // xyz: the volume's cells, w: its depth (the view depth of its last slice's far edge)
uniform vec3 uEye;
uniform vec3 uRay00, uRayX, uRayY; // the bottom-left corner's ray at view depth 1, to the bottom-right's, to the top-left's
uniform KinFog uFog;
uniform float uAnisotropy;   // the local lights' lobe (VolumetricFog3D::anisotropy)
uniform float uLeast;        // the least share of light that reaches the camera (1 - the fog's greatest opacity)
uniform vec4 uSun, uSunLight;   // xyz toward the sun, w its shadow's strength; rgb its light (0: not in the volume)
uniform vec4 uFill, uFillLight; // the fill light's (the moon's), unshadowed
uniform vec3 uForward;
uniform highp sampler3D uLit; // mode 1: the lit cells
#include "lights.inc.glsl"
#include "shadow_maps.inc.glsl"
layout(location = 0) out vec4 out0;
layout(location = 1) out vec4 out1;
layout(location = 2) out vec4 out2;
layout(location = 3) out vec4 out3;
const float PI = 3.14159265358979;

vec3 ray_at(vec2 uv) {
    return uRay00 + uv.x * uRayX + uv.y * uRayY;
}
// The fog's extinction at `p`, `dist` from the camera (kin_fog_optical_depth's integrand).
float density_at(vec3 p, float dist) {
    if (dist < uFog.limits.x) return 0.0;
    float d = uFog.density.x;
    if (uFog.density.y > 0.0) d += uFog.density.y * exp(clamp(-uFog.density.z * (p.y - uFog.density.w), -80.0, 80.0));
    return d;
}
float sunlit(vec3 p, float depth) {
    if (uShadowCount < 1 || uSun.w <= 0.0 || depth > uShadowEnds[uShadowCount - 1]) return 1.0;
    int k = 0;
    for (int i = 0; i < 3; ++i) if (i < uShadowCount - 1 && depth > uShadowEnds[i]) k = i + 1;
    vec4 clip = uShadowMatrix[k] * vec4(p, 1.0);
    vec3 ndc = clip.xyz / clip.w;
    vec2 uv = ndc.xy * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))) || ndc.z < 0.0 || ndc.z > 1.0) return 1.0;
    float lit = ndc.z + uShadowBias[k] >= texture(uShadow, vec3(uv, float(k))).r ? 1.0 : 0.0;
    return mix(1.0, lit, uSun.w);
}
// The light the air at `p` scatters toward the camera along `dir` per unit of extinction (fog_volume.comp's).
vec3 in_scattered(vec3 p, vec3 dir, float depth) {
    float g = uAnisotropy, g2 = g * g;
    vec3 sum = vec3(0.0);
    for (int i = 0; i < uPointCount + uSpotCount; ++i) {
        bool spot = i >= uPointCount;
        vec4 at = spot ? spot_light(i - uPointCount, 0) : point_light(i, 0);
        vec4 colour = spot ? spot_light(i - uPointCount, 1) : point_light(i, 1);
        vec3 to = at.xyz - p;
        float d2 = dot(to, to);
        if (d2 >= at.w * at.w) continue;
        float r2 = d2 / (at.w * at.w);
        float window = clamp(1.0 - r2 * r2, 0.0, 1.0);
        float k = window * window / max(d2, colour.w);
        vec3 toward = to * inversesqrt(max(d2, 1e-8));
        if (spot) {
            vec4 axis = spot_light(i - uPointCount, 2), more = spot_light(i - uPointCount, 3);
            float cone = clamp((dot(-toward, axis.xyz) - axis.w) * more.x, 0.0, 1.0);
            k *= cone * cone;
            if (k > 0.0 && more.y >= 0.0) k *= local_shadow_at(int(more.y), p, vec3(0.0), toward, sqrt(d2), 1);
        } else if (k > 0.0) {
            k *= point_shadow_at(point_shadow_layer(i), p, vec3(0.0), toward, sqrt(d2), 1);
        }
        if (k <= 0.0) continue;
        // Henyey-Greenstein over 4 pi.
        float mu = dot(dir, toward);
        float x = max(1.0 + g2 - 2.0 * g * mu, 1e-4);
        sum += colour.rgb * (k * (1.0 - g2) / (4.0 * PI * x * sqrt(x)));
    }
    if (dot(uSunLight.rgb, vec3(1.0)) > 0.0)
        sum += kin_fog_sun_light(uFog, dir, uSun.xyz, uSunLight.rgb) * sunlit(p, depth);
    if (dot(uFillLight.rgb, vec3(1.0)) > 0.0)
        sum += kin_fog_sun_light(uFog, dir, uFill.xyz, uFillLight.rgb) * moonlight_at(p, vec3(0.0), 1);
    return sum;
}
// Cell (this column, slice z): rgb the light it scatters toward the camera per unit of length, a its extinction.
vec4 lit_cell(int z) {
    vec3 at = vec3(floor(gl_FragCoord.xy) + 0.5, float(z) + 0.5) / uCells.xyz;
    float depth = uCells.w * at.z * at.z;
    vec3 ray = ray_at(at.xy);
    vec3 p = uEye + ray * depth;
    float len = length(ray);
    vec3 dir = ray / len;
    float sigma = density_at(p, depth * len);
    return sigma > 0.0 ? vec4(in_scattered(p, dir, depth) * sigma, sigma) : vec4(0.0);
}
void put(int i, vec4 value) {
    if (i == 0) out0 = value;
    else if (i == 1) out1 = value;
    else if (i == 2) out2 = value;
    else out3 = value;
}
void main() {
    out0 = out1 = out2 = out3 = vec4(0.0);
    int slices = int(uCells.z);
    if (uMode == 0) {
        for (int i = 0; i < 4; ++i)
            if (uFirst + i < slices) put(i, lit_cell(uFirst + i));
        return;
    }
    // Front to back to this draw's last slice: each slice's light over its length (Hillaire's step) times the share
    // that reaches the camera; each cell keeps the sum to its far edge (rgb) and the share past it (a).
    ivec2 xy = ivec2(gl_FragCoord.xy);
    float stretch = length(ray_at((vec2(xy) + 0.5) / uCells.xy));
    vec3 sum = vec3(0.0);
    float through = 1.0, before = 0.0;
    int last = min(uFirst + 3, slices - 1);
    for (int z = 0; z <= last; ++z) {
        float w = float(z + 1) / float(slices);
        float after = uCells.w * w * w;
        float len = (after - before) * stretch;
        before = after;
        vec4 lit = texelFetch(uLit, ivec3(xy, z), 0);
        float s = max(lit.a, 0.0);
        float kept = exp(-s * len);
        vec3 slice = s > 1e-7 ? lit.rgb * ((1.0 - kept) / s) : lit.rgb * len;
        sum += max(through, uLeast) * slice;
        through *= kept;
        if (z >= uFirst) put(z - uFirst, vec4(sum, through));
    }
}
