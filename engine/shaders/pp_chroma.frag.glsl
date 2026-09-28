#version 450 core
// Engine post-fx: chromatic aberration (radial R/B split, strongest at edges). Single-input.
// Params: color_a = (amount, _, _, _). amount ~ 0.002..0.01 of uv.
layout(location = 0) out vec4 fColor;
layout(set = 2, binding = 0) uniform sampler2D sTexture;
layout(set = 3, binding = 0) uniform Params {
    vec4 color_a; // x=amount
    vec4 color_b;
    vec4 misc;
    vec4 _pad;
};
layout(location = 0) in struct { vec4 Color; vec2 UV; } In;

void main() {
    vec2 uv = In.UV;
    float amount = color_a.x > 0.0 ? color_a.x : 0.004;
    vec2 dir = (uv - vec2(0.5));
    vec2 off = dir * amount;
    float r = texture(sTexture, uv + off).r;
    float g = texture(sTexture, uv).g;
    float b = texture(sTexture, uv - off).b;
    fColor = vec4(r, g, b, 1.0);
}
