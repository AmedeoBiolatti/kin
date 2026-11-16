#version 450 core
// Depth of field (DepthOfField3D): the widest blur in front of the focus (x) and the widest either side (y), full
// pixels, in each tile of 8 x 8 half-size texels. A work group a tile.
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D uHalfBlur; // dof_prepare's: x the radius
layout(set = 1, binding = 0, rgba16f) uniform writeonly image2D uTiles;

shared vec2 widest[64];

void main() {
    const ivec2 extent = textureSize(uHalfBlur, 0);
    const ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    const float r = all(lessThan(p, extent)) ? texelFetch(uHalfBlur, p, 0).x : 0.0;
    const uint i = gl_LocalInvocationIndex;
    widest[i] = vec2(max(-r, 0.0), abs(r));
    barrier();
    for (uint step = 32u; step > 0u; step >>= 1u) {
        if (i < step) {
            widest[i] = max(widest[i], widest[i + step]);
        }
        barrier();
    }
    if (i == 0u) {
        imageStore(uTiles, ivec2(gl_WorkGroupID.xy), vec4(widest[0], 0.0, 0.0));
    }
}
