#version 450 core
// The glow (Glow3D) as the view's output reads it in place of kin's own bloom: the glow (the wide chain's, or kin's
// own), the ghosts and halo, the streaks in their colour, and the lens dirt lit by the light near it (a wide level of
// the chain). Half the view's size.
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D uGlow;   // linear samplers throughout
layout(set = 0, binding = 1) uniform sampler2D uFlare;
layout(set = 0, binding = 2) uniform sampler2D uStreak;
layout(set = 0, binding = 3) uniform sampler2D uWide;   // a wide level of the chain, for the dirt's light
layout(set = 0, binding = 4) uniform sampler2D uDirt;
layout(set = 1, binding = 0, rgba16f) uniform writeonly image2D uResult;
layout(set = 2, binding = 0) uniform Params {
    vec4 parts;  // x: 1 with ghosts or halo, y: the dirt's strength (0: none)
    vec4 streak; // rgb: the streaks' colour times their strength (0: none)
}
params;

void main() {
    const ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    const ivec2 size = imageSize(uResult);
    if (any(greaterThanEqual(p, size))) {
        return;
    }
    const vec2 uv = (vec2(p) + 0.5) / vec2(size);
    vec3 light = textureLod(uGlow, uv, 0.0).rgb;
    if (params.parts.x > 0.5) {
        light += textureLod(uFlare, uv, 0.0).rgb;
    }
    if (any(greaterThan(params.streak.rgb, vec3(0.0)))) {
        light += textureLod(uStreak, uv, 0.0).rgb * params.streak.rgb;
    }
    if (params.parts.y > 0.0) {
        light += textureLod(uDirt, uv, 0.0).rgb * textureLod(uWide, uv, 0.0).rgb * params.parts.y;
    }
    imageStore(uResult, p, vec4(light, 1.0));
}
