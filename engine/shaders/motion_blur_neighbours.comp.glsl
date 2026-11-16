#version 450 core
// Motion blur (MotionBlur3D), third of four: the longest smear among each tile and its eight neighbours, the
// direction and reach of what can smear over the tile's pixels (a smear is never longer than a tile).
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D uTiles; // motion_blur_tiles'
layout(set = 1, binding = 0, rgba16f) uniform writeonly image2D uNeighbours;
layout(set = 2, binding = 0) uniform Params {
    vec4 extent; // xy: tiles across and down
}
params;

void main() {
    const ivec2 t = ivec2(gl_GlobalInvocationID.xy);
    const ivec2 extent = ivec2(params.extent.xy);
    if (any(greaterThanEqual(t, extent))) {
        return;
    }
    vec4 best = vec4(0.0);
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            const vec4 v = texelFetch(uTiles, clamp(t + ivec2(x, y), ivec2(0), extent - 1), 0);
            if (v.z > best.z) {
                best = v;
            }
        }
    }
    imageStore(uNeighbours, t, best);
}
