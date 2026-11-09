#version 450 core
// The fog's light from the point and spot lights (Fog3D::volumetric), summed along each ray of the volume
// fog_volume.comp lit: for each column of cells, front to back, the light each slice scatters toward the camera times
// the share of it that reaches the camera through the air before it (the slice's own air taken in closed form: a
// uniform slice of extinction s and length L sends S (1 - e^(-s L)) / s, Hillaire's energy-conserving step), and that
// share. Each cell keeps the sum to its slice's far edge (rgb) and the share past it (a); fog_volume_apply.frag reads
// the sum at a pixel's depth.
layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0) uniform sampler3D uLit; // fog_volume.comp's: rgb the light per unit of length, a the air
layout(set = 0, binding = 1) uniform sampler2D uReach; // per column: the view depth its cells were lit to

layout(set = 1, binding = 0, rgba16f) uniform writeonly image3D uOut;

layout(std140, set = 2, binding = 0) uniform Params {
    vec4 eye;     // w: the volume's depth (the view depth of its last slice's far edge)
    vec4 ray00;   // xyz: the ray of the view's top-left corner, at view depth 1
    vec4 ray_x;   // xyz: from it to the top-right's
    vec4 ray_y;   // xyz: from it to the bottom-left's
    vec4 size;    // xyz: the volume's cells; w: the least share of the light that reaches the camera (1 - the fog's
                  // greatest opacity)
} params;

void main() {
    const ivec2 xy = ivec2(gl_GlobalInvocationID.xy);
    const ivec3 cells = ivec3(params.size.xyz);
    if (any(greaterThanEqual(xy, cells.xy))) {
        return;
    }
    // The ray's length per unit of view depth through this column's middle.
    const vec2 uv = (vec2(xy) + 0.5) / vec2(cells.xy);
    const float stretch = length(params.ray00.xyz + uv.x * params.ray_x.xyz + uv.y * params.ray_y.xyz);
    vec3 sum = vec3(0.0);
    float through = 1.0;
    float before = 0.0;
    // Up to the slice that holds the column's reach (no pixel's ray, nor its neighbours', reads past it).
    const float reach = texelFetch(uReach, xy, 0).r;
    for (int z = 0; z < cells.z; ++z) {
        if (before > reach) {
            break;
        }
        const float w = float(z + 1) / float(cells.z);
        const float after = params.eye.w * w * w;
        const float len = (after - before) * stretch;
        before = after;
        const vec4 lit = texelFetch(uLit, ivec3(xy, z), 0);
        const float s = max(lit.a, 0.0);
        const float kept = exp(-s * len);
        // The slice's light (per unit of extinction, its scattering being the extinction) over its length.
        const vec3 slice = s > 1e-7 ? lit.rgb * ((1.0 - kept) / s) : lit.rgb * len;
        sum += max(through, params.size.w) * slice;
        through *= kept;
        imageStore(uOut, ivec3(xy, z), vec4(sum, through));
    }
}
