#version 450 core
layout(location = 0) out vec4 fColor;
layout(set = 2, binding = 0) uniform sampler2D sDepth;
layout(set = 2, binding = 1) uniform sampler2D sNormals;
layout(set = 3, binding = 0) uniform Params { vec4 options; vec4 b; vec4 c; vec4 d; };
layout(location = 0) in struct { vec4 Color; vec2 UV; } In;
void main() {
    ivec2 size = textureSize(sDepth, 0);
    ivec2 p = clamp(ivec2(In.UV * vec2(size)), ivec2(0), size - 1);
    float depth = texelFetch(sDepth, p, 0).r;
    vec2 e = texelFetch(sNormals, p, 0).rg;
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    n.xy += vec2(n.x >= 0.0 ? -1.0 : 1.0, n.y >= 0.0 ? -1.0 : 1.0) * max(-n.z, 0.0);
    n = normalize(n);
    vec3 value = options.x < 0.5 ? vec3(depth > 0.0) : options.x < 1.5 ? vec3(depth * options.y) : 0.5 + 0.5 * n;
    fColor = vec4(depth > 0.0 ? value : vec3(0.0), 1.0);
}
