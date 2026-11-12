#version 450 core
layout(location = 0) out vec4 fColor;
layout(set = 2, binding = 0) uniform sampler2DArray sDepth;
layout(set = 3, binding = 0) uniform Source { ivec4 region; } source;
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy) + source.region.xy;
    uint bits = floatBitsToUint(texelFetch(sDepth, ivec3(p, source.region.z), 0).r) >> uint(source.region.w);
    fColor = vec4(vec3(uvec3(bits, bits >> 8, bits >> 16) & 255u) / 255.0, 1.0);
}
