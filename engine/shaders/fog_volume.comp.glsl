#version 450 core
// The fog's light from the point and spot lights (Fog3D::volumetric): for each cell of a volume over the view (tiles of
// the view across, slices of its depth along: the slice's far edge at distance * (z / slices)^2, closer together near
// the camera), the light the fog's air scatters toward the camera there, and how thick the air is. A point in the cell,
// moved within it per cell and per frame, is lit by every light the frame chose whose reach holds it (the inverse
// square windowed at the radius and no brighter than at the light's size, a spot's cone, as kin_point_light and
// kin_spot_light have them; through its shadow map where it has one: beams), each weighed by a Henyey-Greenstein lobe
// of the fog's anisotropy between the light's way and the camera's ray. The fog's own density (kin_fog.inc.glsl's: the
// distance fog from its start, the height fog) makes the air; its scattering is the light times it (all of the light
// it takes out goes into scattering: no absorption), times the setting's intensity and each light's own `volumetric`.
// The result is blended with the frames before (the history volume, read where this cell was in the last frame's
// volume), so that the moving points average to the cell's light. fog_volume_integrate.comp then sums it along each
// ray, and fog_volume_apply.frag adds the sum to the picture.
layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

layout(set = 0, binding = 0) uniform sampler2DArrayShadow uLocalShadow; // the local lights' maps, compared
layout(set = 0, binding = 1) uniform sampler3D uHistory;                // the last frame's result
layout(set = 0, binding = 2) uniform sampler2DArrayShadow uSunShadow;   // the sun's cascades, compared
layout(set = 0, binding = 3) uniform sampler2DArrayShadow uMoonShadow;  // the fill light's (the moon's)
layout(set = 0, binding = 4) uniform sampler2D uReach; // per column: the view depth its cells are lit to
                                                       // (fog_volume_depth.comp)

// The lights, as the light grid's (gpu_renderer3d.cpp's GridLight), their colour times the fog's intensity and the
// light's own `volumetric`.
struct FogLight {
    vec4 at;     // position, radius
    vec4 colour; // linear rgb, the square of the light's size
    vec4 axis;   // a spot's way and the cosine of its outer angle (a point light: w is -2)
    vec4 cone;   // x: 1 / (cos inner - cos outer), y: its shadow's layer (a point light's cube's first; -1: none)
};
layout(std430, set = 0, binding = 5) readonly buffer FogLights { FogLight lights[]; };

layout(set = 1, binding = 0, rgba16f) uniform writeonly image3D uOut; // rgb: the light scattered toward the camera
                                                                      // per unit of length, a: the air's extinction

#include "kin_fog.inc.glsl"

layout(std140, set = 2, binding = 0) uniform Params {
    vec4 eye;       // xyz: the camera; w: the volume's depth (view depth of its last slice's far edge)
    vec4 forward;   // xyz: the way it looks (view depth is measured along it); w: lights
    vec4 ray00;     // xyz: the ray of the view's top-left corner, at view depth 1
    vec4 ray_x;     // xyz: from it to the top-right's
    vec4 ray_y;     // xyz: from it to the bottom-left's
    vec4 size;      // xyz: the volume's cells; w: the share of the history kept (0: none)
    vec4 jitter;    // xyz: this frame's offset of the points within their cells (0 to 1); w: the lobe's anisotropy
    mat4 previous;  // the last frame's world to clip (unjittered)
    vec4 previous_eye;     // xyz; w: 1 to light every cell (RenderDebugOptions3D::fog_volume_cull off)
    vec4 previous_forward; // xyz; w: 1 if the history is the last frame's volume (else it is not read)
    KinFog fog;
    // The sun and the fill light (the moon), scattered as the main pass's fog scatters the sun (fog.limits.z, w), each
    // through its shadow: toward each, its light (0: not in the volume), the sun's cascades.
    vec4 sun;        // xyz: toward the sun; w: its shadow's strength (0: unshadowed)
    vec4 sun_light;  // rgb
    vec4 fill;       // xyz: toward the fill light
    vec4 fill_light; // rgb
    mat4 sun_matrix[4];
    vec4 sun_end;    // the view depth where each cascade ends
    vec4 sun_bias;   // each cascade's depth bias
    vec4 sun_shadow; // x: cascades (0: no map), y: the view depth they reach
} params;

