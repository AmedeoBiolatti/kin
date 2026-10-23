#version 450 core
// Explicit pre-feature ABI: three uniform blocks, ten samplers, 64 parameter bytes.
#define KIN_NO_CUSTOM_PARAMS
#include "kin_material.inc.glsl"
layout(set = 3, binding = 2) uniform LegacyParams { vec4 values[4]; } legacy;
void main() {
    vec4 base = kin_base_color();
    FragColor = kin_output(base.rgb * legacy.values[0].rgb, base.a);
}
