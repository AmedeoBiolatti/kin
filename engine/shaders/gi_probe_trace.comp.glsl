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
layout(set = 0, binding = 7) uniform sampler3D uEmission;
layout(std430, set = 0, binding = 8) readonly buffer List {
    uint list[]; // the batch: a probe's stored index in its level, its level in the top 8
};
layout(std430, set = 0, binding = 9) readonly buffer Emitters {
    float strengths[];
};
layout(std430, set = 0, binding = 10) readonly buffer AreaExplicit {
    uint explicit_emitters[];
};
layout(std430, set = 1, binding = 0) writeonly buffer Rays {
    vec4 rays[]; // rgb: the light found, w: the distance (world; negative: a back; 1e4: nothing hit)
};

#define KIN_GI_SCENE
#include "kin_gi.inc.glsl"

struct Portal {
    mat4 to_entry;
    mat4 through;
    vec4 opening;
};

layout(std140, set = 2, binding = 0) uniform Params {
    KinGiScene scene;
    KinGiProbes probes;
    uvec4 batch; // x: the most steps a ray takes, y: the batch's probes, z: rays a probe
    mat4 rotation;
    vec4 sky;    // x: the environment's strength, y: the level of its chain to read, z: 1 with an environment
    mat4 environment_matrix;
    vec4 ambient; // without an environment: the light from above,
    vec4 ground;  // and from below
    Portal portals[8];
    uvec4 portal_count;
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

bool inside_scene(vec3 at) {
    const int last = int(p.scene.size.w) - 1;
    const vec3 q = at / p.scene.level[last].w - p.scene.level[last].xyz;
    return all(greaterThanEqual(q, vec3(0.0))) && all(lessThan(q, p.scene.size.xyz));
}

// The nearest opening entered from its front. A ray outside the rectangle is an ordinary world ray. Transforming
// the ray, rather than copying a light, preserves occlusion on both sides and carries emission and reflected light.
int next_portal(vec3 at, vec3 way, out float distance) {
    int nearest = -1;
    distance = 1e30;
    for (uint i = 0u; i < p.portal_count.x; ++i) {
        const vec3 o = (p.portals[i].to_entry * vec4(at, 1.0)).xyz;
        const vec3 d = mat3(p.portals[i].to_entry) * way;
        if (o.z <= 1e-4 || d.z >= -1e-6) {
            continue;
        }
        const float t = -o.z / d.z;
        const vec2 q = o.xy + d.xy * t;
        if (t < distance && all(lessThan(abs(q), p.portals[i].opening.xy)) && inside_scene(at + way * t)) {
            nearest = int(i);
            distance = t;
        }
    }
    return nearest;
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
    const ivec3 world = first + kin_gi_wrap(st - first, counts);
    vec3 at = vec3(world) * p.probes.level[k].w + data.xyz;
    vec3 way = normalize(mat3(p.rotation) * fibonacci(r, p.batch.z));
    ivec3 hit;
    int level;
    float t;
    int steps = 0;
    const int most = int(p.batch.x);
    vec4 found;
    bool surface = false;
    bool unresolved = false;
    float travelled = 0.0;
    // Four crossings at most, sharing the ordinary ray's step budget. Cycles and an exit beyond the voxel field
    // contribute no light, rather than turning missing geometry into unoccluded skylight.
    if (p.portal_count.x == 0u) {
        surface = kin_gi_march(p.scene, at, way, 1e30, most, hit, level, t, steps);
    } else {
        for (int crossing = 0; crossing <= 4; ++crossing) {
            float distance;
            const int portal = next_portal(at, way, distance);
            if (kin_gi_march(p.scene, at, way, distance, most, hit, level, t, steps)) {
                surface = true;
                t += travelled;
                break;
            }
            if (steps >= most || portal < 0) {
                break;
            }
            if (crossing == 4) {
                unresolved = true;
                break;
            }
            at = (p.portals[portal].through * vec4(at + way * distance, 1.0)).xyz;
            way = normalize(mat3(p.portals[portal].through) * way);
            at += way * 1e-3;
            travelled += distance;
            if (!inside_scene(at)) {
                unresolved = true;
                break;
            }
        }
    }
    if (surface) {
        const vec4 normal = texelFetch(uNormal, hit, 0);
        const vec3 n = normal.xyz * 2.0 - 1.0;
        const float d = t;
        if (dot(way, n) <= 0.0) {
            found = vec4(texelFetch(uRadiance, hit, 0).rgb, d);
        } else if (normal.a > 0.75) {
            found = vec4(texelFetch(uRadianceBack, hit, 0).rgb, d);
        } else {
            // A remote back face can block a path, but does not mean this local probe is inside a solid.
            found = vec4(0.0, 0.0, 0.0, travelled > 0.0 ? d : -d);
        }
        // Exact area lights remove their own glow from ordinary probe rays (their direct path is lit separately).
        // After a portal this is a different path, not covered by that world-space integral: restore its emission.
        if (travelled > 0.0 && (dot(way, n) <= 0.0 || normal.a > 0.75)) {
            const vec4 glow = texelFetch(uEmission, hit, 0);
            const uint emitter = uint(glow.a + 0.5);
            if (emitter > 0u && emitter <= p.portal_count.y && explicit_emitters[emitter - 1u] != 0u) {
                found.rgb += glow.rgb * strengths[emitter - 1u];
            }
        }
    } else if (unresolved || steps >= most) {
        found = vec4(0.0, 0.0, 0.0, 1e4); // lost in a maze: nothing taken for it
    } else {
        found = vec4(sky(way), 1e4);
    }
    rays[id] = found;
}
