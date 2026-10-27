#version 450 core
// mesh_velocity.frag with spot lights and local shadows (P3b; temporal anti-aliasing with no pre-pass).
#define KIN_VELOCITY
#define KIN_LIGHTS
#define KIN_GI
#define KIN_LIGHT_GRID
#include "kin_mesh_fragment.inc.glsl"
