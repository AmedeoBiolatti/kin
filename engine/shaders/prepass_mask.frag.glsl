#version 450 core
// The pre-pass for cut-out materials (Blend3D::Mask): prepass.frag's normal,
// where the base colour's alpha reaches the cutoff, as mesh.frag keeps it.
layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUV;
layout(location = 2) in float vAlpha;
layout(location = 0) out vec2 FragNormal;

#include "kin_cutout.inc.glsl"
#include "kin_prepass.inc.glsl"

void main() {
    if (vAlpha * texture(uBase, vUV).a < material.cutoff.x) {
        discard;
    }
    FragNormal = octahedral(normalize(vNormal) * (gl_FrontFacing ? 1.0 : -1.0));
}
