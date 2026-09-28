#version 450 core
// Engine post-fx: separable Gaussian blur (run horizontally then vertically). Single-input.
// Params: color_a = (dir_x, dir_y, _, _) — the per-tap uv offset (= texel * radius * axis).
// 5-tap linear-sampled Gaussian (offsets 1.3846, 3.2308).
layout(location = 0) out vec4 fColor;
layout(set = 2, binding = 0) uniform sampler2D sTexture;
layout(set = 3, binding = 0) uniform Params {
    vec4 color_a; // xy = uv step direction
    vec4 color_b;
    vec4 misc;
    vec4 _pad;
};
layout(location = 0) in struct { vec4 Color; vec2 UV; } In;

void main() {
    vec2 uv = In.UV;
    vec2 d = color_a.xy;
    vec3 sum = texture(sTexture, uv).rgb * 0.2270270270;
    sum += texture(sTexture, uv + d * 1.3846153846).rgb * 0.3162162162;
    sum += texture(sTexture, uv - d * 1.3846153846).rgb * 0.3162162162;
    sum += texture(sTexture, uv + d * 3.2307692308).rgb * 0.0702702703;
    sum += texture(sTexture, uv - d * 3.2307692308).rgb * 0.0702702703;
    fColor = vec4(sum, 1.0);
}
