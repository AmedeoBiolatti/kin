#version 450 core
// kin's 3D pick pass for cut-out materials (Blend3D::Mask): pick.frag, with
// the pixels mesh.frag does not draw left out.
layout(location = 1) in vec4 vColor;
layout(location = 2) in vec2 vUV;
layout(location = 3) in vec3 vWorld;
layout(location = 4) flat in uint vId;
layout(location = 0) out vec4 FragColor;

layout(set = 2, binding = 0) uniform sampler2D uBase;
layout(set = 3, binding = 0) uniform Cutoff {
    vec4 cutoff; // x: the alpha below which nothing is drawn
} material;

void main() {
    if (vColor.a * texture(uBase, vUV).a < material.cutoff.x) {
        discard;
    }
    FragColor = vec4(vWorld, uintBitsToFloat(vId));
}
