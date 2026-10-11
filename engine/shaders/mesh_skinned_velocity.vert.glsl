#version 450 core
// mesh_velocity.vert for a skinned copy: where the vertex was on the screen last frame is its pose of the frame before
// (the joints' matrices of that frame, matched to the copy as its model matrix is) placed by the copy's matrix of that frame
// and seen by the last camera, so a moving pose has its own velocity and a still one under a moving camera the camera's.
#define KIN_SKIN
#define KIN_VELOCITY
#include "kin_mesh_vertex.inc.glsl"
