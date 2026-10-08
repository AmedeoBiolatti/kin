#version 450 core
// kin's 3D shadow pass for cut-out materials (Blend3D::Mask): depth only,
// where the base colour's alpha reaches the cutoff, as mesh.frag keeps it.
layout(location = 0) in vec2 vUV;
layout(location = 1) in float vAlpha;

#include "kin_cutout.inc.glsl"

void main() {
    if (vAlpha * texture(uBase, vUV).a < material.cutoff.x) {
        discard;
    }
}
