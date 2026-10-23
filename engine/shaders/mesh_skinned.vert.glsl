#version 450 core
// mesh.vert for a skinned copy (Scene3D::draw_skinned): the same varyings, so mesh.frag and a material's own shader work
// unchanged, with the vertex moved by its joints first.
#define KIN_SKIN
#include "kin_mesh_vertex.inc.glsl"