#define KIN_LIGHTS_SET 2
#define KIN_LIGHTS_BINDING 1
const int MAX_LIGHTS = 16;
#include "kin_local_lights.inc.glsl"

const float PI = 3.14159265358979;

// Interleaved gradient noise (Jimenez).
float ign(vec2 p) {
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

// The fog's extinction at `p`, `dist` from the camera (kin_fog_optical_depth's integrand).
float density_at(vec3 p, float dist) {
    if (dist < params.fog.limits.x) {
        return 0.0;
    }
    float d = params.fog.density.x;
    if (params.fog.density.y > 0.0) {
        d += params.fog.density.y * exp(clamp(-params.fog.density.z * (p.y - params.fog.density.w), -80.0, 80.0));
    }
    return d;
}

// The share of a local light's shadow map's layer `layer` that reaches `p`, `distance` from the light: one filtered
// comparison (the frames' moving points average the rest), the reference moved toward the light by the layer's bias
// (kin_shadows.inc.glsl's local_light, without a surface to push off). 1 outside the map's view.
float fog_local_shadow(int layer, vec3 p, float distance) {
    const vec4 a = locals.local_a[kin_local_light(layer)];
    const vec4 b = locals.local_b[kin_local_light(layer)];
    const vec4 clip = kin_local_clip(layer, p);
    if (clip.w <= a.z || clip.w > a.w) {
        return 1.0;
    }
    const vec2 ndc = clip.xy / clip.w;
    const vec2 uv = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) {
        return 1.0;
    }
    const float texel_world = clip.w * a.y;
    const float d = max(clip.w - texel_world * b.x, a.z);
    const float ref = a.z * (a.w / d - 1.0) / (a.w - a.z);
    const float texel = locals.local.x;
    const vec4 c = locals.local_c[layer];
    const vec2 q = clamp(c.xy + uv * a.x, c.xy + vec2(0.5 * texel), c.xy + vec2(a.x - 0.5 * texel));
    const float lit = texture(uLocalShadow, vec4(q, c.z, ref));
    return mix(lit, 1.0, smoothstep(0.8 * b.w, b.w, distance));
}

// Whether a directional light reaches `p` (view depth `depth`): one comparison in the cascade whose slice holds it (1
// outside them). `count` cascades, `ends` where each stops.
float cascade_lit(sampler2DArrayShadow maps, mat4 m, int k, vec3 p, float bias) {
    const vec4 clip = m * vec4(p, 1.0);
    const vec3 ndc = clip.xyz / clip.w;
    const vec2 uv = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0 || ndc.z < 0.0 || ndc.z > 1.0) {
        return 1.0;
    }
    return texture(maps, vec4(uv, float(k), ndc.z + bias));
}

float sunlit(vec3 p, float depth) {
    const int count = int(params.sun_shadow.x);
    if (count < 1 || params.sun.w <= 0.0 || depth > params.sun_shadow.y) {
        return 1.0;
    }
    int k = 0;
    for (int i = 0; i < count - 1; ++i) {
        k += depth > params.sun_end[i] ? 1 : 0;
    }
    return mix(1.0, cascade_lit(uSunShadow, params.sun_matrix[k], k, p, params.sun_bias[k]), params.sun.w);
}

float moonlit(vec3 p, float depth) {
    const int count = int(locals.counts.y);
    if (count < 1) {
        return 1.0;
    }
    int k = 0;
    for (int i = 0; i < count - 1; ++i) {
        k += depth > locals.moon_end[i] ? 1 : 0;
    }
    if (count > 1 && depth > locals.moon_end[count - 1]) {
        return 1.0; // (past the last cascade; one map covers the whole area and has no end)
    }
    return mix(1.0, cascade_lit(uMoonShadow, locals.moon_matrix[k], k, p, locals.moon_bias[k].x), locals.counts.z);
}

