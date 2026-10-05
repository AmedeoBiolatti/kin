#version 450 core
// kin GPU backend: shape primitives (circles, ellipses, rounded rectangles),
// one instance each: a quad round the primitive, a stroke and a margin wider
// than it, mapped by its transform, carrying its own coordinates to
// sdf_shape.frag. Outputs start like textured_quad.vert's, so any fragment
// shader that reads only those (the overdraw view's) serves too.
layout(location = 0) in vec4 aLinear; // a, b, c, d: own units to draw coordinates
layout(location = 1) in vec4 aPlace;  // tx, ty, half width, half height
layout(location = 2) in vec4 aShape;  // corner radius, half stroke width, margin, kind
layout(location = 3) in vec4 aFill;
layout(location = 4) in vec4 aStroke;

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
    vec4 Color; // the fill
    vec2 UV;    // the position in the primitive's own units
} Out;
layout(location = 2) flat out vec4 Size;   // half width, half height, corner radius, half stroke
layout(location = 3) flat out vec4 Stroke; // the stroke's colour
layout(location = 4) flat out float Kind;  // 0 rounded rectangle, 1 sharp rectangle, 2 ellipse

const vec2 corners[6] = vec2[6](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
                                vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));

void main() {
    const vec2 local = corners[gl_VertexIndex] * (aPlace.zw + vec2(aShape.y + aShape.z));
    const vec2 pos = vec2(aLinear.x * local.x + aLinear.z * local.y + aPlace.x,
                          aLinear.y * local.x + aLinear.w * local.y + aPlace.y);
    Out.Color = decode_color(aFill);
    Out.UV = local;
    Size = vec4(aPlace.zw, aShape.x, aShape.y);
    Stroke = decode_color(aStroke);
    Kind = aShape.w;
    gl_Position = vec4(pos * ubo.uScale + ubo.uTranslate, 0.0, 1.0);
    gl_Position.y *= -1.0;
}
