#version 450 core
// An exclusive prefix sum over `data` (n inputs, n + 1 results, the last the total), in three dispatches because
// compute passes do not wait for one another inside a dispatch: mode 0 sums each block of 1024 into `blocks`, mode 1
// scans the blocks (one workgroup), mode 2 scans each block and adds its block's start, in place. The sum is of
// integers, so the result does not depend on the order the GPU adds in: the gathered indices are laid out the same
// every run (docs/3d_plan.md, G3c).
layout(local_size_x = 256) in;

layout(std430, set = 1, binding = 0) buffer Data {
    uint data[];
};
layout(std430, set = 1, binding = 1) buffer Blocks {
    uint blocks[];
};
layout(std140, set = 2, binding = 0) uniform Params {
    uvec4 a; // x the mode, y n, z blocks
} P;

shared uint sums[256];

void main() {
    const uint tid = gl_LocalInvocationID.x;
    const uint n = P.a.y;
    if (P.a.x == 1u) {
        const uint count = P.a.z;
        const uint chunk = (count + 255u) / 256u;
        const uint start = tid * chunk;
        uint s = 0u;
        for (uint i = 0u; i < chunk; ++i) {
            if (start + i < count) {
                s += blocks[start + i];
            }
        }
        sums[tid] = s;
        barrier();
        if (tid == 0u) {
            uint run = 0u;
            for (uint t = 0u; t < 256u; ++t) {
                const uint v = sums[t];
                sums[t] = run;
                run += v;
            }
        }
        barrier();
        uint run = sums[tid];
        for (uint i = 0u; i < chunk; ++i) {
            if (start + i < count) {
                const uint v = blocks[start + i];
                blocks[start + i] = run;
                run += v;
            }
        }
        return;
    }
    const uint base = gl_WorkGroupID.x * 1024u + tid * 4u;
    uint v[4];
    uint s = 0u;
    for (uint i = 0u; i < 4u; ++i) {
        v[i] = base + i < n ? data[base + i] : 0u;
        s += v[i];
    }
    if (P.a.x == 0u) {
        sums[tid] = s;
        barrier();
        for (uint stride = 128u; stride > 0u; stride >>= 1u) {
            if (tid < stride) {
                sums[tid] += sums[tid + stride];
            }
            barrier();
        }
        if (tid == 0u) {
            blocks[gl_WorkGroupID.x] = sums[0];
        }
        return;
    }
    sums[tid] = s;
    barrier();
    for (uint offset = 1u; offset < 256u; offset <<= 1u) {
        const uint other = tid >= offset ? sums[tid - offset] : 0u;
        barrier();
        sums[tid] += other;
        barrier();
    }
    uint run = blocks[gl_WorkGroupID.x] + (sums[tid] - s);
    for (uint i = 0u; i < 4u; ++i) {
        if (base + i <= n) {
            data[base + i] = run;
        }
        run += v[i];
    }
}
