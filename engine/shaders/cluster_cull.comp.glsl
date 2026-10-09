#version 450 core
// kin's GPU cluster cut (docs/3d_plan.md, G3c): for every cluster of every copy of one clustered mesh, whether it is
// drawn (its own error is within the allowed pixels, its parent's is not: the test needs no neighbour, so no walk
// down the hierarchy) and in which views (the camera's passes, the shadow map or each cascade) its sphere is in the
// frustum. The answer is the cluster's triangle count in that view's row of `scan`, or 0; the scan that follows turns
// the counts into positions, so the gathered indices come out in a fixed order. Every float expression is written as
// mesh_clusters.cpp's ClusterCutTest and gpu_renderer3d.cpp's ClusterViews write theirs, in the same order and with
// `precise` so that the compiler does not fuse a multiply and an add: the same clusters are chosen, bar the last bit
// of a sqrt or a divide (tests/render3d_tests.cpp compares the two).
layout(local_size_x = 64) in;

struct Cluster {
    vec4 sphere;        // the sphere its error is measured at: xyz centre, w radius
    vec4 parent_sphere;
    vec4 cull_sphere;   // round its own triangles
    vec4 errors;        // x error, y parent error (infinite for a root)
    uvec4 ids;          // x first index (in the mesh's index buffer), y triangles, z depth, w parent depth (~0: a root)
};
layout(std430, set = 0, binding = 0) readonly buffer Clusters {
    Cluster clusters[];
};
struct Job {
    uvec4 ids;   // x the copy's slot in the instance buffer, y scene (0 or 1) | caster << 8, z _visible's bits, w its first cluster among all
    uvec4 more;  // x the copy's scale (as bits), y the clusters of its mesh, z where they start in the chunk's table, w this copy's first thread in its dispatch
};
layout(std430, set = 0, binding = 1) readonly buffer Jobs {
    Job jobs[];
};
struct Instance {
    mat4 model;
    vec4 rest[4];
};
layout(std430, set = 0, binding = 2) readonly buffer Instances {
    Instance instances[];
};
layout(std430, set = 1, binding = 0) buffer Scan {
    uint scan[];
};
// The chosen (view, cluster) elements of this mesh's copies, in the order the threads got to them: a work queue for
// cluster_gather (the places the indices go are the scan's, so this order shows nowhere). counts[8 + d] counts the
// queue of dispatch d; counts[0..7] are cluster_gather's statistics.
layout(std430, set = 1, binding = 1) buffer Chosen {
    uint chosen[];
};
layout(std430, set = 1, binding = 2) buffer Counts {
    uint counts[];
};

layout(std140, set = 2, binding = 0) uniform Params {
    vec4 eye[2];     // xyz, w the near plane
    vec4 forward[2]; // xyz, w 1 for an orthographic camera
    vec4 cut[2];     // x pixels a world unit spans, y the error allowed in pixels, z the forced depth (-1: by error), w 1 when the camera's frustum culls
    // The frustums, six planes each: 0 and 1 the cameras', 2 to 5 the cascades', 6 to 9 their slices', 10 the single map's.
    vec4 planes[66];
    uvec4 counts[3]; // each frustum's plane count (index f: counts[f >> 2][f & 3])
    vec4 down;       // the way the sun's light goes
    vec4 area_lo;
    vec4 area_hi;
    uvec4 shadow;    // x the cascades (more than 1: cascades; else the single map), y 1 when the sun casts, z where this dispatch's queue starts in `chosen`
    uvec4 range;     // x the first job, y unused, z jobs, w threads (the clusters of all the jobs)
    uvec4 sizes;     // x clusters in all jobs, y views, z groups a row of the dispatch, w this dispatch's number
} P;

const uint no_cluster = 0xFFFFFFFFu;

uint plane_count(uint f) {
    return P.counts[f >> 2][f & 3u];
}

bool sphere_in(uint f, vec3 c, float r) {
    const uint n = plane_count(f);
    for (uint k = 0u; k < n; ++k) {
        const vec4 p = P.planes[f * 6u + k];
        precise float d = p.x * c.x;
        d = d + p.y * c.y;
        d = d + p.z * c.z;
        d = d + p.w;
        if (d < -r) {
            return false;
        }
    }
    return true;
}

bool shadow_in(uint f, vec3 c, float r, vec3 down, float reach) {
    precise vec3 e = vec3(c.x + down.x * reach, c.y + down.y * reach, c.z + down.z * reach);
    const uint n = plane_count(f);
    for (uint k = 0u; k < n; ++k) {
        const vec4 p = P.planes[f * 6u + k];
        precise float a = p.x * c.x;
        a = a + p.y * c.y;
        a = a + p.z * c.z;
        a = a + p.w;
        precise float b = p.x * e.x;
        b = b + p.y * e.y;
        b = b + p.z * e.z;
        b = b + p.w;
        if (a < -r && b < -r) {
            return false;
        }
    }
    return true;
}

