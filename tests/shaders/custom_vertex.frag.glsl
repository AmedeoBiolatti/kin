#version 450 core
// Test material for renderer_tests: draw_shader_geometry's per-vertex `custom`
// as the colour, times the vertex colour and source 0, so each input shows.
layout(location = 0) out vec4 fColor;
layout(set = 2, binding = 0) uniform sampler2D sSource0;
layout(location = 0) in struct { vec4 Color; vec2 UV; } In;
layout(location = 2) in vec4 vCustom;

void main() {
    fColor = vCustom * In.Color * texture(sSource0, In.UV);
}
