#version 450 core
// mesh_light.frag for a scene with baked lighting (Scene3D::baked_lighting): the diffuse light from a lightmapped copy's
// lightmap (with mesh_lightmap.vert), else from the baked probes, in place of the GI's (whose samplers' places they take).
#define KIN_LIGHTS
#define KIN_GI
#define KIN_LIGHT_GRID
#define KIN_BAKED
#include "kin_mesh_fragment.inc.glsl"
