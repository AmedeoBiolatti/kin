#version 450 core
// mesh_velocity.vert for a skinned copy: the velocity the main pass writes is the camera's alone (the copy's past pose is
// not kept yet), so a character standing still under a moving camera is right and one that moves is not, until the next step.
#define KIN_SKIN
#define KIN_VELOCITY
#include "kin_mesh_vertex.inc.glsl"
