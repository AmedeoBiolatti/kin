#version 450 core
// Motion blur (MotionBlur3D), second of four: the longest smear in each tile (`tile.x` pixels square, at least the
// longest smear, so the gather's reach stays within a tile and its neighbours). A work group a tile.
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D uPrepared; // motion_blur_prepare's
layout(set = 1, binding = 0, rgba16f) uniform writeonly image2D uTiles; // xy: the longest smear, z: its length
layout(set = 2, binding = 0) uniform Params {
    vec4 tile; // x: tile size in pixels, yz: uPrepared's extent
}
params;

shared vec3 longest[64];

void main() {
    const ivec2 tile = ivec2(gl_WorkGroupID.xy);
    const int side = int(params.tile.x);
    const ivec2 extent = ivec2(params.tile.yz);
    const ivec2 origin = tile * side;
    vec3 best = vec3(0.0);
    for (int y = int(gl_LocalInvocationID.y); y < side; y += 8) {
        for (int x = int(gl_LocalInvocationID.x); x < side; x += 8) {
            const ivec2 p = origin + ivec2(x, y);
            if (p.x < extent.x && p.y < extent.y) {
                const vec4 v = texelFetch(uPrepared, p, 0);
                if (v.w > best.z) {
                    best = vec3(v.xy, v.w);
                }
            }
        }
    }
    const uint i = gl_LocalInvocationIndex;
    longest[i] = best;
    barrier();
    for (uint step = 32u; step > 0u; step >>= 1u) {
        if (i < step && longest[i + step].z > longest[i].z) {
            longest[i] = longest[i + step];
        }
        barrier();
    }
    if (i == 0u) {
        imageStore(uTiles, tile, vec4(longest[0], 0.0));
    }
}
