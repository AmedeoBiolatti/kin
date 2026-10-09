#version 450 core
// The velocity pre-pass for cut-out materials (Blend3D::Mask): the same,
// where the base colour's alpha reaches the cutoff, as mesh.frag keeps it.
layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUV;
layout(location = 2) in float vAlpha;
layout(location = 3) in vec3 vCurrent;
layout(location = 4) in vec3 vPrevious;
layout(location = 5) flat in float vFresh;
layout(location = 6) in vec3 vWorld;
layout(location = 0) out vec4 FragNormalVelocity;

#include "kin_cutout.inc.glsl"
#include "kin_prepass.inc.glsl"

void main() {
    if (vAlpha * texture(uBase, vUV).a < material.cutoff.x) {
        discard;
    }
    FragNormalVelocity = vec4(octahedral(kin_prepass_normal()), kin_velocity(vCurrent, vPrevious, vFresh));
}
