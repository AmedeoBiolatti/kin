#version 450 core
#include "kin_material.inc.glsl"

void main() {
    const vec4 base = kin_base_color();
    FragColor = kin_output_unfogged(base.rgb * kin_param(0).rgb, base.a);
}
