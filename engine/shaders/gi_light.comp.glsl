#version 450 core
// kin's lit scene (docs/3d_plan.md, G5b): the light leaving each voxel a surface is in (diffuse, the same every way):
// its colour times what reaches it, plus its emission. What reaches it: the sun and the fill light, and each point
// and spot light in reach, each through a ray walked toward it through the GI scene (a surface in the way: none); and
// the probes' light, which carries the sky and every bounce so far. Probe rays that hit a surface read the light
// here, and this reads the probes' of the render before: the bounces add up from render to render. A voxel with faces
// both ways (a thin wall, a double-sided material) has each side's own light, so a wall between a lit room and a dark
// one is dark on the dark side.
layout(local_size_x = 64) in;

layout(set = 0, binding = 0) uniform sampler3D uGiDistance;
layout(set = 0, binding = 1) uniform sampler3D uGiAlbedo;
layout(set = 0, binding = 2) uniform sampler3D uNormal;
layout(set = 0, binding = 3) uniform sampler3D uEmission;
layout(set = 0, binding = 4) uniform sampler3D uGiIrradiance;
layout(set = 0, binding = 5) uniform sampler3D uGiMoments;
layout(set = 0, binding = 6) uniform sampler3D uGiProbes;
struct Light {
    vec4 at;     // xyz: the light's place (world); w: its reach
    vec4 colour; // rgb: linear (irradiance over pi a unit away); w: the square of its size
    vec4 axis;   // xyz: a spot's way (unit); w: the cosine of its outer angle (a point light: -2, all round)
    vec4 cone;   // x: 1 / (cos inner - cos outer)
};
layout(std430, set = 0, binding = 7) readonly buffer Lights {
    Light lights[];
};
layout(set = 1, binding = 0, rgba16f) uniform writeonly image3D uRadiance;     // the light leaving its front
layout(set = 1, binding = 1, rgba16f) uniform writeonly image3D uRadianceBack; // and its back, if it has one

#define KIN_GI_SCENE
#define KIN_GI_PROBES
#include "kin_gi.inc.glsl"

layout(std140, set = 2, binding = 0) uniform Params {
    vec4 origin;         // xyz: voxel (0, 0, 0)'s low corner (world); w: voxels a unit
    vec4 size;           // xyz: voxels a side; w: the lights in Lights
    vec4 sun_direction;  // xyz: toward the sun (unit); w: its shadow's strength (0 to 1)
    vec4 sun_colour;     // rgb; w: 1 with a sun
    vec4 fill_direction; // xyz: toward the fill light (unit)
    vec4 fill_colour;    // rgb; w: 1 with a fill light
    KinGiProbes probes;  // counts.w: 1 when the probes have light to give
} p;

// How much of a light along `l` (unit, voxels) reaches the side facing `n` of the voxel at `centre`: the cosine, and
// 0 where a surface is in the way within `far` voxels. Rays start 1.8 voxels out from the surface, past every voxel a
// flat surface touches (those boxes reach 1.74 from it), so a surface never shadows itself.
float reaches(vec3 centre, vec3 n, vec3 l, float far) {
    const float c = dot(n, l);
    if (c <= 0.0) {
        return 0.0;
    }
    ivec3 hit;
    float t;
    int steps = 0;
    return kin_gi_march(centre + n * 1.8, l, far, p.size.xyz, 512, hit, t, steps) ? 0.0 : c;
}

// The same toward a light at `at` (voxels): the ray walks from its start to a voxel and a half short of the light,
// which sits on or in the lamp that holds it.
float reaches_point(vec3 centre, vec3 n, vec3 at) {
    const float c = dot(n, normalize(at - centre));
    if (c <= 0.0) {
        return 0.0;
    }
    const vec3 start = centre + n * 1.8;
    const vec3 to = at - start;
    const float d = length(to);
    if (d <= 1.5) {
        return c;
    }
    ivec3 hit;
    float t;
    int steps = 0;
    return kin_gi_march(start, to / d, d - 1.5, p.size.xyz, 512, hit, t, steps) ? 0.0 : c;
}

// The light reaching the side facing `n` of the voxel at `centre` (`world` in world units), over pi.
vec3 reaching(vec3 centre, vec3 world, vec3 n) {
    vec3 light = vec3(0.0);
    if (p.sun_colour.w > 0.5) {
        const float seen = reaches(centre, n, p.sun_direction.xyz, 1e30);
        light += p.sun_colour.rgb * mix(max(dot(n, p.sun_direction.xyz), 0.0), seen, p.sun_direction.w);
    }
    if (p.fill_colour.w > 0.5) {
        light += p.fill_colour.rgb * reaches(centre, n, p.fill_direction.xyz, 1e30);
    }
    const int count = int(p.size.w);
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
        light += s.colour.rgb * (k * reaches_point(centre, n, (s.at.xyz - p.origin.xyz) * p.origin.w));
    }
    if (p.probes.counts.w > 0.5) {
        light += kin_gi_irradiance(p.probes, world, n, vec3(0.0)) * p.probes.bias.z;
    }
    return light;
}

void main() {
    const uint i = (gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x) * 64u + gl_LocalInvocationIndex;
    const ivec3 size = ivec3(p.size.xyz);
    if (i >= uint(size.x * size.y * size.z)) {
        return;
    }
    const ivec3 voxel = ivec3(int(i) % size.x, (int(i) / size.x) % size.y, int(i) / (size.x * size.y));
    const vec4 albedo = texelFetch(uGiAlbedo, voxel, 0);
    if (albedo.a == 0.0) {
        imageStore(uRadiance, voxel, vec4(0.0));
        imageStore(uRadianceBack, voxel, vec4(0.0));
        return;
    }
    const vec4 normal = texelFetch(uNormal, voxel, 0);
    const vec3 n = normalize(normal.xyz * 2.0 - 1.0);
    const vec3 centre = vec3(voxel) + 0.5;
    const vec3 world = p.origin.xyz + centre / p.origin.w;
    const vec3 emission = texelFetch(uEmission, voxel, 0).rgb;
    imageStore(uRadiance, voxel, vec4(min(albedo.rgb * reaching(centre, world, n) + emission, vec3(65000.0)), 1.0));
    // A voxel with faces both ways (a double-sided material, a wall thinner than a voxel): its back's own light.
    imageStore(uRadianceBack, voxel,
               normal.a > 0.75 ? vec4(min(albedo.rgb * reaching(centre, world, -n) + emission, vec3(65000.0)), 1.0)
                               : vec4(0.0));
}
