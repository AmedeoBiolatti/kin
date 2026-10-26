#version 450 core
// kin's GI probes (docs/3d_plan.md, G5b): this render's batch of probes casts its rays through the GI scene. A ray's
// answer: the light of the side of the surface it hits first (the lit scene's) and how far it went; nothing and minus
// the distance where it meets the back of a one-sided surface (the probe may be inside something); the sky's light
// where it leaves the scene. The rays of a probe are a spherical Fibonacci set, turned each render (`rotation`), so that the updates
// cover every direction.
layout(local_size_x = 64) in;

layout(set = 0, binding = 0) uniform sampler3D uGiDistance;
layout(set = 0, binding = 1) uniform sampler3D uGiAlbedo;
layout(set = 0, binding = 2) uniform sampler3D uNormal;
layout(set = 0, binding = 3) uniform sampler3D uRadiance;
layout(set = 0, binding = 4) uniform sampler3D uGiProbes;
layout(set = 0, binding = 5) uniform sampler2D uSky; // the environment's specular chain (any texture without)
layout(set = 0, binding = 6) uniform sampler3D uRadianceBack;
layout(std430, set = 0, binding = 7) readonly buffer List {
    uint list[]; // the batch: a probe's stored index in its level, its level in the top 8
};
layout(std430, set = 1, binding = 0) writeonly buffer Rays {
    vec4 rays[]; // rgb: the light found, w: the distance (world; negative: a back; 1e4: nothing hit)
};

#define KIN_GI_SCENE
#include "kin_gi.inc.glsl"

layout(std140, set = 2, binding = 0) uniform Params {
    KinGiScene scene;
    KinGiProbes probes;
    uvec4 batch; // x: the most steps a ray takes, y: the batch's probes, z: rays a probe
    mat4 rotation;
    vec4 sky;    // x: the environment's strength, y: the level of its chain to read, z: 1 with an environment
    mat4 environment_matrix;
    vec4 ambient; // without an environment: the light from above,
    vec4 ground;  // and from below
} p;

const float PI = 3.14159265358979;

vec3 fibonacci(uint i, uint n) {
    const float phi = 2.399963229728653 * float(i);
    const float z = 1.0 - (2.0 * float(i) + 1.0) / float(n);
    const float r = sqrt(max(0.0, 1.0 - z * z));
    return vec3(cos(phi) * r, sin(phi) * r, z);
}

vec3 sky(vec3 d) {
    if (p.sky.z > 0.5) {
        const vec3 e = mat3(p.environment_matrix) * d;
        const vec2 st = vec2(0.5 + atan(e.z, e.x) / (2.0 * PI), 0.5 - asin(clamp(e.y, -1.0, 1.0)) / PI);
        return textureLod(uSky, st, p.sky.y).rgb * p.sky.x;
    }
    return mix(p.ground.rgb, p.ambient.rgb, d.y * 0.5 + 0.5);
}

void main() {
    const uint id = (gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x) * 64u + gl_LocalInvocationIndex;
    const uint b = id / p.batch.z, r = id - b * p.batch.z;
    if (b >= p.batch.y) {
        return;
    }
    const uint entry = list[b];
    const int k = int(entry >> 24u), index = int(entry & 0x7fffffu);
    const ivec3 counts = ivec3(p.probes.counts.xyz);
    const ivec3 st = ivec3(index % counts.x, (index / counts.x) % counts.y, index / (counts.x * counts.y));
    const vec4 data = texelFetch(uGiProbes, ivec3(st.xy, st.z + k * counts.z), 0);
    if (data.w == 0.0) {
        rays[id] = vec4(0.0); // asleep
        return;
    }
    const ivec3 first = ivec3(p.probes.level[k].xyz);
    const ivec3 world = first + (st - first + counts * 4096) % counts;
    const vec3 at = vec3(world) * p.probes.level[k].w + data.xyz;
    const vec3 way = normalize(mat3(p.rotation) * fibonacci(r, p.batch.z));
    ivec3 hit;
    int level;
    float t;
    int steps = 0;
    const int most = int(p.batch.x);
    vec4 found;
    if (kin_gi_march(p.scene, at, way, 1e30, most, hit, level, t, steps)) {
        const vec4 normal = texelFetch(uNormal, hit, 0);
        const vec3 n = normal.xyz * 2.0 - 1.0;
        const float d = t;
        if (dot(way, n) <= 0.0) {
            found = vec4(texelFetch(uRadiance, hit, 0).rgb, d);
        } else if (normal.a > 0.75) {
            found = vec4(texelFetch(uRadianceBack, hit, 0).rgb, d);
        } else {
            found = vec4(0.0, 0.0, 0.0, -d); // a back
        }
    } else if (steps >= most) {
        found = vec4(0.0, 0.0, 0.0, 1e4); // lost in a maze: nothing taken for it
    } else {
        found = vec4(sky(way), 1e4);
    }
    rays[id] = found;
}
