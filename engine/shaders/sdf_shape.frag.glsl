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

#include "kin_sdf_shape.inc.glsl"

void main() {
    fColor = kin_shape_color(In.UV, Size, Kind, In.Color, Stroke);
    if (fColor.a <= 0.0) discard;
}
