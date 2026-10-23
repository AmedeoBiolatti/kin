#version 450 core
// Gathers the chosen clusters' indices into the frame's one index buffer, each at its scanned position (so the order
// is the clusters' own, every run, whatever order cluster_cull queued them in). A fixed number of workgroups walk the
// queue of one mesh's copies, a workgroup a cluster at a time, its threads copying the cluster's indices. What would
// pass the room is not written (the run cluster_args made ends there): the clusters cut short or dropped are counted.
layout(local_size_x = 64) in;

struct Cluster {
    vec4 sphere;
    vec4 parent_sphere;
    vec4 cull_sphere;
    vec4 errors;
    uvec4 ids; // x first index in the pool
};
layout(std430, set = 0, binding = 0) readonly buffer Clusters {
    Cluster clusters[];
};
layout(std430, set = 0, binding = 1) readonly buffer Pool {
    uint pool[];
};
layout(std430, set = 0, binding = 2) readonly buffer Scan {
    uint scan[];
};
layout(std430, set = 0, binding = 3) readonly buffer Chosen {
    uint chosen[];
};
struct Job {
    uvec4 ids;   // w its first cluster among all
    uvec4 more;  // z where its mesh's clusters start in the chunk's table
};
layout(std430, set = 0, binding = 4) readonly buffer Jobs {
    Job jobs[];
};
layout(std430, set = 1, binding = 0) buffer Out {
    uint gathered[];
};
layout(std430, set = 1, binding = 1) buffer Counts {
    uint counts[]; // 0 camera clusters, 1 camera triangles, 2 clusters dropped or cut short, 3 their triangles lost, 4 clusters, 5 triangles (all views)
};
layout(std140, set = 2, binding = 0) uniform Params {
    uvec4 a; // x this dispatch's number, y where its queue starts, z copies in all, w room (triangles)
    uvec4 b; // x clusters in all jobs, y unused, z workgroups
} P;

void main() {
    const uint count = counts[8u + P.a.x];
    const uint tid = gl_LocalInvocationID.x;
    for (uint i = gl_WorkGroupID.x; i < count; i += P.b.z) {
        const uint e = chosen[P.a.y + i];
        const uint v = e / P.b.x;
        const uint g = e - v * P.b.x;
        // Its copy: the last job that starts at or before the cluster.
        uint lo = 0u, hi = P.a.z - 1u;
        while (lo < hi) {
            const uint mid = (lo + hi + 1u) >> 1;
            if (jobs[mid].ids.w <= g) {
                lo = mid;
            } else {
                hi = mid - 1u;
            }
        }
        const uint index = jobs[lo].more.z + (g - jobs[lo].ids.w);
        const uint start = scan[e];
        const uint n = scan[e + 1u] - start;
        const uint take = start >= P.a.w ? 0u : min(n, P.a.w - start);
        const uint from = clusters[index].ids.x;
        for (uint k = tid; k < take * 3u; k += 64u) {
            gathered[start * 3u + k] = pool[from + k];
        }
        if (tid == 0u) {
            if (take < n) {
                atomicAdd(counts[2], 1u);
                atomicAdd(counts[3], n - take);
            }
            if (take > 0u) {
                atomicAdd(counts[4], 1u);
                atomicAdd(counts[5], take);
                if (v == 0u) {
                    atomicAdd(counts[0], 1u);
                    atomicAdd(counts[1], take);
                }
            }
        }
    }
}
