#version 300 es
precision highp float;
in vec2 vLocal;
flat in vec4 vSize;
flat in vec4 vFill;
flat in vec4 vStroke;
flat in float vKind;
out vec4 outColor;
#include "../kin_sdf_shape.inc.glsl"
void main() {
    outColor = kin_shape_color(vLocal, vSize, vKind, vFill, vStroke);
    if (outColor.a <= 0.0) discard;
}
