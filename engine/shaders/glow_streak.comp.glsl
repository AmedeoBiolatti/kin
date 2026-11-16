#version 450 core
// Anamorphic streaks (Glow3D): the bright light drawn out sideways, each pixel the light along its row, weighed down
// exponentially with the distance (a third of the streak's length its scale), normalised. 48 samples each way.
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D uBright; // a level of the glow's chain, down (linear sampler)
layout(set = 1, binding = 0, rgba16f) uniform writeonly image2D uStreak;
layout(set = 2, binding = 0) uniform Params {
    vec4 shape; // x: the streak's length each way (UV across)
}
params;

void main() {
    const ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    const ivec2 size = imageSize(uStreak);
    if (any(greaterThanEqual(p, size))) {
        return;
    }
    const vec2 uv = (vec2(p) + 0.5) / vec2(size);
    const float reach = params.shape.x;
    vec3 sum = textureLod(uBright, uv, 0.0).rgb;
    float weights = 1.0;
    for (int i = 1; i <= 48; ++i) {
        const float x = reach * float(i) / 48.0;
        const float w = exp(-3.0 * float(i) / 48.0);
        sum += (textureLod(uBright, uv + vec2(x, 0.0), 0.0).rgb + textureLod(uBright, uv - vec2(x, 0.0), 0.0).rgb) * w;
        weights += 2.0 * w;
    }
    imageStore(uStreak, p, vec4(sum / weights, 1.0));
}
