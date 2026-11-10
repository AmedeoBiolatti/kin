#version 450 core
// prepass_velocity_skinned.vert for a skinned mesh with morph targets: the previous position is the one drawn the frame
// before, posed by the joints of the frame before.
#define KIN_MORPH_PREVIOUS
#include "kin_prepass_velocity_skinned.inc.glsl"
