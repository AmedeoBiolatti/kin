#version 450 core
// kin GPU backend default 2D fragment shader (ported from v0 renderer_gpu).
// Per-vertex color * sampled texture; solids bind a 1x1 white texture so the
// output is just the vertex color (mirrors the SDL backend's geometry path).
layout(location = 0) out vec4 fColor;

layout(set = 2, binding = 0) uniform sampler2D sTexture;

layout(location = 0) in struct {
    vec4 Color;
    vec2 UV;
} In;

void main() {
    fColor = In.Color * texture(sTexture, In.UV.st);
}
