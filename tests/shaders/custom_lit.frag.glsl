#version 450 core
// A 3D material shader (render3d_tests): kin's own lighting, called by hand,
// times a colour from the material's parameters (white: as the built-in).
#include "kin_material.inc.glsl"

void main() {
    const vec4 base = kin_base_color();
    const vec2 mr = kin_metallic_roughness();
    FragColor = kin_output(kin_lit(base.rgb, kin_normal(), mr.x, mr.y, kin_occlusion()) * kin_param(0).rgb, base.a);
}
