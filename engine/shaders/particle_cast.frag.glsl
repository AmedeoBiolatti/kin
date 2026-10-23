#version 450 core
layout(location = 0) in vec2 vUV0;
layout(location = 1) in vec2 vUV1;
layout(location = 2) in vec4 vColour;
layout(location = 3) in vec2 vLocal;
layout(location = 6) in float vMix;
layout(set = 2, binding = 0) uniform sampler2D uTexture;
#include "kin_particle_frag_uniforms.inc.glsl"
float smooth01(float x) {
    x = clamp(x, 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x);
}
// The sampler include's RGB decode is irrelevant to shadow coverage.
vec3 decode(vec3 c) { return c; }
void main() {
#include "kin_particle_sample.inc.glsl"
    const float alpha = tex.a * vColour.a * shape;
    if (alpha <= 0.0 || alpha < frag.disc.w) discard;
}
