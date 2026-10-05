#version 450 core
// kin GPU backend: anti-aliased shapes (Renderer2D::draw_shape and the shape
// primitives). Custom.x is where the fragment lies in the shape's soft edge:
// 0 on and inside the outline, -1 at the soft edge's rim. Its change per screen
// pixel turns it into one pixel of fade outside the outline, whatever the
// shape's scale or turn.
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
    float coverage = clamp(1.0 + edge / max(per_pixel, 1e-6), 0.0, 1.0);
    vec4 color = In.Color * texture(sTexture, In.UV);
    fColor = vec4(color.rgb, color.a * coverage);
}
