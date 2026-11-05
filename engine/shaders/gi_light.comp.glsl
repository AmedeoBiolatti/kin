#version 450 core
// kin's lit scene (docs/3d_plan.md, G5b): the light leaving each voxel a surface is in (diffuse, the same every way):
// its colour times what reaches it, plus its emission. What reaches it: the sun and the fill light, and each point
// and spot light in reach, each through a ray walked toward it through the GI scene (a surface in the way: none); and
// the probes' light, which carries the sky and every bounce so far. Probe rays that hit a surface read the light
// here, and this reads the probes' of the render before: the bounces add up from render to render. A voxel with faces
// both ways (a thin wall, a double-sided material) has each side's own light, so a wall between a lit room and a dark
// one is dark on the dark side. Over a list of blocks of 8 x 8 x 8 (those made again this render, and a share of the
// rest in turn, G5c), a thread a voxel.
layout(local_size_x = 64) in;

layout(set = 0, binding = 0) uniform sampler3D uGiDistance;
layout(set = 0, binding = 1) uniform sampler3D uGiAlbedo;
layout(set = 0, binding = 2) uniform sampler3D uNormal;
layout(set = 0, binding = 3) uniform sampler3D uEmission;
layout(set = 0, binding = 4) uniform sampler3D uGiIrradiance;
layout(set = 0, binding = 5) uniform sampler3D uGiMoments;
layout(set = 0, binding = 6) uniform sampler3D uGiProbes;
layout(set = 0, binding = 7) uniform sampler2DArrayShadow uSunMap; // the GI's sun map (layer 0), reverse Z
struct Light {
    vec4 at;     // xyz: the light's place (world); w: its reach
    vec4 colour; // rgb: linear (irradiance over pi a unit away); w: the square of its size
    vec4 axis;   // xyz: a spot's way (unit); w: the cosine of its outer angle (a point light: -2, all round)
    vec4 cone;   // x: 1 / (cos inner - cos outer)
};
layout(std430, set = 0, binding = 8) readonly buffer Lights {
    Light lights[];
};
layout(std430, set = 0, binding = 9) readonly buffer Blocks {
    uint blocks[]; // a stored block: x, y and z (8 bits each), and its level (the top 8)
};
layout(std430, set = 0, binding = 10) readonly buffer Emitters {
    float strengths[]; // each emitter's emissive strength this render (an emissive voxel's alpha: its number, from 1)
};
// Area lights (GlobalIllumination3D::area_lights): the emitters lit exactly (kin_area_light.inc.glsl), and for each
// GI emitter whether it is one of them (its voxels' own glow then stays out: the light it gives is counted here).
layout(std430, set = 0, binding = 11) readonly buffer AreaTriangles {
    vec4 kin_area_triangles[];
};
layout(std430, set = 0, binding = 12) readonly buffer AreaNodes {
    vec4 kin_area_nodes[];
};
layout(std430, set = 0, binding = 13) readonly buffer AreaEmitters {
    vec4 kin_area_emitters[];
};
layout(std430, set = 0, binding = 14) readonly buffer AreaExplicit {
    uint explicit_emitters[]; // a GI emitter's (from its number - 1): 1 when lit as an area light
};
layout(std430, set = 0, binding = 15) readonly buffer AreaScene {
    vec4 kin_area_scene[]; // the copies' hierarchy, then the copies
};
layout(set = 1, binding = 0, rgba16f) uniform writeonly image3D uRadiance;     // the light leaving its front
layout(set = 1, binding = 1, rgba16f) uniform writeonly image3D uRadianceBack; // and its back, if it has one
// Area lights: each voxel's front's light from them, at the probes' strength, and in the alpha the epoch it is for
// (Params.area.y); a 1 x 1 x 1 stand-in (never touched) without them. The probes' strength is applied after.
layout(set = 1, binding = 2, rgba16f) uniform image3D uAreaCache;

#define KIN_GI_SCENE
#define KIN_GI_PROBES
#include "kin_gi.inc.glsl"

layout(std140, set = 2, binding = 0) uniform Params {
    KinGiScene scene;
    uvec4 count;         // x: the lights in Lights, y: the blocks in Blocks, z: how many of them come first, made this
                         // render (their area light is found afresh)
    vec4 sun_direction;  // xyz: toward the sun (unit); w: its shadow's strength (0 to 1)
    vec4 sun_colour;     // rgb; w: 1 with a sun
    vec4 fill_direction; // xyz: toward the fill light (unit)
    vec4 fill_colour;    // rgb; w: 1 with a fill light
    KinGiProbes probes;  // counts.w: 1 when the probes have light to give
    mat4 sun_matrix;     // the GI's sun map's view and projection
    vec4 sun_map;        // x: 1 with the map, y: its depth bias, z: a texel (uv), w: a texel (world)
    vec4 area;           // x: area-light emitters, y: with area lights, the cache's epoch (1 to 1024; 0 without),
                         // z: GI emitters in AreaExplicit, w: where
                         // the copies start in AreaScene
    vec4 area_scene;     // the same, then the settled hierarchy's first node and triangle, 1 when there is one
} p;

