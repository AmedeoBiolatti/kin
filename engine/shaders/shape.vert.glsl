#version 450 core
// kin GPU backend: shape vertices (Renderer2D's shapes, drawn indexed): a
// position, a colour, and how far past the outline the vertex lies, which
// shape.frag turns into a one-pixel soft edge.
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec4 aColor;
layout(location = 2) in float aEdge;

layout(set = 1, binding = 0) uniform UBO {
    vec2 uScale;
    vec2 uTranslate;
    vec4 uFlags;
} ubo;

// Linear pipeline (uFlags.x): colours are given in sRGB, blended in linear light.
vec4 decode_color(vec4 c) {
    if (ubo.uFlags.x < 0.5) {
        return c;
    }
    return vec4(mix(c.rgb / 12.92, pow((c.rgb + 0.055) / 1.055, vec3(2.4)), step(0.04045, c.rgb)), c.a);
}

layout(location = 0) out struct {
    vec4 Color;
    vec2 UV;
} Out;
layout(location = 2) out vec4 Custom;

void main() {
    Out.Color = decode_color(aColor);
    Out.UV = vec2(0.0);
    Custom = vec4(aEdge, 0.0, 0.0, 0.0);
    gl_Position = vec4(aPos * ubo.uScale + ubo.uTranslate, 0.0, 1.0);
    gl_Position.y *= -1.0;
}
