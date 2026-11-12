#version 450 core
// Exact D32F restore at the original atlas coordinates; never filtered.
layout(set = 2, binding = 0) uniform sampler2DArray sStatic;
layout(set = 3, binding = 0) uniform Source { ivec4 layer; } source;
void main() {
    gl_FragDepth = texelFetch(sStatic, ivec3(ivec2(gl_FragCoord.xy), source.layer.x), 0).r;
}
