#version 450 core
// Depth of field (DepthOfField3D): each tile's widest near blur, spread over the tiles it reaches (`reach.x` tiles
// round): what is in focus there may be covered by a blurred thing in front, so the gather looks that far.
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D uTiles; // dof_tiles'
layout(set = 1, binding = 0, rgba16f) uniform writeonly image2D uSpread;
layout(set = 2, binding = 0) uniform Params {
    vec4 reach; // x: tiles round
}
params;

void main() {
    const ivec2 t = ivec2(gl_GlobalInvocationID.xy);
    const ivec2 extent = textureSize(uTiles, 0);
    if (any(greaterThanEqual(t, extent))) {
        return;
    }
    const int reach = int(params.reach.x);
    vec2 widest = texelFetch(uTiles, t, 0).xy;
    for (int y = -reach; y <= reach; ++y) {
        for (int x = -reach; x <= reach; ++x) {
            widest.x = max(widest.x, texelFetch(uTiles, clamp(t + ivec2(x, y), ivec2(0), extent - 1), 0).x);
        }
    }
    imageStore(uSpread, t, vec4(widest, 0.0, 0.0));
}
