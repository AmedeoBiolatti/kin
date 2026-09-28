#version 450 core
// Engine transition: noise dissolve. Single-input (the captured scene snapshot).
// Params: color_a = cover color rgb; misc.x = progress (0..1); misc.y = direction
// (0 = out: scene -> color, 1 = in: color -> scene). Output is opaque (replaces screen).
layout(location = 0) out vec4 fColor;
layout(set = 2, binding = 0) uniform sampler2D sTexture;
layout(set = 3, binding = 0) uniform Params {
    vec4 color_a; // rgb = cover color
    vec4 color_b;
    vec4 misc;    // x=progress y=direction
    vec4 _pad;
};
layout(location = 0) in struct { vec4 Color; vec2 UV; } In;

float hash(vec2 p) {
    vec3 p3 = fract(vec3(p.x, p.y, p.x) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

void main() {
    vec3 scene = texture(sTexture, In.UV).rgb;
    float p = clamp(misc.x, 0.0, 1.0);
    float cover = misc.y < 0.5 ? p : (1.0 - p); // out grows cover, in shrinks it
    float n = hash(floor(In.UV * 220.0));        // per-cell threshold
    float m = step(n, cover);                     // 1 = covered by color
    fColor = vec4(mix(scene, color_a.rgb, m), 1.0);
}
