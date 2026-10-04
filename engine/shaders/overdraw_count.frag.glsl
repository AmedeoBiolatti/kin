#version 450 core
// The overdraw view (Renderer2D::set_overdraw_view): every draw, whatever its
// own shader, adds 1/32 where it covers (additive blend), so the target's red
// channel counts the layers, up to 32.
layout(location = 0) out vec4 fColor;
layout(set = 2, binding = 0) uniform sampler2D sUnused;
layout(location = 0) in struct { vec4 Color; vec2 UV; } In;

void main() {
    fColor = vec4(vec3(1.0 / 32.0), 1.0);
}
