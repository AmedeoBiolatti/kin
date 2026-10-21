#ifdef KIN_REACTIVE
#include "kin_velocity.inc.glsl"
layout(location = 10) in vec3 vCurrent;
layout(location = 11) in vec3 vPrevious;
layout(location = 12) flat in float vFresh;
#endif
// kin's 3D particle fragment shader (P3c), shared by particle.frag and particle_soft.frag (which defines KIN_SOFT
// first): the soft one also reads the view's depth (the pre-pass's, one sample a pixel) and fades the particle where it
// is near the surface behind it.
//
// The output is premultiplied, as kin's targets are: Alpha mixes the (fogged) colour by its alpha, Additive adds colour
// times alpha and is dimmed, not mixed, by the fog, Premultiplied takes a texture that already holds colour times alpha
// (alpha 0 with colour is an addition inside the sprite). The same arithmetic runs on the CPU in the tests.
layout(location = 0) in vec2 vUV0;
layout(location = 1) in vec2 vUV1;
layout(location = 2) in vec4 vColour;
layout(location = 3) in vec2 vLocal;
layout(location = 4) in float vDepth;
layout(location = 5) in vec4 vFog;
layout(location = 6) in float vMix;
#ifdef KIN_PARTICLE_LIGHTS
layout(location = 7) in vec3 vLocalLight;
#endif

layout(set = 2, binding = 0) uniform sampler2D uTexture; // white without one
#ifdef KIN_SOFT
layout(set = 2, binding = 1) uniform sampler2D uDepth; // the pre-pass's depth (reverse Z), read by texel
#endif

#include "kin_particle_frag_uniforms.inc.glsl"

#ifdef KIN_PARTICLE_SHADOWS
#include "kin_particle_shadow.inc.glsl"
#endif

layout(location = 0) out vec4 FragColor;

vec3 decode(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}

vec3 encode(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}

float smooth01(float x) {
    x = clamp(x, 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x);
}

void main() {
#include "kin_particle_sample.inc.glsl"
    float fade = 1.0;
#ifdef KIN_SOFT
    if (frag.disc.z > 0.0) {
        // The surface behind, along the camera's forward, against this fragment's: 0 where they meet. Far (depth 0):
        // nothing.
        const float d = texelFetch(uDepth, ivec2(gl_FragCoord.xy), 0).r;
        if (d > 0.0) {
            const float scene = -(frag.unproject.x * d + frag.unproject.y) / (frag.unproject.z * d + frag.unproject.w);
            fade = smooth01((scene - vDepth) / frag.disc.z);
        }
    }
#endif
    const float k = vColour.a * shape * fade;
#ifdef KIN_PARTICLE_SHADOWS
    const vec3 shadow_tint = particle_shadow_tint();
    vec3 colour = tex.rgb * vColour.rgb * shadow_tint;
#elif defined(KIN_PARTICLE_LIGHTS)
    vec3 colour = tex.rgb * vColour.rgb * (frag.tint.rgb + vLocalLight);
#else
    vec3 colour = tex.rgb * vColour.rgb * frag.tint.rgb;
#endif
    float alpha;
    if (frag.flags.z > 1.5) {
        // Premultiplied: the texel is colour * alpha already; the particle's alpha scales both.
        colour *= k;
        alpha = tex.a * k;
    } else if (frag.flags.z > 0.5) {
        alpha = tex.a * k;
        colour *= alpha; // additive: no mixing with what is behind
    } else {
        alpha = tex.a * k;
        colour *= alpha;
    }
    // The fog: a surface's colour mixed toward the fog's by its opacity (the fog's colour is itself premultiplied by
    // this particle's alpha: a faint particle adds a faint fog); an additive one is dimmed (what is behind has the fog
    // already).
    if (vFog.a > 0.0) {
        if (frag.flags.z > 0.5 && frag.flags.z < 1.5) {
            colour *= 1.0 - vFog.a;
        } else {
            colour = mix(colour, vFog.rgb * alpha, vFog.a);
        }
    }
    if (frag.flags.x > 0.5) {
        // Encoded straight, as the meshes are: encode(colour) times alpha. Premultiplied colour is taken back to
        // straight.
        if (alpha > 1e-4) {
            colour = encode(colour / alpha) * alpha;
        } else {
            colour = encode(colour);
        }
    }
    FragColor = vec4(colour, frag.flags.z > 0.5 && frag.flags.z < 1.5 ? 0.0 : alpha);
#ifdef KIN_REACTIVE
    // Match actual contribution, including alpha-zero premultiplied emission,
    // fog attenuation, the atlas/disc and soft intersections. Invisible sprite
    // corners must not erase another sprite's rejection mark.
    if (all(equal(FragColor, vec4(0.0))))
        discard;
    float coverage = vFresh > .5 ? 6.0 : 4.0;
    float signature = 0.0;
#ifdef KIN_PARTICLE_SHADOWS
    if (vFresh < .5)
        coverage = frag.tint.w > .5 ? 8.0 : 4.0;
    const float energy = dot(shadow_tint, vec3(.2126, .7152, .0722));
    signature = energy / (1.0 + energy);
#elif defined(KIN_PARTICLE_LIGHTS)
    if (vFresh < .5)
        coverage = frag.tint.w > .5 ? 8.0 : 4.0;
    const float energy = dot(frag.tint.rgb + vLocalLight, vec3(.2126, .7152, .0722));
    signature = energy / (1.0 + energy);
#endif
    FragColor = vec4(coverage + signature, 0.0, kin_velocity(vCurrent, vPrevious, vFresh));
#endif
}
