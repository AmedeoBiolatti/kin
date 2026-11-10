#version 450 core
layout(location = 0) out vec4 fColor;
layout(set = 2, binding = 0) uniform sampler2DArray sDepth;
layout(set = 3, binding = 0) uniform Params { vec4 options; vec4 b; vec4 c; vec4 d; };
layout(location = 0) in struct { vec4 Color; vec2 UV; } In;
void main() {
    ivec2 size = textureSize(sDepth, 0).xy;
    ivec2 p = clamp(ivec2(In.UV * vec2(size)), ivec2(0), size - 1);
    float depth = texelFetch(sDepth, ivec3(p, int(options.x)), 0).r;
    fColor = vec4(vec3(depth > 0.0), 1.0);
}
