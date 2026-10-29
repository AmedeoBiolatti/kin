#version 450 core
// Public geometry pass data: reverse-Z depth and signed octahedral world normal.
// No stock colour render is involved. Normals provide the fill and crease edges.
layout(location = 0) out vec4 fColor;
layout(set = 2, binding = 0) uniform sampler2D sDepth;
layout(set = 2, binding = 1) uniform sampler2D sNormals;
layout(set = 3, binding = 0) uniform Params { vec4 settings; vec4 unused_b; vec4 unused_c; vec4 unused_d; };
layout(location = 0) in struct { vec4 Color; vec2 UV; } In;
vec3 normal_at(ivec2 p) {
    vec2 e = texelFetch(sNormals, p, 0).rg;
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    n.xy += vec2(n.x >= 0.0 ? -1.0 : 1.0, n.y >= 0.0 ? -1.0 : 1.0) * max(-n.z, 0.0);
    return normalize(n);
}
void main() {
    ivec2 size = textureSize(sDepth, 0);
    ivec2 p = clamp(ivec2(In.UV * vec2(size)), ivec2(0), size - 1);
    float d = texelFetch(sDepth, p, 0).r;
    vec3 background = vec3(0.035, 0.05, 0.075);
    if (d == 0.0) { fColor = vec4(background, 1.0); return; }
    vec3 n = normal_at(p);
    float edge = 0.0;
    const ivec2 offsets[4] = ivec2[4](ivec2(-1,0), ivec2(1,0), ivec2(0,-1), ivec2(0,1));
    int width = max(1, int(settings.x));
    for (int i = 0; i < 4; ++i) {
        ivec2 q = clamp(p + offsets[i] * width, ivec2(0), size - 1);
        float other = texelFetch(sDepth, q, 0).r;
        // Relative reverse-Z discontinuity plus an angular normal discontinuity.
        edge = max(edge, float(other == 0.0 || abs(other - d) > max(d, other) * settings.y));
        if (other > 0.0) edge = max(edge, float(dot(n, normal_at(q)) < settings.z));
    }
    float light = 0.3 + 0.7 * max(dot(n, normalize(vec3(0.4, 0.8, 0.6))), 0.0);
    vec3 fill = mix(vec3(0.15, 0.5, 0.7), 0.5 + 0.5 * n, 0.35) * light;
    fColor = vec4(mix(fill, vec3(0.005, 0.01, 0.02), edge), 1.0);
}
