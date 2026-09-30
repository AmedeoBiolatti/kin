#version 450 core
// Test material for renderer_tests: reads one texel from each data texture format
// and a uniform past the first 16 floats, so each shows in the output colour.
layout(location = 0) out vec4 fColor;
layout(set = 2, binding = 0) uniform usampler2D sR16;
layout(set = 2, binding = 1) uniform usampler2D sRg16;
layout(set = 2, binding = 2) uniform sampler2D sR32f;
layout(set = 3, binding = 0) uniform Params {
    vec4 u[8];
};
layout(location = 0) in struct { vec4 Color; vec2 UV; } In;

void main() {
    uint r = texelFetch(sR16, ivec2(0, 0), 0).r;
    uint g = texelFetch(sRg16, ivec2(1, 0), 0).g;
    float b = texelFetch(sR32f, ivec2(0, 0), 0).r + u[5].y; // u[5].y is float 21
    fColor = vec4(float(r) / 65535.0, float(g) / 65535.0, b, 1.0);
}
