#version 450 core
// kin GPU backend: draw_shader_geometry() triangles. As textured_quad.vert, plus
// a free vec4 per vertex passed on at location 2, for the material fragment
// shader to read (`layout(location = 2) in vec4 vCustom;`).
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;
layout(location = 3) in vec4 aCustom;

layout(set = 1, binding = 0) uniform UBO {
    vec2 uScale;
    vec2 uTranslate;
} ubo;

layout(location = 0) out struct {
    vec4 Color;
    vec2 UV;
} Out;
layout(location = 2) out vec4 Custom;

void main() {
    Out.Color = aColor;
    Out.UV = aUV;
    Custom = aCustom;
    gl_Position = vec4(aPos * ubo.uScale + ubo.uTranslate, 0.0, 1.0);
    gl_Position.y *= -1.0;
}
