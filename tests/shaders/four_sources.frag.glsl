#version 450 core
// Test material for renderer_tests: samples four textures (fragment sampler slots
// 0..3) and adds slot 0's red, slot 1's green, slot 2's blue and slot 3's rgb, so a
// missing or misplaced binding changes the output colour.
layout(location = 0) out vec4 fColor;
layout(set = 2, binding = 0) uniform sampler2D sSource0;
layout(set = 2, binding = 1) uniform sampler2D sSource1;
layout(set = 2, binding = 2) uniform sampler2D sSource2;
layout(set = 2, binding = 3) uniform sampler2D sSource3;
layout(location = 0) in struct { vec4 Color; vec2 UV; } In;

void main() {
    vec3 rgb = vec3(texture(sSource0, In.UV).r, texture(sSource1, In.UV).g, texture(sSource2, In.UV).b);
    fColor = vec4(min(rgb + texture(sSource3, In.UV).rgb, vec3(1.0)), 1.0);
}
