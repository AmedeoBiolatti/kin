#version 450 core
// Engine post-fx: vignette (darken toward the edges). Single-input.
// Params: color_a = (strength, radius, softness, _). color_a defaults to a gentle vig.
layout(location = 0) out vec4 fColor;
layout(set = 2, binding = 0) uniform sampler2D sTexture;
layout(set = 3, binding = 0) uniform Params {
    vec4 color_a; // x=strength y=radius z=softness
    vec4 color_b;
    vec4 misc;
    vec4 _pad;
};
layout(location = 0) in struct { vec4 Color; vec2 UV; } In;

void main() {
    vec4 c = texture(sTexture, In.UV);
    float strength = clamp(color_a.x, 0.0, 1.0);
    float radius   = color_a.y > 0.0 ? color_a.y : 0.75;
    float soft     = color_a.z > 0.0 ? color_a.z : 0.45;
    // Distance from center, aspect-agnostic (uv space). 1 at center -> 0 in corners.
    float d = length(In.UV - vec2(0.5));
    float vig = smoothstep(radius, radius - soft, d);
    c.rgb *= mix(1.0, vig, strength);
    fColor = vec4(c.rgb, 1.0);
}
