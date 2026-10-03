#version 450 core
// Test material for renderer_tests: the colour is entry vCustom.x of a storage
// buffer (binding 1, after the one sampler), so binding, index and updates show.
layout(location = 0) out vec4 fColor;
layout(set = 2, binding = 0) uniform sampler2D sUnused;
layout(std430, set = 2, binding = 1) readonly buffer Items {
    vec4 items[];
};
layout(location = 0) in struct { vec4 Color; vec2 UV; } In;
layout(location = 2) in vec4 vCustom;

void main() {
    fColor = items[int(vCustom.x)] * texture(sUnused, In.UV);
}
