#version 450 core
// kin_draw_bench / renderer_tests: a smooth, costly effect from the UV alone
// (layered sines), for drawing at a lower resolution.
layout(location = 0) out vec4 fColor;
layout(set = 2, binding = 0) uniform sampler2D sUnused;
layout(location = 0) in struct { vec4 Color; vec2 UV; } In;

void main() {
    float v = 0.0;
    for (int i = 1; i <= 768; ++i) {
        v += sin(In.UV.x * float(i) * 0.37 + In.UV.y * float(i) * 0.21) / float(i);
    }
    fColor = vec4(0.5 + 0.25 * v, In.UV.x, In.UV.y, 1.0);
}
