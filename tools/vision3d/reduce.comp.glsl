#version 450 core
// One workgroup per observer. Only the count leaves the mask; no image readback.
layout(local_size_x = 64) in;
layout(set = 0, binding = 0) uniform sampler2D uMask;
layout(std430, set = 1, binding = 0) buffer Counts { uint count[]; } result;
layout(std140, set = 2, binding = 0) uniform Params { vec4 info; } params;
shared uint totals[64];
void main() {
    const uint lane = gl_LocalInvocationIndex;
    const ivec2 size = textureSize(uMask, 0);
    uint sum = 0u;
    for (uint p = lane; p < uint(size.x * size.y); p += 64u)
        sum += texelFetch(uMask, ivec2(int(p) % size.x, int(p) / size.x), 0).r > 0.5 ? 1u : 0u;
    totals[lane] = sum;
    barrier();
    for (uint step = 32u; step > 0u; step >>= 1u) {
        if (lane < step) totals[lane] += totals[lane + step];
        barrier();
    }
    if (lane == 0u) result.count[uint(params.info.x)] = totals[0];
}
