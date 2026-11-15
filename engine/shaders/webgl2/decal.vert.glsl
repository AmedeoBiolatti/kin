#version 300 es
// A decal's box (Scene3D::draw_decal) for mesh.frag's KIN_DECAL variant: the 36 corners of the unit cube from the vertex
// index, placed by the decal's matrix (native decal.vert's, a decal a draw). Its back faces are drawn: each pixel the box
// covers once, wherever the camera is; the fragment finds the surface under the pixel from the view's depth.
precision highp float;
uniform mat4 uViewProjection;
uniform mat4 uModel;
const vec3 corners[8] = vec3[8](vec3(-0.5, -0.5, -0.5), vec3(0.5, -0.5, -0.5), vec3(0.5, 0.5, -0.5),
                                vec3(-0.5, 0.5, -0.5), vec3(-0.5, -0.5, 0.5), vec3(0.5, -0.5, 0.5),
                                vec3(0.5, 0.5, 0.5), vec3(-0.5, 0.5, 0.5));
// Counter-clockwise seen from outside.
const int faces[36] = int[36](4, 5, 6, 4, 6, 7, 1, 0, 3, 1, 3, 2, 5, 1, 2, 5, 2, 6, 0, 4, 7, 0, 7, 3, 7, 6, 2, 7, 2, 3,
                              0, 1, 5, 0, 5, 4);
void main() {
    gl_Position = uViewProjection * (uModel * vec4(corners[faces[gl_VertexID]], 1.0));
    gl_Position.z = 2.0 * gl_Position.z - gl_Position.w;
}
