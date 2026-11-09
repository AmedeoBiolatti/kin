#version 450 core
// prepass_velocity.vert for a mesh with morph targets: the previous position is the one drawn the frame before.
#define KIN_MORPH_PREVIOUS
#include "kin_prepass_velocity_vertex.inc.glsl"
