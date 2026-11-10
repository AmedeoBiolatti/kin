#version 450 core
layout(location = 1) in vec2 vUV;
layout(set = 2, binding = 0) uniform sampler2D uImage;
layout(location = 0) out vec4 FragColor;
void main() { FragColor = texture(uImage, vUV); }
