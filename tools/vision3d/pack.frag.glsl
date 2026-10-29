#version 450 core
layout(std430, set = 2, binding = 0) readonly buffer Counts { uint count[]; } result;
layout(location = 0) out vec4 FragColor;
// Four bytes/observer, one byte in each pixel's alpha. Alpha stays linear in gamma, linear and HDR targets;
// a cleared destination preserves it under premultiplied blending. Colour transforms cannot change these bytes.
void main() {
    const uint value = result.count[uint(gl_FragCoord.y)];
    const uint byte = (value >> (8u * uint(gl_FragCoord.x))) & 255u;
    FragColor = vec4(0.0, 0.0, 0.0, float(byte) / 255.0);
}
