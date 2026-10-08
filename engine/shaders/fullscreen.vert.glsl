#version 450 core
// One triangle over the whole target (kin's 3D view passes), UV (0, 0) at its
// top-left; no vertex buffer.
layout(location = 0) out vec2 vUV;

void main() {
    const vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    vUV = p;
    gl_Position = vec4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
}
