// The first half of kin's 3D mesh fragment shader (kin_mesh_fragment.inc.glsl): the inputs, the material and every
// kin_* function a surface function may call (kin_material.inc.glsl), and the velocity's outputs.
#define KIN_NO_CUSTOM_PARAMS
#include "kin_material.inc.glsl"

#ifdef KIN_VELOCITY
// Temporal anti-aliasing with no pre-pass: the velocity as a second output, in the last two of four floats as the
// pre-pass writes it (taa.comp reads them there). The pipeline masks the write for blended and wireframe draws.
layout(location = 5) in vec3 vCurrent;
layout(location = 6) in vec3 vPrevious;
layout(location = 7) flat in float vFresh;
layout(location = 8) in vec3 vOldInCurrent;
layout(location = 1) out vec4 FragVelocity;
#include "kin_velocity.inc.glsl"
#endif
