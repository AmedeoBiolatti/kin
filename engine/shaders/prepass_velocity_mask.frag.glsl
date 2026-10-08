#version 450 core
// The velocity pre-pass for cut-out materials (Blend3D::Mask): the same,
// where the base colour's alpha reaches the cutoff, as mesh.frag keeps it.
layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUV;
layout(location = 2) in float vAlpha;
layout(location = 3) in vec3 vCurrent;
layout(location = 4) in vec3 vPrevious;
layout(location = 5) flat in float vFresh;
layout(location = 0) out vec4 FragNormalVelocity;

layout(set = 2, binding = 0) uniform sampler2D uBase;
layout(set = 3, binding = 0) uniform Cutoff {
    vec4 cutoff; // x: the alpha below which nothing is drawn
} material;

vec2 octahedral(vec3 n) {
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    const vec2 fold = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return n.z >= 0.0 ? n.xy : fold;
}

vec2 velocity() {
    if (vFresh > 0.5 || vPrevious.z <= 1e-5 || vCurrent.z <= 1e-5) {
        return vec2(8.0);
    }
    return (vCurrent.xy / vCurrent.z - vPrevious.xy / vPrevious.z) * vec2(0.5, -0.5);
}

void main() {
    if (vAlpha * texture(uBase, vUV).a < material.cutoff.x) {
        discard;
    }
    FragNormalVelocity = vec4(octahedral(normalize(vNormal) * (gl_FrontFacing ? 1.0 : -1.0)), velocity());
}
