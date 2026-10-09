#version 450 core
// The linear depth chain of the new ambient occlusion (gtao_chain.comp), after XeGTAO: the pre-pass's depth as
// the distance in front of the camera, in R32F, and four coarser levels, each texel a weighted average of the four
// below that leans to the farthest (a thin near thing does not drag a far surface toward the camera, and a
// silhouette does not grow a halo: a texel more than the falloff nearer than the farthest of the four counts for
// less). A workgroup takes a 16 x 16 tile of the depth and writes its 16 x 16, 8 x 8, 4 x 4, 2 x 2 and 1 x 1
// reductions into the five levels. The sky (no depth) is 0 and counts for nothing in an average.
layout(local_size_x = 16, local_size_y = 16) in;

layout(set = 0, binding = 0) uniform sampler2D uDepth; // reverse Z, 0 is nothing
layout(set = 1, binding = 0, r32f) uniform writeonly image2D level0;
layout(set = 1, binding = 1, r32f) uniform writeonly image2D level1;
layout(set = 1, binding = 2, r32f) uniform writeonly image2D level2;
layout(set = 1, binding = 3, r32f) uniform writeonly image2D level3;
layout(set = 1, binding = 4, r32f) uniform writeonly image2D level4;
layout(std140, set = 2, binding = 0) uniform Params {
    vec4 unproject_zw; // e, f, g, h: a view-space z of a depth is (e depth + f) / (g depth + h)
    vec4 weights;      // x, y: a texel's weight is clamp((farthest - its distance) * x + y, 0, 1)
    uvec4 sizes[3];    // the levels' sizes: 0 and 1 in sizes[0], 2 and 3 in sizes[1], 4 in sizes[2].xy
} P;

shared float tile[16][16];

bool inside(uint level, ivec2 t) {
    const uvec4 s = P.sizes[level >> 1];
    const uvec2 size = (level & 1u) == 0u ? s.xy : s.zw;
    return t.x < int(size.x) && t.y < int(size.y);
}

float reduce4(ivec2 at, int step) {
    const float a = tile[at.y][at.x], b = tile[at.y][at.x + step], c = tile[at.y + step][at.x], d = tile[at.y + step][at.x + step];
    const float farthest = max(max(a, b), max(c, d));
    if (farthest <= 0.0) {
        return 0.0;
    }
    const float wa = a > 0.0 ? clamp((farthest - a) * P.weights.x + P.weights.y, 0.0, 1.0) : 0.0;
    const float wb = b > 0.0 ? clamp((farthest - b) * P.weights.x + P.weights.y, 0.0, 1.0) : 0.0;
    const float wc = c > 0.0 ? clamp((farthest - c) * P.weights.x + P.weights.y, 0.0, 1.0) : 0.0;
    const float wd = d > 0.0 ? clamp((farthest - d) * P.weights.x + P.weights.y, 0.0, 1.0) : 0.0;
    return (wa * a + wb * b + wc * c + wd * d) / (wa + wb + wc + wd);
}

void main() {
    const ivec2 lid = ivec2(gl_LocalInvocationID.xy);
    const ivec2 group = ivec2(gl_WorkGroupID.xy);
    const ivec2 p = group * 16 + lid;
    float distance_in_front = 0.0;
    if (inside(0u, p)) {
        const float depth = texelFetch(uDepth, p, 0).r;
        if (depth > 0.0) {
            distance_in_front = -(P.unproject_zw.x * depth + P.unproject_zw.y) / (P.unproject_zw.z * depth + P.unproject_zw.w);
        }
        imageStore(level0, p, vec4(distance_in_front));
    }
    tile[lid.y][lid.x] = distance_in_front;
    barrier();
    if ((lid.x & 1) == 0 && (lid.y & 1) == 0) {
        const float v = reduce4(lid, 1);
        tile[lid.y][lid.x] = v;
        const ivec2 t = group * 8 + (lid >> 1);
        if (inside(1u, t)) {
            imageStore(level1, t, vec4(v));
        }
    }
    barrier();
    if ((lid.x & 3) == 0 && (lid.y & 3) == 0) {
        const float v = reduce4(lid, 2);
        tile[lid.y][lid.x] = v;
        const ivec2 t = group * 4 + (lid >> 2);
        if (inside(2u, t)) {
            imageStore(level2, t, vec4(v));
        }
    }
    barrier();
    if ((lid.x & 7) == 0 && (lid.y & 7) == 0) {
        const float v = reduce4(lid, 4);
        tile[lid.y][lid.x] = v;
        const ivec2 t = group * 2 + (lid >> 3);
        if (inside(3u, t)) {
            imageStore(level3, t, vec4(v));
        }
    }
    barrier();
    if (lid.x == 0 && lid.y == 0) {
        const float v = reduce4(lid, 8);
        if (inside(4u, group)) {
            imageStore(level4, group, vec4(v));
        }
    }
}
