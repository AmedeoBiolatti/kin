#version 450 core
// Engine transition: pixelate-and-fade. Single-input (the captured scene snapshot).
// Params: color_a = cover color rgb; misc.x = progress (0..1); misc.y = direction
// (0 = out, 1 = in). As cover grows the scene blocks up and fades toward color_a.
layout(location = 0) out vec4 fColor;
layout(set = 2, binding = 0) uniform sampler2D sTexture;
layout(set = 3, binding = 0) uniform Params {
    vec4 color_a; // rgb = cover color
    vec4 color_b;
    vec4 misc;    // x=progress y=direction
    vec4 _pad;
};
layout(location = 0) in struct { vec4 Color; vec2 UV; } In;

void main() {
    float p = clamp(misc.x, 0.0, 1.0);
    float cover = misc.y < 0.5 ? p : (1.0 - p);
    float cells = mix(220.0, 8.0, cover);        // fewer cells = chunkier blocks
    vec2 quv = (floor(In.UV * cells) + 0.5) / cells;
    vec3 scene = texture(sTexture, quv).rgb;
    fColor = vec4(mix(scene, color_a.rgb, cover * cover), 1.0);
}
