#version 450 core
// kin's 3D pick pass: each pixel's world position and the pick id (plus one)
// of the copy drawn there, nearest the camera; 0 where nothing is.
layout(location = 3) in vec3 vWorld;
layout(location = 4) flat in uint vId;
layout(location = 0) out vec4 FragColor;

void main() {
    FragColor = vec4(vWorld, uintBitsToFloat(vId));
}
