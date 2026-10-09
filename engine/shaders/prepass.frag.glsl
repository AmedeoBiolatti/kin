#version 450 core
// kin's 3D ambient occlusion pre-pass: the world normal of what is nearest,
// on the side the viewer sees, octahedral in two floats.
layout(location = 0) in vec3 vNormal;
layout(location = 3) in vec3 vWorld;
layout(location = 0) out vec2 FragNormal;

#include "kin_prepass.inc.glsl"

void main() {
    FragNormal = octahedral(kin_prepass_normal());
}
