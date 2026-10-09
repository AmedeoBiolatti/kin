#version 450 core
// Four more levels of the depth pyramid from one (Lod3D::occlusion, docs/3d_plan.md G3d): a workgroup takes a 16 x 16
// tile of the level it reads and writes its 8 x 8, 4 x 4, 2 x 2 and 1 x 1 reductions (each texel the farthest depth of
// the four below, reverse Z: the smallest) into the next four levels, so the pyramid costs a pass for every four levels.
// Texels past an edge count as 1 (nearest), and a level of size 0 is not written (a pass with fewer levels to make).
layout(local_size_x = 16, local_size_y = 16) in;

layout(set = 0, binding = 0) uniform sampler2D source;
layout(set = 1, binding = 0, r32f) uniform writeonly image2D level1;
layout(set = 1, binding = 1, r32f) uniform writeonly image2D level2;
layout(set = 1, binding = 2, r32f) uniform writeonly image2D level3;
layout(set = 1, binding = 3, r32f) uniform writeonly image2D level4;
layout(std140, set = 2, binding = 0) uniform Params {
    uvec4 sizes[5]; // xy: the level read, then the four written
} P;

shared float tile[16][16];

bool inside(uint level, ivec2 t) {
    return t.x < int(P.sizes[level].x) && t.y < int(P.sizes[level].y);
}

float farthest4(ivec2 at, int step) {
    // The four texels at `at` + {0, step}^2 of the tile, step being the tile's stride at that level (all texels
    // of coarser levels are kept at the stride's multiples).
    return min(min(tile[at.y][at.x], tile[at.y][at.x + step]), min(tile[at.y + step][at.x], tile[at.y + step][at.x + step]));
}

void main() {
    const ivec2 lid = ivec2(gl_LocalInvocationID.xy);
    const ivec2 group = ivec2(gl_WorkGroupID.xy);
    const ivec2 p = group * 16 + lid;
    tile[lid.y][lid.x] = inside(0u, p) ? texelFetch(source, p, 0).r : 1.0;
    barrier();
    // Level 1 (8 x 8 of the tile): the thread at (2i, 2j) of the tile, keeping its result at its own place.
    if ((lid.x & 1) == 0 && (lid.y & 1) == 0) {
        const float v = farthest4(lid, 1);
        tile[lid.y][lid.x] = v;
        const ivec2 t = group * 8 + (lid >> 1);
        if (inside(1u, t)) {
            imageStore(level1, t, vec4(v));
        }
    }
    barrier();
    // Level 2 (4 x 4): the thread at (4i, 4j), whose four texels are at a stride of 2.
    if ((lid.x & 3) == 0 && (lid.y & 3) == 0) {
        const float v = farthest4(lid, 2);
        tile[lid.y][lid.x] = v;
        const ivec2 t = group * 4 + (lid >> 2);
        if (inside(2u, t)) {
            imageStore(level2, t, vec4(v));
        }
    }
    barrier();
    if ((lid.x & 7) == 0 && (lid.y & 7) == 0) {
        const float v = farthest4(lid, 4);
        tile[lid.y][lid.x] = v;
        const ivec2 t = group * 2 + (lid >> 3);
        if (inside(3u, t)) {
            imageStore(level3, t, vec4(v));
        }
    }
    barrier();
    if (lid.x == 0 && lid.y == 0) {
        const float v = farthest4(lid, 8);
        if (inside(4u, group)) {
            imageStore(level4, group, vec4(v));
        }
    }
}