float kin_area_strength(uint gi_emitter) {
    return strengths[gi_emitter - 1u];
}

#define KIN_AREA_LINES 2
#include "kin_area_light.inc.glsl"

// How much of a light along `l` (unit) reaches the side facing `n` of the voxel at `at` (world) of a level whose voxels
// are `cell` wide: the cosine, and 0 where a surface is in the way. Rays start 1.8 voxels out from the surface, past
// every voxel a flat surface touches (those boxes reach 1.74 from it), so a surface never shadows itself.
float reaches(vec3 at, float cell, vec3 n, vec3 l) {
    const float c = dot(n, l);
    if (c <= 0.0) {
        return 0.0;
    }
    ivec3 hit;
    int level;
    float t;
    int steps = 0;
    return kin_gi_march(p.scene, at + n * (1.8 * cell), l, 1e30, 512, hit, level, t, steps) ? 0.0 : c;
}

// The same toward a light at `light` (world): the ray walks from its start to a voxel and a half short of the light,
// which sits on or in the lamp that holds it.
float reaches_point(vec3 at, float cell, vec3 n, vec3 light) {
    const float c = dot(n, normalize(light - at));
    if (c <= 0.0) {
        return 0.0;
    }
    const vec3 start = at + n * (1.8 * cell);
    const vec3 to = light - start;
    const float d = length(to);
    if (d <= 1.5 * cell) {
        return c;
    }
    ivec3 hit;
    int level;
    float t;
    int steps = 0;
    return kin_gi_march(p.scene, start, to / d, d - 1.5 * cell, 512, hit, level, t, steps) ? 0.0 : c;
}

// How much of the sun's light reaches the side facing `n` of the voxel at `world` (its cosine times the share of it
// the GI's sun map lets through), from the map: the voxel's face sampled at four points a third of a voxel from its
// middle, each a voxel out along the normal (a surface in the voxel lies within its half diagonal of the middle), the
// depth compared with a bias of two texels. The map is the sun's own view of the meshes, as the main pass's
// shadows, where the voxels' march (conservative: every voxel a surface touches is full, and the coarser windows'
// more so) closes the narrow gaps the sun comes through. -1 where the voxel is outside the map.
float sun_seen(vec3 world, float cell, vec3 n, vec3 l) {
    const float c = dot(n, l);
    if (c <= 0.0) {
        return 0.0;
    }
    const vec3 a = normalize(abs(n.y) < 0.9 ? cross(n, vec3(0.0, 1.0, 0.0)) : cross(n, vec3(1.0, 0.0, 0.0)));
    const vec3 b = cross(n, a);
    float lit = 0.0;
    for (int i = 0; i < 4; ++i) {
        const vec2 o = vec2(i & 1, i >> 1) * 2.0 - 1.0;
        const vec3 q = world + n * cell + (a * o.x + b * o.y) * (cell / 3.0);
        const vec4 clip = p.sun_matrix * vec4(q, 1.0);
        const vec3 ndc = clip.xyz / clip.w;
        const vec2 uv = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
        if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0 || ndc.z < 0.0 || ndc.z > 1.0) {
            return -1.0;
        }
        lit += texture(uSunMap, vec4(uv, 0.0, ndc.z + p.sun_map.y));
    }
    return c * lit * 0.25;
}

