#version 450 core
// Only for proven nonnegative alpha with zero cutoff; keep all shading arithmetic.
#define KIN_OPAQUE_DEPTH
#define KIN_DRAW_MATERIALS
#define KIN_LIGHTS
#define KIN_GI
#define KIN_LIGHT_GRID
#include "kin_mesh_fragment.inc.glsl"
