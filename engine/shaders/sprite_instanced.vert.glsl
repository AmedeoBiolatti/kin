#version 450 core
// kin GPU backend: draw_sprites() quads, one instance each. The quad's six
// corners come from gl_VertexIndex; outputs and the pixel -> NDC mapping match
// textured_quad.vert, so the default fragment shader serves both.
layout(location = 0) in vec4 aDest;  // x, y, w, h (pixels)
layout(location = 1) in vec4 aUV;    // u0, v0, u1, v1
layout(location = 2) in vec4 aTurn;  // pivot x, pivot y (pixels), cos, sin
layout(location = 3) in vec4 aColor;

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

// Two triangles, in the order the CPU quad path emits them.
const vec2 corners[6] = vec2[6](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
                                vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));

void main() {
    const vec2 c = corners[gl_VertexIndex];
    vec2 pos = vec2(c.x == 0.0 ? aDest.x : aDest.x + aDest.z, c.y == 0.0 ? aDest.y : aDest.y + aDest.w);
    if (aTurn.w != 0.0 || aTurn.z != 1.0) {
        const vec2 d = pos - aTurn.xy;
        pos = aTurn.xy + vec2(d.x * aTurn.z - d.y * aTurn.w, d.x * aTurn.w + d.y * aTurn.z);
    }
    Out.Color = decode_color(aColor);
    Out.UV = vec2(c.x == 0.0 ? aUV.x : aUV.z, c.y == 0.0 ? aUV.y : aUV.w);
    gl_Position = vec4(pos * ubo.uScale + ubo.uTranslate, 0.0, 1.0);
    gl_Position.y *= -1.0;
}
