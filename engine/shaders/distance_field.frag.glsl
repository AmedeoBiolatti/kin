#version 450 core
// kin GPU backend: distance-field images (Renderer2D::draw_distance_field:
// scalable text, icons). The texture's alpha is a signed distance to the
// outline, 0.5 on it; its change per screen pixel makes the edge one pixel
// soft at any size or turn. An outline, if any, lies outside it.
layout(location = 0) out vec4 fColor;
layout(set = 2, binding = 0) uniform sampler2D sTexture;
layout(set = 3, binding = 0) uniform Style {
    vec4 outline_color;
    vec4 params; // x: texels the alpha spans either side of the outline; y: outline width, texels
} style;
layout(location = 0) in struct {
    vec4 Color;
    vec2 UV;
} In;

void main() {
    const float d = (texture(sTexture, In.UV).a - 0.5) * 2.0 * style.params.x; // texels, inside > 0
    const float pixel = max(length(vec2(dFdx(d), dFdy(d))), 1e-4);
    const float fill = clamp(d / pixel + 0.5, 0.0, 1.0);
    const float ring = style.params.y > 0.0 ? clamp((d + style.params.y) / pixel + 0.5, 0.0, 1.0) : 0.0;
    // The fill over its outline, premultiplied, then back to straight alpha.
    const vec4 f = vec4(In.Color.rgb, 1.0) * In.Color.a * fill;
    const vec4 o = vec4(style.outline_color.rgb, 1.0) * style.outline_color.a * ring;
    const vec4 c = f + o * (1.0 - f.a);
    if (c.a <= 0.0) {
        discard;
    }
    fColor = vec4(c.rgb / c.a, c.a);
}
