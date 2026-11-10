#version 450 core
// mesh.frag with spot lights and the shadows of lights that are not the sun (P3b): the same shading, a spot loop, shadow maps for
// spots, point lights and the moon, and the sun's shadow strength (the block and samplers are in kin_material.inc.glsl);
// and global illumination's probes (KIN_GI, G5b), whose light takes the place of the sky's diffuse where they are on;
// and the light grid (KIN_LIGHT_GRID): every point and spot light in view, a fragment looping over its cell's only.
#define KIN_LIGHTS
#define KIN_GI
#define KIN_LIGHT_GRID
#include "kin_mesh_fragment.inc.glsl"
