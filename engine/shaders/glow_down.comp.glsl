#version 450 core
// The glow's chain (Glow3D), down: each level half the one before, read with 13 taps (Jimenez, "Next Generation Post
// Processing in Call of Duty: Advanced Warfare", 2014: no aliasing as it shrinks). The first level from the picture
// also weighs each group of taps down by its brightness (Karis: a lone bright pixel does not flicker) and keeps the
// light past the threshold, with the soft knee kin's bloom has.
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D uSource; // linear sampler
layout(set = 1, binding = 0, rgba16f) uniform writeonly image2D uLevel;
layout(set = 2, binding = 0) uniform Params {
    vec4 shape; // x: 1 for the first level (Karis and threshold), y: the threshold
}
params;

const vec3 luma_weights = vec3(0.2126, 0.7152, 0.0722);

float karis(vec3 c) {
    return 1.0 / (1.0 + dot(c, luma_weights));
}

void main() {
    const ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    const ivec2 size = imageSize(uLevel);
    if (any(greaterThanEqual(p, size))) {
        return;
    }
    const vec2 uv = (vec2(p) + 0.5) / vec2(size);
    const vec2 t = 1.0 / vec2(textureSize(uSource, 0));
    const vec3 a = textureLod(uSource, uv + t * vec2(-2, -2), 0.0).rgb;
    const vec3 b = textureLod(uSource, uv + t * vec2(0, -2), 0.0).rgb;
    const vec3 c = textureLod(uSource, uv + t * vec2(2, -2), 0.0).rgb;
    const vec3 d = textureLod(uSource, uv + t * vec2(-2, 0), 0.0).rgb;
    const vec3 e = textureLod(uSource, uv, 0.0).rgb;
    const vec3 f = textureLod(uSource, uv + t * vec2(2, 0), 0.0).rgb;
    const vec3 g = textureLod(uSource, uv + t * vec2(-2, 2), 0.0).rgb;
    const vec3 h = textureLod(uSource, uv + t * vec2(0, 2), 0.0).rgb;
    const vec3 i = textureLod(uSource, uv + t * vec2(2, 2), 0.0).rgb;
    const vec3 j = textureLod(uSource, uv + t * vec2(-1, -1), 0.0).rgb;
    const vec3 k = textureLod(uSource, uv + t * vec2(1, -1), 0.0).rgb;
    const vec3 l = textureLod(uSource, uv + t * vec2(-1, 1), 0.0).rgb;
    const vec3 m = textureLod(uSource, uv + t * vec2(1, 1), 0.0).rgb;
    vec3 sum;
    if (params.shape.x > 0.5) {
        const vec3 groups[5] = vec3[](
            (j + k + l + m) * 0.25, (a + b + d + e) * 0.25, (b + c + e + f) * 0.25, (d + e + g + h) * 0.25,
            (e + f + h + i) * 0.25);
        const float shares[5] = float[](0.5, 0.125, 0.125, 0.125, 0.125);
        float weights = 0.0;
        sum = vec3(0.0);
        for (int n = 0; n < 5; ++n) {
            const float w = shares[n] * karis(groups[n]);
            sum += groups[n] * w;
            weights += w;
        }
        sum /= weights;
        const float luma = dot(sum, luma_weights);
        sum *= max(luma - params.shape.y, 0.0) / max(luma, 1e-4);
    } else {
        sum = e * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 + (j + k + l + m) * 0.125;
    }
    imageStore(uLevel, p, vec4(max(sum, vec3(0.0)), 1.0));
}
