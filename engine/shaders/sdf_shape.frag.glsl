#version 450 core
// kin GPU backend: shape primitives, from their distance to the outline. The
// fill is whole up to the outline and fades over the pixel past it; the stroke
// is centred on the outline and fades over the pixel past each side, as the
// tessellated shapes do (shape.frag). The distance's change per pixel keeps
// that one pixel at any scale or turn.
layout(location = 0) out vec4 fColor;
layout(set = 2, binding = 0) uniform sampler2D sTexture; // unused: bound for every draw
layout(location = 0) in struct {
    vec4 Color;
    vec2 UV;
} In;
layout(location = 2) flat in vec4 Size;
layout(location = 3) flat in vec4 Stroke;
layout(location = 4) flat in float Kind;

// A rounded rectangle's signed distance (exact). Sharp, its outside is
// measured square, so a stroke keeps mitred corners.
float rounded_box(vec2 p, vec2 half_size, float r, bool sharp) {
    const vec2 q = abs(p) - half_size + r;
    if (sharp) {
        return max(q.x, q.y);
    }
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

// An ellipse's, estimated: exact on the outline and close near it.
float ellipse(vec2 p, vec2 ab) {
    const float k0 = length(p / ab);
    const float k1 = length(p / (ab * ab));
    return k1 > 0.0 ? k0 * (k0 - 1.0) / k1 : -min(ab.x, ab.y);
}

void main() {
    const vec2 p = In.UV;
    const float d = Kind > 1.5 ? ellipse(p, Size.xy) : rounded_box(p, Size.xy, Size.z, Kind > 0.5);
    const float pixel = max(length(vec2(dFdx(d), dFdy(d))), 1e-6);
    const float fill_cover = clamp(1.0 - d / pixel, 0.0, 1.0);
    const float stroke_cover = Size.w > 0.0 ? clamp(1.0 - (abs(d) - Size.w) / pixel, 0.0, 1.0) : 0.0;
    // The stroke over the fill, premultiplied, then back to straight alpha.
    const vec4 f = vec4(In.Color.rgb, 1.0) * In.Color.a * fill_cover;
    const vec4 s = vec4(Stroke.rgb, 1.0) * Stroke.a * stroke_cover;
    const vec4 c = s + f * (1.0 - s.a);
    if (c.a <= 0.0) {
        discard;
    }
    fColor = vec4(c.rgb / c.a, c.a);
}
