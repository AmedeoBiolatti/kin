#version 450 core
// The depth pyramid's first level (Lod3D::occlusion, docs/3d_plan.md G3d): each texel is the farthest depth of the
// 4 x 4 pixels of the view's single-sample depth it covers. Reverse Z (near is 1, far 0), so the farthest is the smallest;
// what lies outside the depth is 1 (the identity of a minimum), so an odd edge is not taken for far.
layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0) uniform sampler2D depth;
layout(set = 1, binding = 0, r32f) uniform writeonly image2D level0;
layout(std140, set = 2, binding = 0) uniform Params {
    uvec4 sizes; // x, y the depth's size; z, w this level's
} P;

void main() {
    const ivec2 t = ivec2(gl_GlobalInvocationID.xy);
    if (t.x >= int(P.sizes.z) || t.y >= int(P.sizes.w)) {
        return;
    }
    float farthest = 1.0;
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            const ivec2 p = t * 4 + ivec2(x, y);
            if (p.x < int(P.sizes.x) && p.y < int(P.sizes.y)) {
                farthest = min(farthest, texelFetch(depth, p, 0).r);
            }
        }
    }
    imageStore(level0, t, vec4(farthest));
}
