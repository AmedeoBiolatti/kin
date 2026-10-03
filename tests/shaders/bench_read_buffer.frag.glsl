#version 450 core
// kin_draw_bench: per-object data from a storage buffer, entries 64 x vCustom.x on.
layout(location = 0) out vec4 fColor;
layout(set = 2, binding = 0) uniform sampler2D sUnused;
layout(std430, set = 2, binding = 1) readonly buffer Data {
    float values[];
};
layout(location = 2) in vec4 vCustom;

void main() {
    const int first = int(vCustom.x) * 64;
    float sum = 0.0;
    for (int i = 0; i < 64; ++i) {
        sum += values[first + i];
    }
    fColor = vec4(sum / 64.0, 0.0, 0.0, 1.0);
}