// The light reaching the side facing `n` of the voxel at `world`, over pi.
vec3 reaching(vec3 world, float cell, vec3 n) {
    vec3 light = vec3(0.0);
    if (p.sun_colour.w > 0.5) {
        float seen = p.sun_map.x > 0.5 ? sun_seen(world, cell, n, p.sun_direction.xyz) : -1.0;
        if (seen < 0.0) {
            seen = reaches(world, cell, n, p.sun_direction.xyz);
        }
        light += p.sun_colour.rgb * mix(max(dot(n, p.sun_direction.xyz), 0.0), seen, p.sun_direction.w);
    }
    if (p.fill_colour.w > 0.5) {
        light += p.fill_colour.rgb * reaches(world, cell, n, p.fill_direction.xyz);
    }
    const int count = int(p.count.x);
    for (int j = 0; j < count; ++j) {
        const Light s = lights[j];
        const vec3 to = s.at.xyz - world;
        const float d2 = dot(to, to);
        // kin_lit's fall-off: the inverse square, windowed to nothing at the reach, no brighter than at the light's size.
        const float r2 = d2 / (s.at.w * s.at.w);
        const float window = clamp(1.0 - r2 * r2, 0.0, 1.0);
        float k = window * window / max(d2, s.colour.w);
        if (k <= 0.0 || dot(to, n) <= 0.0) {
            continue;
        }
        if (s.axis.w > -1.5) {
            const float step_in_cone =
                clamp((dot(-to * inversesqrt(max(d2, 1e-12)), s.axis.xyz) - s.axis.w) * s.cone.x, 0.0, 1.0);
            k *= step_in_cone * step_in_cone;
            if (k <= 0.0) {
                continue;
            }
        }
        light += s.colour.rgb * (k * reaches_point(world, cell, n, s.at.xyz));
    }
    if (p.probes.counts.w > 0.5) {
        vec3 around;
        if (kin_gi_irradiance(p.probes, world, n, vec3(0.0), around)) {
            light += around * p.probes.bias.z;
        }
    }
    return light;
}

// The area lights' light on the side facing `n` (over pi): shadow rays from a voxel out, past the surface it holds.
vec3 area_reaching(vec3 world, float cell, vec3 n) {
    return kin_area_light(uint(p.area.x), uvec4(p.area_scene), world, n, world + n * cell);
}

void main() {
    const uint i = (gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x) * 64u + gl_LocalInvocationIndex;
    const uint entry = i >> 9u;
    if (entry >= p.count.y) {
        return;
    }
    const uint block = blocks[entry], v = i & 511u;
    const int k = int(block >> 24u);
    const ivec3 size = ivec3(p.scene.size.xyz);
    const ivec3 st = ivec3(block & 255u, (block >> 8u) & 255u, (block >> 16u) & 255u) * 8 +
                     ivec3(v & 7u, (v >> 3u) & 7u, v >> 6u);
    const ivec3 voxel = ivec3(st.xy, st.z + k * size.z);
    const vec4 albedo = texelFetch(uGiAlbedo, voxel, 0);
    if (albedo.a == 0.0) {
        imageStore(uRadiance, voxel, vec4(0.0));
        imageStore(uRadianceBack, voxel, vec4(0.0));
        return;
    }
    // The voxel's place in the world: the window voxel stored here.
    const ivec3 first = ivec3(p.scene.level[k].xyz);
    const ivec3 local = kin_gi_wrap(st - first, size);
    const float cell = p.scene.level[k].w;
    const vec3 world = (vec3(first + local) + 0.5) * cell;
    const vec4 normal = texelFetch(uNormal, voxel, 0);
    const vec3 n = normalize(normal.xyz * 2.0 - 1.0);
    const vec4 glow = texelFetch(uEmission, voxel, 0);
    const uint emitter = uint(glow.a + 0.5);
    const bool exact = emitter > 0u && p.area.y > 0.5 && emitter <= uint(p.area.z) &&
                       explicit_emitters[emitter - 1u] != 0u; // lit as an area light: its glow is not the probes'
    const vec3 emission = exact ? vec3(0.0) : (glow.a > 0.5 ? glow.rgb * strengths[emitter - 1u] : glow.rgb);
    // The area lights' (the front's kept from render to render while their scene and strengths stay, but for the
    // blocks made this render).
    vec3 area = vec3(0.0), area_back = vec3(0.0);
    if (p.area.y > 0.5) {
        const vec4 kept = imageLoad(uAreaCache, voxel);
        if (entry >= p.count.z && abs(kept.a - p.area.y) < 0.5) {
            area = kept.rgb;
        } else {
            area = area_reaching(world, cell, n);
            imageStore(uAreaCache, voxel, vec4(min(area, vec3(65000.0)), p.area.y));
        }
        if (normal.a > 0.75) {
            area_back = area_reaching(world, cell, -n);
        }
        area *= p.probes.bias.z;
        area_back *= p.probes.bias.z;
    }
    imageStore(uRadiance, voxel,
               vec4(min(albedo.rgb * (reaching(world, cell, n) + area) + emission, vec3(65000.0)), 1.0));
    // A voxel with faces both ways (a double-sided material, a wall thinner than a voxel): its back's own light.
    imageStore(uRadianceBack, voxel,
               normal.a > 0.75
                   ? vec4(min(albedo.rgb * (reaching(world, cell, -n) + area_back) + emission, vec3(65000.0)), 1.0)
                   : vec4(0.0));
}
