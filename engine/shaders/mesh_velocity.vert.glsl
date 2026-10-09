#version 450 core
// mesh.vert, and where the point was on the screen last frame (temporal anti-aliasing with no pre-pass: the main
// pass writes the velocity itself).
#define KIN_VELOCITY
#include "kin_mesh_vertex.inc.glsl"