// The sun's and the fill light's light that the air scatters toward the camera along `dir`, per unit of extinction:
// the main pass's fog's in-scattering (kin_fog_sun_light: the lobe scaled so that an even one is 1) through each
// one's shadow.
vec3 directional_scattered(vec3 p, vec3 dir, float depth) {
    vec3 sum = vec3(0.0);
    if (dot(params.sun_light.rgb, vec3(1.0)) > 0.0) {
        sum += kin_fog_sun_light(params.fog, dir, params.sun.xyz, params.sun_light.rgb) * sunlit(p, depth);
    }
    if (dot(params.fill_light.rgb, vec3(1.0)) > 0.0) {
        sum += kin_fog_sun_light(params.fog, dir, params.fill.xyz, params.fill_light.rgb) * moonlit(p, depth);
    }
    return sum;
}

// A point light's cube face for the way from the light to the point (as point_light_shadow_from).
int cube_face(vec3 from_light) {
    const vec3 m = abs(from_light);
    if (m.x >= m.y && m.x >= m.z) {
        return from_light.x < 0.0 ? 1 : 0;
    }
    if (m.y >= m.z) {
        return from_light.y < 0.0 ? 3 : 2;
    }
    return from_light.z < 0.0 ? 5 : 4;
}

// The lights whose reach meets this group's block of cells (a sphere round its corners), gathered by the group's
// threads together; past MAX_GROUP_LIGHTS every thread goes through all of them.
const uint MAX_GROUP_LIGHTS = 128u;
shared uint group_count;
shared uint group_needed; // a cell of the group is in front of its column's farthest surface
shared uint group_lights[MAX_GROUP_LIGHTS];

// The world position of volume coordinates `at` (u, v across and down the view, w along its depth).
vec3 volume_point(vec3 at) {
    return params.eye.xyz + (params.ray00.xyz + at.x * params.ray_x.xyz + at.y * params.ray_y.xyz) *
                                (params.eye.w * at.z * at.z);
}

void gather_lights() {
    const vec3 cells = params.size.xyz;
    const vec3 lo = vec3(gl_WorkGroupID * gl_WorkGroupSize) / cells;
    const vec3 hi = min(vec3((gl_WorkGroupID + 1u) * gl_WorkGroupSize), cells) / cells;
    vec3 corners[8];
    vec3 centre = vec3(0.0);
    for (int i = 0; i < 8; ++i) {
        corners[i] = volume_point(vec3((i & 1) != 0 ? hi.x : lo.x, (i & 2) != 0 ? hi.y : lo.y, (i & 4) != 0 ? hi.z : lo.z));
        centre += corners[i] * 0.125;
    }
    float radius = 0.0;
    for (int i = 0; i < 8; ++i) {
        radius = max(radius, distance(centre, corners[i]));
    }
    const uint count = uint(params.forward.w);
    const uint threads = gl_WorkGroupSize.x * gl_WorkGroupSize.y * gl_WorkGroupSize.z;
    for (uint i = gl_LocalInvocationIndex; i < count; i += threads) {
        const vec4 at = lights[i].at;
        if (distance(at.xyz, centre) < at.w + radius) {
            const uint k = atomicAdd(group_count, 1u);
            if (k < MAX_GROUP_LIGHTS) {
                group_lights[k] = i;
            }
        }
    }
}

// The light the lights shine on `p` that the air scatters toward the camera along `dir` (the view ray), per unit of
// extinction.
vec3 in_scattered(vec3 p, vec3 dir) {
    const float g = params.jitter.w;
    const float g2 = g * g;
    const bool all_lights = group_count > MAX_GROUP_LIGHTS;
    const int count = all_lights ? int(params.forward.w) : int(group_count);
    vec3 sum = vec3(0.0);
    for (int j = 0; j < count; ++j) {
        const FogLight l = lights[all_lights ? uint(j) : group_lights[j]];
        const vec3 to = l.at.xyz - p;
        const float d2 = dot(to, to);
        if (d2 >= l.at.w * l.at.w) {
            continue;
        }
        const float r2 = d2 / (l.at.w * l.at.w);
        const float window = clamp(1.0 - r2 * r2, 0.0, 1.0);
        float k = window * window / max(d2, l.colour.w);
        const vec3 toward = to * inversesqrt(max(d2, 1e-8));
        const bool spot = l.axis.w > -1.5;
        if (spot) {
            const float step_in_cone = clamp((dot(-toward, l.axis.xyz) - l.axis.w) * l.cone.x, 0.0, 1.0);
            k *= step_in_cone * step_in_cone;
        }
        if (k <= 0.0) {
            continue;
        }
        if (l.cone.y >= 0.0) {
            const int layer = spot ? int(l.cone.y) : int(l.cone.y) + cube_face(-toward);
            k *= fog_local_shadow(layer, p, sqrt(d2));
        }
        // Henyey-Greenstein over 4 pi: what a unit of air sends toward the camera of the light it takes in.
        const float mu = dot(dir, toward);
        const float x = max(1.0 + g2 - 2.0 * g * mu, 1e-4);
        const float phase = (1.0 - g2) / (4.0 * PI * x * sqrt(x));
        sum += l.colour.rgb * (k * phase);
    }
    return sum;
}

