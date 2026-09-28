#version 450 core
// Engine post-fx: CRT-style horizontal scanlines. Single-input.
// Params: color_a = (intensity, line_count, _, _).
layout(location = 0) out vec4 fColor;
layout(set = 2, binding = 0) uniform sampler2D sTexture;
layout(set = 3, binding = 0) uniform Params {
    vec4 color_a; // x=intensity y=line_count
    vec4 color_b;
    vec4 misc;
    vec4 _pad;
};
layout(location = 0) in struct { vec4 Color; vec2 UV; } In;

void main() {
    vec3 c = texture(sTexture, In.UV).rgb;
    float intensity = clamp(color_a.x, 0.0, 1.0);
    float lines = color_a.y > 0.0 ? color_a.y : 240.0;
    float s = 0.5 + 0.5 * sin(In.UV.y * lines * 6.28318530718);
    c *= mix(1.0, 0.6 + 0.4 * s, intensity);
    fColor = vec4(c, 1.0);
}
