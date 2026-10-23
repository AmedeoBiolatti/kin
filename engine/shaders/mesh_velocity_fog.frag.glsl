#version 450 core
// mesh_velocity.frag with the fog applied (Scene3D::fog with temporal anti-aliasing and no pre-pass).
#define KIN_VELOCITY
#define KIN_FOG
#include "kin_mesh_fragment.inc.glsl"
