#version 300 es
// Decals' boxes (Scene3D::draw_decal) for mesh.frag's KIN_DECAL variant: the 36 corners of the unit cube from the vertex
// index, each instance a decal placed by its own matrix (native decal.vert's; its storage buffer's records here are
// instanced attributes). A run of decals sharing their pictures is one draw. Their back faces are drawn: each pixel a
// box covers once, wherever the camera is; the fragment finds the surface under the pixel from the view's depth.
precision highp float;
layout(location = 0) in mat4 aModel;    // the box to the world
layout(location = 4) in mat4 aToDecal;  // the world to the box
layout(location = 8) in vec4 aColour;   // linear, straight alpha
layout(location = 9) in vec4 aSurface;  // x roughness, y metallic, z normal map strength (0: none), w emission
layout(location = 10) in vec4 aFade;    // x normal fade, y depth fade
uniform mat4 uViewProjection;
flat out mat4 dToDecal;
flat out vec4 dColour, dSurface, dFade;
// The box's axes in the world, unit: the picture's u (X), the way it is projected from (Y), its v (Z).
flat out vec3 dAxisX, dAxisY, dAxisZ;
const vec3 corners[8] = vec3[8](vec3(-0.5, -0.5, -0.5), vec3(0.5, -0.5, -0.5), vec3(0.5, 0.5, -0.5),
                                vec3(-0.5, 0.5, -0.5), vec3(-0.5, -0.5, 0.5), vec3(0.5, -0.5, 0.5),
                                vec3(0.5, 0.5, 0.5), vec3(-0.5, 0.5, 0.5));
// Counter-clockwise seen from outside.
const int faces[36] = int[36](4, 5, 6, 4, 6, 7, 1, 0, 3, 1, 3, 2, 5, 1, 2, 5, 2, 6, 0, 4, 7, 0, 7, 3, 7, 6, 2, 7, 2, 3,
                              0, 1, 5, 0, 5, 4);
void main() {
    gl_Position = uViewProjection * (aModel * vec4(corners[faces[gl_VertexID]], 1.0));
    gl_Position.z = 2.0 * gl_Position.z - gl_Position.w;
    dToDecal = aToDecal;
    dColour = aColour;
    dSurface = aSurface;
    dFade = aFade;
    dAxisX = normalize(aModel[0].xyz);
    dAxisY = normalize(aModel[1].xyz);
    dAxisZ = normalize(aModel[2].xyz);
}
