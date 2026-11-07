#version 450 core
// A surface material's base colour over its UV square, for its stand-in in the GI scene (gpu_gi3d.cpp,
// bake_gi_proxy): the material drawn unlit (its base colour plus its emission) less the same drawn lit by nothing (its
// emission alone), a texel each; its alpha the unlit one's (a cut-out's clear texels none).
#include "kin_srgb.inc.glsl"

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

layout(set = 2, binding = 0) uniform sampler2D uUnlit;
layout(set = 2, binding = 1) uniform sampler2D uEmission;

layout(std140, set = 3, binding = 0) uniform Proxy {
    vec4 flags; // x: 1 to write it encoded (materials' maps hold sRGB bytes, decoded by the shaders)
};

void main() {
    const ivec2 at = ivec2(gl_FragCoord.xy);
    const vec4 unlit = texelFetch(uUnlit, at, 0);
    const vec3 base = clamp(unlit.rgb - texelFetch(uEmission, at, 0).rgb, 0.0, 1.0);
    FragColor = vec4(flags.x > 0.5 ? encode(base) : base, clamp(unlit.a, 0.0, 1.0));
}
