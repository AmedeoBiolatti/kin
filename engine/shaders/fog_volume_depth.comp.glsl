#version 450 core
// How deep the fog volume (Fog3D::volumetric) has to be lit in each column of its cells: the view depth of the
// farthest surface the pre-pass drew in the column's tile of pixels and in the eight round it (the adding pass reads a
// column's sums bilinearly with its neighbours'), the sky's as far as can be. fog_volume.comp leaves the cells behind it
// unlit: no pixel's ray reaches them. One thread a column; the tile's farthest first (fog_volume_depth's first
// dispatch), then the 3 x 3 neighbourhood's (its second, `params.tile.z` 1).
layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0) uniform sampler2D uDepth; // the pre-pass's, reverse Z (0: nothing) or the first step's
layout(set = 1, binding = 0, r32f) uniform writeonly image2D uOut;

layout(std140, set = 2, binding = 0) uniform Params {
    vec4 unproject_zw; // the inverse projection's z and w rows' last two entries (view depth from reverse Z)
    vec4 sizes;        // xy: the columns, zw: the depth's size
    vec4 tile;         // x: pixels a column across, y: down; z: 0 for the tiles' step, 1 for the neighbourhood's
} params;

void main() {
    const ivec2 column = ivec2(gl_GlobalInvocationID.xy);
    const ivec2 columns = ivec2(params.sizes.xy);
    if (any(greaterThanEqual(column, columns))) {
        return;
    }
    if (params.tile.z > 0.5) {
        // The neighbourhood's: the farthest of the 3 x 3 tiles' (uDepth is the first step's view depths).
        float far = 0.0;
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                far = max(far, texelFetch(uDepth, clamp(column + ivec2(x, y), ivec2(0), columns - 1), 0).r);
            }
        }
        imageStore(uOut, column, vec4(far));
        return;
    }
    // The tile's pixels (a column covers its share of the view, rounded out).
    const ivec2 size = ivec2(params.sizes.zw);
    const ivec2 lo = ivec2(floor(vec2(column) * params.tile.xy));
    const ivec2 hi = min(ivec2(ceil(vec2(column + 1) * params.tile.xy)), size);
    float nearest_z = 1.0; // reverse Z: the farthest surface has the least
    for (int y = lo.y; y < hi.y; ++y) {
        for (int x = lo.x; x < hi.x; ++x) {
            nearest_z = min(nearest_z, texelFetch(uDepth, ivec2(x, y), 0).r);
        }
    }
    // View depth from reverse Z (as kin_depth.inc.glsl's view_position: z / w, negated); the sky's: unbounded.
    const float w = params.unproject_zw.z * nearest_z + params.unproject_zw.w;
    const float depth = nearest_z <= 0.0 ? 1e30 : -(params.unproject_zw.x * nearest_z + params.unproject_zw.y) / w;
    imageStore(uOut, column, vec4(depth));
}
