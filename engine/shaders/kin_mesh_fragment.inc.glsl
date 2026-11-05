// kin's 3D mesh fragment shader, shared by mesh.frag and mesh_velocity.frag (which defines KIN_VELOCITY first).
// kin's 3D meshes (kin/render3d): a base colour lit in linear light by the
// scene, plus the material's own light; or unlit. The shading is in
// kin_material.inc.glsl, which custom material shaders share.
//
// In two halves, so that a surface shader's variants (docs/material_graph_plan.md) put its surface function between
// them: what the function may call, then main(), which calls it where KIN_SURFACE names it.
#include "kin_mesh_surface.inc.glsl"
#include "kin_mesh_main.inc.glsl"
