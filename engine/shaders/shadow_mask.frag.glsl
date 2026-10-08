#version 450 core
// kin's 3D shadow pass for cut-out materials (Blend3D::Mask): depth only,
// where the base colour's alpha reaches the cutoff, as mesh.frag keeps it.
layout(location = 0) in vec2 vUV;
layout(location = 1) in float vAlpha;

layout(set = 2, binding = 0) uniform sampler2D uBase;
layout(set = 3, binding = 0) uniform Cutoff {
    vec4 cutoff; // x: the alpha below which nothing is drawn
} material;

void main() {
    if (vAlpha * texture(uBase, vUV).a < material.cutoff.x) {
        discard;
    }
}
