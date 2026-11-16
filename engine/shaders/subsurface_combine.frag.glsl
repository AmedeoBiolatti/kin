#version 450 core
// Subsurface scattering (Material3D::subsurface): the spread light (subsurface_scatter.comp's) added into the main pass
// after its opaque meshes, before the sky, the fog's light, the overlay and blended meshes (which then cover or dim it
// as the rest). Additive; the target's alpha (coverage) left as it is. Where nothing scattered it adds nothing.
layout(location = 0) in vec2 vUV;
layout(set = 2, binding = 0) uniform sampler2D uScattered;
layout(location = 0) out vec4 FragColor;

void main() {
    FragColor = vec4(texelFetch(uScattered, ivec2(gl_FragCoord.xy), 0).rgb, 0.0);
}
