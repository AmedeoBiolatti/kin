#version 450 core
// The stock mesh vertex shader's colour/UV, with precisely its masked alpha test.
// Using the main vertex shader keeps projected positions identical between passes.
layout(location = 1) in vec4 vColor;
layout(location = 2) in vec2 vUV;
#include "kin_cutout.inc.glsl"

void main() {
    if (vColor.a * texture(uBase, vUV).a < material.cutoff.x) discard;
}
