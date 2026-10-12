// kin's 3D particle fragment shader (P3c), shared by particle.frag and particle_soft.frag (which defines KIN_SOFT first): the soft one also reads the view's depth
// (the pre-pass's, one sample a pixel) and fades the particle where it is near the surface behind it.
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

layout(set = 2, binding = 0) uniform sampler2D uTexture; // white without one
#ifdef KIN_SOFT
layout(set = 2, binding = 1) uniform sampler2D uDepth;   // the pre-pass's depth (reverse Z), read by texel
#endif

layout(set = 3, binding = 0) uniform ParticleFrag {
    vec4 tint;      // rgb: the light's tint (linear), w: unused
    vec4 flags;     // x: encode to sRGB, y: decode the texture, z: blend (0 alpha, 1 additive, 2 premultiplied), w: 1 with a texture
    vec4 disc;      // x: the procedural disc's softness, y: its ring, z: the soft fade's distance (0: none)
    vec4 unproject; // the inverse projection's entries e f g h: a depth's view-space z = (e d + f) / (g d + h)
} frag;

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
    // The picture, or a soft disc (with a ring, if asked): alpha 0 outside the disc's edge.
    vec4 tex = vec4(1.0);
    float shape = 1.0;
    if (frag.flags.w > 0.5) {
        tex = texture(uTexture, vUV0);
        if (vMix > 0.0) {
            tex = mix(tex, texture(uTexture, vUV1), vMix);
        }
        if (frag.flags.y > 0.5) {
            tex.rgb = decode(tex.rgb);
        }
    } else {
        const float r = length(vLocal);
        const float s = clamp(frag.disc.x, 0.0, 1.0);
        const float inner = 1.0 - s;
        float a = r <= inner ? 1.0 : 1.0 - smooth01((r - inner) / max(s, 1e-4));
        if (r > 1.0) {
            a = 0.0;
        }
        if (frag.disc.y > 0.0) {
            const float width = clamp(frag.disc.y, 0.01, 1.0);
            a *= smooth01((r - (1.0 - width)) / width);
        }
        shape = a;
    }
    float fade = 1.0;
#ifdef KIN_SOFT
    if (frag.disc.z > 0.0) {
        // The surface behind, along the camera's forward, against this fragment's: 0 where they meet. Far (depth 0): nothing.
        const float d = texelFetch(uDepth, ivec2(gl_FragCoord.xy), 0).r;
        if (d > 0.0) {
            const float scene = -(frag.unproject.x * d + frag.unproject.y) / (frag.unproject.z * d + frag.unproject.w);
            fade = smooth01((scene - vDepth) / frag.disc.z);
        }
    }
#endif
    const float k = vColour.a * shape * fade;
    vec3 colour = tex.rgb * vColour.rgb * frag.tint.rgb;
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
    // The fog: a surface's colour mixed toward the fog's by its opacity (the fog's colour is itself premultiplied by this
    // particle's alpha: a faint particle adds a faint fog); an additive one is dimmed (what is behind has the fog already).
    if (vFog.a > 0.0) {
        if (frag.flags.z > 0.5 && frag.flags.z < 1.5) {
            colour *= 1.0 - vFog.a;
        } else {
            colour = mix(colour, vFog.rgb * alpha, vFog.a);
        }
    }
    if (frag.flags.x > 0.5) {
        // Encoded straight, as the meshes are: encode(colour) times alpha. Premultiplied colour is taken back to straight.
        if (alpha > 1e-4) {
            colour = encode(colour / alpha) * alpha;
        } else {
            colour = encode(colour);
        }
    }
    FragColor = vec4(colour, frag.flags.z > 0.5 && frag.flags.z < 1.5 ? 0.0 : alpha);
}
