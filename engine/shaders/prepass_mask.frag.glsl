#version 450 core
// The pre-pass for cut-out materials (Blend3D::Mask): prepass.frag's normal,
// where the base colour's alpha reaches the cutoff, as mesh.frag keeps it.
layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUV;
layout(location = 2) in float vAlpha;
layout(location = 0) out vec2 FragNormal;

layout(set = 2, binding = 0) uniform sampler2D uBase;
layout(set = 3, binding = 0) uniform Cutoff {
    vec4 cutoff; // x: the alpha below which nothing is drawn
} material;

vec2 octahedral(vec3 n) {
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    const vec2 fold = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return n.z >= 0.0 ? n.xy : fold;
}

void main() {
    if (vAlpha * texture(uBase, vUV).a < material.cutoff.x) {
        discard;
    }
    FragNormal = octahedral(normalize(vNormal) * (gl_FrontFacing ? 1.0 : -1.0));
}
