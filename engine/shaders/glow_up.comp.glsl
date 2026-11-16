#version 450 core
// The glow's chain (Glow3D), up: each level its own light and the wider level below it, read with a 3 x 3 tent,
// mixed by the radius ((1 - r) here, r below: the weights sum to one, so the glow holds the light it was made of).
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D uHere;  // this level, down (linear sampler)
layout(set = 0, binding = 1) uniform sampler2D uBelow; // the next smaller level, up
layout(set = 1, binding = 0, rgba16f) uniform writeonly image2D uLevel;
layout(set = 2, binding = 0) uniform Params {
    vec4 shape; // x: the radius
}
params;

void main() {
    const ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    const ivec2 size = imageSize(uLevel);
    if (any(greaterThanEqual(p, size))) {
        return;
    }
    const vec2 uv = (vec2(p) + 0.5) / vec2(size);
    const vec2 t = 1.0 / vec2(textureSize(uBelow, 0));
    vec3 below = textureLod(uBelow, uv, 0.0).rgb * 4.0;
    below += (textureLod(uBelow, uv + vec2(-t.x, 0), 0.0).rgb + textureLod(uBelow, uv + vec2(t.x, 0), 0.0).rgb +
              textureLod(uBelow, uv + vec2(0, -t.y), 0.0).rgb + textureLod(uBelow, uv + vec2(0, t.y), 0.0).rgb) *
             2.0;
    below += textureLod(uBelow, uv - t, 0.0).rgb + textureLod(uBelow, uv + t, 0.0).rgb +
             textureLod(uBelow, uv + vec2(t.x, -t.y), 0.0).rgb + textureLod(uBelow, uv + vec2(-t.x, t.y), 0.0).rgb;
    below /= 16.0;
    const vec3 here = textureLod(uHere, uv, 0.0).rgb;
    imageStore(uLevel, p, vec4(mix(here, below, params.shape.x), 1.0));
}
