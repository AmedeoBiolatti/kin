#version 450 core
// Engine post-fx: bloom bright-pass (keep only luminance above a threshold). Single-input.
// Params: color_a = (threshold, intensity, _, _).
layout(location = 0) out vec4 fColor;
layout(set = 2, binding = 0) uniform sampler2D sTexture;
layout(set = 3, binding = 0) uniform Params {
    vec4 color_a; // x=threshold y=intensity
    vec4 color_b;
    vec4 misc;
    vec4 _pad;
};
layout(location = 0) in struct { vec4 Color; vec2 UV; } In;

void main() {
    vec3 c = texture(sTexture, In.UV).rgb;
    float threshold = color_a.x >= 0.0 ? color_a.x : 0.7;
    float intensity = color_a.y > 0.0 ? color_a.y : 1.0;
    float l = dot(c, vec3(0.299, 0.587, 0.114));
    float f = max(0.0, l - threshold) / max(0.0001, 1.0 - threshold);
    fColor = vec4(c * f * intensity, 1.0);
}
