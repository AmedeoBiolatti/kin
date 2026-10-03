#version 450 core
// kin_draw_bench: per-object data from an R32F texture, row vCustom.x, 64 reads.
layout(location = 0) out vec4 fColor;
layout(set = 2, binding = 0) uniform sampler2D sData;
layout(location = 2) in vec4 vCustom;

void main() {
    const int row = int(vCustom.x);
    float sum = 0.0;
    for (int i = 0; i < 64; ++i) {
        sum += texelFetch(sData, ivec2(i, row), 0).r;
    }
    fColor = vec4(sum / 64.0, 0.0, 0.0, 1.0);
}
