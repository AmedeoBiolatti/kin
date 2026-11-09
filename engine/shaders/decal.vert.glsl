#version 450 core
// A decal's box (Scene3D::draw_decal) for decal.frag: the 36 corners of the unit cube [-0.5, 0.5]^3 from the vertex
// index (no vertex buffer), placed by the decal's matrix. Its back faces are drawn (the camera can be inside the box),
// each pixel of the box once; the fragment shader finds the surface under the pixel and projects the decal onto it.
layout(set = 1, binding = 0) uniform Decal {
    mat4 view_projection; // the main pass's (jittered with temporal anti-aliasing, as the pre-pass's depth is)
    mat4 model;           // the box to the world
    mat4 to_decal;        // the world to the box
    mat4 to_world;        // the inverse of view_projection: a pixel and its depth to the world
    vec4 colour;          // linear, straight alpha
    vec4 surface;         // x: roughness, y: metallic, z: normal map strength (0: none), w: emission
    vec4 fade;            // x: normal fade, y: depth fade
} decal;

layout(location = 0) flat out mat4 dToDecal;
layout(location = 4) flat out mat4 dToWorld;
layout(location = 8) flat out vec4 dColour;
layout(location = 9) flat out vec4 dSurface;
layout(location = 10) flat out vec4 dFade;
// The box's axes in the world, unit: the picture's u (X), the way it is projected from (Y), its v (Z).
layout(location = 11) flat out vec3 dAxisX;
layout(location = 12) flat out vec3 dAxisY;
layout(location = 13) flat out vec3 dAxisZ;

const vec3 corners[8] = vec3[8](vec3(-0.5, -0.5, -0.5), vec3(0.5, -0.5, -0.5), vec3(0.5, 0.5, -0.5),
                                vec3(-0.5, 0.5, -0.5), vec3(-0.5, -0.5, 0.5), vec3(0.5, -0.5, 0.5),
                                vec3(0.5, 0.5, 0.5), vec3(-0.5, 0.5, 0.5));
// Counter-clockwise seen from outside.
const int faces[36] = int[36](4, 5, 6, 4, 6, 7,  // +Z
                              1, 0, 3, 1, 3, 2,  // -Z
                              5, 1, 2, 5, 2, 6,  // +X
                              0, 4, 7, 0, 7, 3,  // -X
                              7, 6, 2, 7, 2, 3,  // +Y
                              0, 1, 5, 0, 5, 4); // -Y

void main() {
    const vec3 p = corners[faces[gl_VertexIndex]];
    gl_Position = decal.view_projection * (decal.model * vec4(p, 1.0));
    dToDecal = decal.to_decal;
    dToWorld = decal.to_world;
    dColour = decal.colour;
    dSurface = decal.surface;
    dFade = decal.fade;
    dAxisX = normalize(mat3(decal.model)[0]);
    dAxisY = normalize(mat3(decal.model)[1]);
    dAxisZ = normalize(mat3(decal.model)[2]);
}
