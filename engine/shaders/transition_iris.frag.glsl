#version 450 core
// Engine transition: iris (radial circle) wipe. Single-input (the captured scene snapshot).
// Params: color_a = cover color rgb; misc.x = progress (0..1); misc.y = direction
// (0 = out: circle of color closes in, 1 = in: circle opens out). misc.z = aspect (w/h).
layout(location = 0) out vec4 fColor;
layout(set = 2, binding = 0) uniform sampler2D sTexture;
layout(set = 3, binding = 0) uniform Params {
    vec4 color_a; // rgb = cover color
    vec4 color_b;
    vec4 misc;    // x=progress y=direction z=aspect
    vec4 _pad;
};
layout(location = 0) in struct { vec4 Color; vec2 UV; } In;

void main() {
    vec3 scene = texture(sTexture, In.UV).rgb;
    float p = clamp(misc.x, 0.0, 1.0);
    float cover = misc.y < 0.5 ? p : (1.0 - p);
    float aspect = misc.z > 0.0 ? misc.z : 1.0;
    vec2 d = (In.UV - vec2(0.5)) * vec2(aspect, 1.0);
    float dist = length(d) / 0.75;               // ~1 at the corners
    // radius shrinks from 1 (all scene) to 0 (all color) as cover -> 1
    float radius = 1.0 - cover;
    float m = smoothstep(radius - 0.04, radius + 0.04, dist); // 1 outside radius = covered
    fColor = vec4(mix(scene, color_a.rgb, m), 1.0);
}
