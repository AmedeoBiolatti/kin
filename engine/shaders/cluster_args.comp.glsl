#version 450 core
// The indirect draw of each (view, copy): no vertex offset, one instance (the copy's slot in the instance buffer, as
// the first instance), and its run of the gathered indices: from where the scan puts its first cluster to where it puts
// the next copy's (the scan is one array over every view's row of every copy's clusters, so the next element is the
// next run's start), both held to the buffer's room. A copy that nothing draws in a view gets a run of nothing.
layout(local_size_x = 64) in;

struct Job {
    uvec4 ids;   // x slot, w its first cluster among all
    uvec4 more;  // y the clusters of its mesh
};
layout(std430, set = 0, binding = 0) readonly buffer Jobs {
    Job jobs[];
};
layout(std430, set = 0, binding = 1) readonly buffer Scan {
    uint scan[];
};
layout(std430, set = 1, binding = 0) buffer Args {
    uint args[];
};
layout(std140, set = 2, binding = 0) uniform Params {
    uvec4 a; // x jobs, y views, z clusters in all, w room (triangles)
    uvec4 b; // x groups a row
} P;

void main() {
    const uint t = (gl_WorkGroupID.y * P.b.x + gl_WorkGroupID.x) * 64u + gl_LocalInvocationID.x;
    if (t >= P.a.x * P.a.y) {
        return;
    }
    const uint v = t / P.a.x;
    const uint j = t - v * P.a.x;
    const Job job = jobs[j];
    const uint start = v * P.a.z + job.ids.w;
    const uint first = min(scan[start], P.a.w);
    const uint end = min(scan[start + job.more.y], P.a.w);
    args[t * 5u + 0u] = (end - first) * 3u;
    args[t * 5u + 1u] = 1u;
    args[t * 5u + 2u] = first * 3u;
    args[t * 5u + 3u] = 0u;
    args[t * 5u + 4u] = job.ids.x;
}
