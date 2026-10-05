#version 450 core
// kin GPU backend default 2D vertex shader (ported from v0 renderer_gpu).
// Maps scene-pixel coords -> NDC with a top-left origin (Y flipped).
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

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

void main() {
    Out.Color = decode_color(aColor);
    Out.UV = aUV;
    gl_Position = vec4(aPos * ubo.uScale + ubo.uTranslate, 0.0, 1.0);
    gl_Position.y *= -1.0;
}
