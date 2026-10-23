#version 450 core
// mesh.frag with spot lights and the shadows of lights that are not the sun (P3b): the same shading, a spot loop, shadow maps for
// spots, point lights and the moon, and the sun's shadow strength (the block and samplers are in kin_material.inc.glsl).
#define KIN_LIGHTS
#include "kin_mesh_fragment.inc.glsl"