float to_leave(vec3 c, vec3 down) {
    float t = 1e30;
    const float from[3] = float[3](c.x, c.y, c.z);
    const float d[3] = float[3](down.x, down.y, down.z);
    const float lo[3] = float[3](P.area_lo.x, P.area_lo.y, P.area_lo.z);
    const float hi[3] = float[3](P.area_hi.x, P.area_hi.y, P.area_hi.z);
    for (int k = 0; k < 3; ++k) {
        if (d[k] > 1e-6) {
            t = min(t, (hi[k] - from[k]) / d[k]);
        } else if (d[k] < -1e-6) {
            t = min(t, (lo[k] - from[k]) / d[k]);
        }
    }
    return max(t, 0.0);
}

// The copy's matrix applied to a point, as Mat4f::apply (m[0] * x + m[4] * y + m[8] * z + m[12] * w, in that order).
vec3 place(mat4 m, vec3 p) {
    precise float x = m[0][0] * p.x;
    x = x + m[1][0] * p.y;
    x = x + m[2][0] * p.z;
    x = x + m[3][0] * 1.0;
    precise float y = m[0][1] * p.x;
    y = y + m[1][1] * p.y;
    y = y + m[2][1] * p.z;
    y = y + m[3][1] * 1.0;
    precise float z = m[0][2] * p.x;
    z = z + m[1][2] * p.y;
    z = z + m[2][2] * p.z;
    z = z + m[3][2] * 1.0;
    return vec3(x, y, z);
}

float dot3(vec3 a, vec3 b) {
    precise float d = a.x * b.x;
    d = d + a.y * b.y;
    d = d + a.z * b.z;
    return d;
}

// ClusterCutTest::distance
float cut_distance(uint scene, vec3 c, float r) {
    if (P.forward[scene].w > 0.5) {
        return 1.0;
    }
    const vec3 to = c - P.eye[scene].xyz;
    const float depth = dot3(to, P.forward[scene].xyz);
    float d = 0.0;
    if (depth > r) {
        d = depth - r;
    } else if (depth < -r) {
        precise float len = sqrt(dot3(to, to));
        d = len - r;
    }
    return max(d, P.eye[scene].w);
}

// ClusterCutTest::fine
bool fine(uint scene, mat4 model, float scale, float error, vec4 sphere) {
    if (error <= 0.0) {
        return true;
    }
    if (isinf(error) || isnan(error)) {
        return false;
    }
    precise float lhs = error * scale;
    lhs = lhs * P.cut[scene].x;
    precise float rhs = P.cut[scene].y * cut_distance(scene, place(model, sphere.xyz), sphere.w * scale);
    return lhs <= rhs;
}

// ClusterViews::views
uint views_of(uint scene, uint visible, bool caster, vec3 c, float r) {
    uint mask = 0u;
    const bool cull_on = P.cut[scene].w > 0.5;
    if ((visible & 1u) != 0u && (!cull_on || sphere_in(scene, c, r))) {
        mask |= 1u;
    }
    if (caster) {
        const vec3 down = P.down.xyz;
        if (P.shadow.x > 1u) {
            const float leave = to_leave(c, down) + r;
            for (uint k = 0u; k < P.shadow.x; ++k) {
                if ((visible & (4u << k)) != 0u && sphere_in(2u + k, c, r) && shadow_in(6u + k, c, r, down, leave)) {
                    mask |= 2u << k;
                }
            }
        } else if (sphere_in(10u, c, r) && (!cull_on || shadow_in(scene, c, r, down, to_leave(c, down) + r))) {
            mask |= 2u;
        }
    }
    return mask;
}

void main() {
    const uint t = (gl_WorkGroupID.y * P.sizes.z + gl_WorkGroupID.x) * 64u + gl_LocalInvocationID.x;
    if (t >= P.range.w) {
        return;
    }
    // The copy this thread's cluster belongs to: the last job of the dispatch that starts at or before it.
    uint lo = P.range.x, hi = P.range.x + P.range.z - 1u;
    while (lo < hi) {
        const uint mid = (lo + hi + 1u) >> 1;
        if (jobs[mid].more.w <= t) {
            lo = mid;
        } else {
            hi = mid - 1u;
        }
    }
    const Job job = jobs[lo];
    const uint index = t - job.more.w;
    const uint scene = job.ids.y & 0xFFu;
    const bool caster = (job.ids.y >> 8) != 0u;
    const mat4 model = instances[job.ids.x].model;
    const float scale = uintBitsToFloat(job.more.x);
    const Cluster c = clusters[job.more.z + index];
    const int force = int(P.cut[scene].z);
    bool selected;
    if (force >= 0) {
        const uint depth = uint(force);
        selected = c.ids.z <= depth && !(c.ids.w != no_cluster && c.ids.w <= depth);
    } else {
        selected = fine(scene, model, scale, c.errors.x, c.sphere) && !fine(scene, model, scale, c.errors.y, c.parent_sphere);
    }
    uint mask = 0u;
    if (selected) {
        mask = views_of(scene, job.ids.z, caster, place(model, c.cull_sphere.xyz), c.cull_sphere.w * scale);
    }
    const uint g = job.ids.w + index;
    for (uint v = 0u; v < P.sizes.y; ++v) {
        const bool drawn = ((mask >> v) & 1u) != 0u;
        scan[v * P.sizes.x + g] = drawn ? c.ids.y : 0u;
        if (drawn) {
            chosen[P.shadow.z + atomicAdd(counts[8u + P.sizes.w], 1u)] = v * P.sizes.x + g;
        }
    }
}
