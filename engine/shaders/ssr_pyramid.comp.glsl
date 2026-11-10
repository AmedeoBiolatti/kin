#version 450 core
// The picture the next frame's screen-space reflections read (Scene3D::ssr, kin_ssr_reflection): the frame's lit
// picture at half the view's size, then each level half the one before, in the reflections' buffer after the trace's
// texels (two words a texel: red and green, blue and 1, as halves). Level 0 from the picture (linear; sRGB-decoded where
// the view's output is encoded), each level after from the one before (the four texels under it, averaged). The main
// pass reads a level by how wide a surface's reflection lobe is where it meets what it reflects: a rough or far
// reflection reads a blurred one.
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D uSource; // the frame's picture (level 0 only)
layout(std430, set = 1, binding = 0) buffer Pyramid {
    uint words[];
};
layout(set = 2, binding = 0) uniform Params {
    vec4 size;  // xy: this level's size, zw: the level before's
    vec4 where; // x: this level's first word, y: the level before's, z: 1 for level 0 (from the picture), w: 1 to decode
    vec4 share; // xy: the part of the picture the render filled (View3D::set_render_size), from its top-left; z: 1 when
                // it is compressed (ViewOptions3D::compress: x / (1 + x)), taken back
} params;

vec3 decode_srgb(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}

vec3 before(ivec2 q) {
    const ivec2 size = ivec2(params.size.zw);
    q = clamp(q, ivec2(0), size - 1);
    const uint at = uint(params.where.y) + uint(q.y * size.x + q.x) * 2u;
    return vec3(unpackHalf2x16(words[at]), unpackHalf2x16(words[at + 1u]).x);
}

void main() {
    const ivec2 xy = ivec2(gl_GlobalInvocationID.xy);
    const ivec2 size = ivec2(params.size.xy);
    if (xy.x >= size.x || xy.y >= size.y) {
        return;
    }
    vec3 c;
    if (params.where.z > 0.5) {
        // Bilinear at the texel's centre: the picture's 2 x 2 under it, averaged.
        c = textureLod(uSource, (vec2(xy) + 0.5) / vec2(size) * params.share.xy, 0.0).rgb;
        if (params.where.w > 0.5)
            c = decode_srgb(c);
        if (params.share.z > 0.5)
            c = c / max(vec3(1.0) - c, vec3(1.0 / 64.0));
    } else {
        c = 0.25 * (before(xy * 2) + before(xy * 2 + ivec2(1, 0)) + before(xy * 2 + ivec2(0, 1)) +
                    before(xy * 2 + ivec2(1, 1)));
    }
    c = any(isnan(c)) || any(isinf(c)) ? vec3(0.0) : clamp(c, vec3(0.0), vec3(64.0));
    const uint at = uint(params.where.x) + uint(xy.y * size.x + xy.x) * 2u;
    words[at] = packHalf2x16(c.rg);
    words[at + 1u] = packHalf2x16(vec2(c.b, 1.0));
}
