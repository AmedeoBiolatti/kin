// Depth-aware visibility blur shared by RG8 full/detail AO and R8 contact shadows.
// KIN_LINEAR_DEPTH reads the first level of the linear depth chain; otherwise read reverse-Z depth.
#ifdef KIN_CONTACT_BLUR
#define KIN_VISIBILITY float
#define KIN_CHANNELS r
layout(set = 1, binding = 0, r8) uniform writeonly image2D uResult;
vec4 packed_visibility(float value) { return vec4(value); }
#else
#define KIN_VISIBILITY vec2
#define KIN_CHANNELS rg
layout(set = 1, binding = 0, rg8) uniform writeonly image2D uResult;
vec4 packed_visibility(vec2 value) { return vec4(value, 0.0, 0.0); }
#endif
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D uDepth;
layout(set = 0, binding = 1) uniform sampler2D uSource;
layout(set = 2, binding = 0) uniform Params {
    vec4 sizes; // xy: result size, zw: depth size
    vec4 step;  // xy: direction in result pixels
#ifndef KIN_LINEAR_DEPTH
    vec4 depth; // inverse projection's z/w coefficients
#endif
} params;

const int TAPS = 3;
const float EDGE = 0.03;
const float WEIGHTS[TAPS + 1] = float[](1.0, 0.8, 0.5, 0.25);

float view_z(float d) {
#ifdef KIN_LINEAR_DEPTH
    return d;
#else
    return (params.depth.x * d + params.depth.y) / (params.depth.z * d + params.depth.w);
#endif
}

void main() {
    const ivec2 xy = ivec2(gl_GlobalInvocationID.xy);
    const ivec2 size = ivec2(params.sizes.xy);
    if (xy.x >= size.x || xy.y >= size.y)
        return;
    const ivec2 depth_size = ivec2(params.sizes.zw);
    const vec2 scale = params.sizes.zw / params.sizes.xy;
    const ivec2 direction = ivec2(params.step.xy);
    const float centre_depth = texelFetch(uDepth, min(ivec2(vec2(xy) * scale), depth_size - 1), 0).r;
    if (centre_depth <= 0.0) {
        imageStore(uResult, xy, vec4(1.0));
        return;
    }
    const float centre = view_z(centre_depth);
#ifdef KIN_LINEAR_DEPTH
    const float edge_scale = 1.0 / (EDGE * max(centre_depth, 1e-3));
#endif
    KIN_VISIBILITY sum = KIN_VISIBILITY(0.0);
    float total = 0.0;
    for (int i = -TAPS; i <= TAPS; ++i) {
        const ivec2 q = clamp(xy + direction * i, ivec2(0), size - 1);
        const float d = texelFetch(uDepth, min(ivec2(vec2(q) * scale), depth_size - 1), 0).r;
#ifdef KIN_LINEAR_DEPTH
        const float w = WEIGHTS[abs(i)] * (d > 0.0 ? exp(-abs(d - centre_depth) * edge_scale) : 0.0);
#else
        const float w = WEIGHTS[abs(i)] * (d > 0.0 ? exp(-abs(view_z(d) - centre) / (EDGE * max(abs(centre), 1e-3))) : 0.0);
#endif
        sum += texelFetch(uSource, q, 0).KIN_CHANNELS * w;
        total += w;
    }
    imageStore(uResult, xy, packed_visibility(total > 0.0 ? sum / total : KIN_VISIBILITY(1.0)));
}
