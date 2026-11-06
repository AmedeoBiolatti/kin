#version 450 core
// kin GPU backend: anti-aliased shapes (Renderer2D::draw_shape and the shape
// primitives). Custom.x is where the fragment lies in the shape's soft edge:
// 0 on and inside the outline, -1 at the soft edge's rim. Its change per screen
// pixel turns it into the fragment's distance past the outline in pixels,
// whatever the shape's scale or turn.
//
// The edge rule, the same as kin_edge_cover's in kin_sdf_shape.inc.glsl: solid
// up to the outline, then from half coverage to none over the half pixel past
// it (exact outside the outline; the half pixel inside is solid, which keeps
// shapes that share an edge seamless). docs/rendering.md, "Shape edges", has
// the measurements and how to go to exact coverage on both sides instead.
layout(location = 0) out vec4 fColor;
layout(set = 2, binding = 0) uniform sampler2D sTexture;
layout(location = 0) in struct {
    vec4 Color;
    vec2 UV;
} In;
layout(location = 2) in vec4 Custom;

void main() {
    float edge = Custom.x;
    float per_pixel = length(vec2(dFdx(edge), dFdy(edge)));
    float past = -edge / max(per_pixel, 1e-6); // pixels past the outline
    float coverage = past > 0.0 ? clamp(0.5 - past, 0.0, 1.0) : 1.0;
    vec4 color = In.Color * texture(sTexture, In.UV);
    fColor = vec4(color.rgb, color.a * coverage);
}