// A cell no pixel's ray reaches (behind its column's farthest surface and its neighbours'): left unlit, its extinction
// a mark that keeps the next frame from reading it as history (a filtered read near it comes out negative).
const vec4 UNSEEN = vec4(0.0, 0.0, 0.0, -65504.0);

void main() {
    const ivec3 cell = ivec3(gl_GlobalInvocationID);
    const bool in_volume = all(lessThan(cell, ivec3(params.size.xyz)));
    // Whether the cell's near edge is in front of the farthest surface its column's pixels (and their neighbours')
    // show: else nothing sees it.
    bool needed = false;
    if (in_volume) {
        const float w0 = float(cell.z) / params.size.z;
        // (previous_eye.w: RenderDebugOptions3D::fog_volume_cull off, every cell lit.)
        needed = params.previous_eye.w > 0.5 || params.eye.w * w0 * w0 <= texelFetch(uReach, cell.xy, 0).r;
    }
    if (gl_LocalInvocationIndex == 0u) {
        group_count = 0u;
        group_needed = 0u;
    }
    barrier();
    if (needed) {
        group_needed = 1u;
    }
    barrier();
    if (group_needed == 0u) {
        // (The whole group unseen: no lights gathered.)
        if (in_volume) {
            imageStore(uOut, cell, UNSEEN);
        }
        return;
    }
    gather_lights();
    barrier();
    if (!in_volume) {
        return;
    }
    if (!needed) {
        imageStore(uOut, cell, UNSEEN);
        return;
    }
    // A point in the cell: moved within it by this frame's offset and the cell's noise when there is a history to
    // average them, else its middle.
    const vec3 cells = params.size.xyz;
    const float n = ign(vec2(cell.xy) + vec2(cell.z * 7.0, cell.z * 3.0));
    const vec3 inside = params.size.w > 0.0 ? fract(params.jitter.xyz + vec3(n, fract(n * 1.618034), fract(n * 2.618034)))
                                            : vec3(0.5);
    const vec3 at = (vec3(cell) + inside) / cells; // u, v, w in the volume
    const float depth = params.eye.w * at.z * at.z;
    const vec3 ray = params.ray00.xyz + at.x * params.ray_x.xyz + at.y * params.ray_y.xyz;
    const vec3 p = volume_point(at);
    const float ray_length = length(ray);
    const vec3 dir = ray / ray_length;
    const float sigma = density_at(p, depth * ray_length);
    vec3 light = sigma > 0.0 ? (in_scattered(p, dir) + directional_scattered(p, dir, depth)) * sigma : vec3(0.0);
    vec4 result = vec4(light, sigma);

    // The history: where the cell's middle was in the last frame's volume.
    if (params.previous_forward.w > 0.5 && params.size.w > 0.0) {
        const vec3 q = volume_point((vec3(cell) + 0.5) / cells);
        const vec4 clip = params.previous * vec4(q, 1.0);
        const float was_depth = dot(q - params.previous_eye.xyz, params.previous_forward.xyz);
        if (clip.w > 1e-5 && was_depth > 0.0) {
            const vec2 ndc = clip.xy / clip.w;
            const vec3 was = vec3(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5, sqrt(was_depth / params.eye.w));
            if (all(greaterThanEqual(was, vec3(0.0))) && all(lessThanEqual(was, vec3(1.0)))) {
                const vec4 history = textureLod(uHistory, was, 0.0);
                if (history.a >= 0.0) { // (not near a cell left unlit last frame)
                    result = mix(result, history, params.size.w);
                }
            }
        }
    }
    imageStore(uOut, cell, result);
}
