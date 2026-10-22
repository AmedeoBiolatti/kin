#version 450 core
// The pre-pass with velocity (temporal anti-aliasing): the world normal as
// prepass.frag has it, octahedral, in R and G; in B and A how far this pixel's
// point has moved on the screen since the last frame, in units of the picture
// (0 to 1 across), so that the history is at uv - velocity. A point with no
// past (a new copy, or one behind the camera last frame) gets a velocity far
// off the screen, which the resolve reads as "no history here".
layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUV;
layout(location = 2) in float vAlpha;
layout(location = 3) in vec3 vCurrent;
layout(location = 4) in vec3 vPrevious;
layout(location = 5) flat in float vFresh;
layout(location = 6) in vec3 vWorld;
layout(location = 7) in vec3 vOldInCurrent;
layout(location = 0) out vec4 FragNormalVelocity;

#include "kin_prepass.inc.glsl"

void main() {
    vec2 normal = octahedral(kin_prepass_normal());
    // Carry independent geometry motion in the normal's unused range. AO
    // decodes the offset; transparent coverage preserves G for the resolve.
    if (kin_independent_motion(vCurrent, vOldInCurrent) > 0.25 * length(fwidth(vCurrent.xy / vCurrent.z)))
        normal.y += 4.0;
    FragNormalVelocity = vec4(normal, kin_velocity(vCurrent, vPrevious, vFresh));
}
