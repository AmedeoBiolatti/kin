layout(set = 3, binding = 0) uniform ParticleFrag {
    vec4 tint;      // rgb: the light's tint (linear), w: lighting/shadow revision changed (reactive pass)
    vec4 flags;     // x: encode to sRGB, y: decode the texture, z: blend (0 alpha, 1 additive, 2 premultiplied), w: 1 with a texture
    vec4 disc;      // x: the procedural disc's softness, y: its ring, z: the soft fade's distance (0: none)
    vec4 unproject; // the inverse projection's entries e f g h: a depth's view-space z = (e d + f) / (g d + h)
} frag;
