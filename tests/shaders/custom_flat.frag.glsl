#version 450 core
// A 3D material shader (render3d_tests) that ignores the lights: the colour
// in the material's second parameter, brighter toward +x (the world position).
#include "kin_material.inc.glsl"

void main() {
    FragColor = kin_output(kin_param(1).rgb * (0.5 + 0.5 * clamp(kin_position().x, 0.0, 1.0)), 1.0);
}
