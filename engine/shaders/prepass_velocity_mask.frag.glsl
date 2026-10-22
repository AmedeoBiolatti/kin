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
layout(location = 7) in vec3 vOldInCurrent;
layout(location = 0) out vec4 FragNormalVelocity;

#include "kin_cutout.inc.glsl"
#include "kin_prepass.inc.glsl"

void main() {
    const float motion_footprint = 0.25 * length(fwidth(vCurrent.xy / vCurrent.z));
    vec2 normal = octahedral(kin_prepass_normal());
    if (vAlpha * texture(uBase, vUV).a < material.cutoff.x) {
        discard;
    }
    // Carry independent geometry motion in the normal's unused range. AO
    // decodes the offset; transparent coverage preserves G for the resolve.
    if (kin_independent_motion(vCurrent, vOldInCurrent) > motion_footprint)
        normal.y += 4.0;
    FragNormalVelocity = vec4(normal, kin_velocity(vCurrent, vPrevious, vFresh));
}
