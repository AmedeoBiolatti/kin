#version 450 core
// Dense temporal history back to display resolution, in linear light. Optional display-pixel sharpening shares
// this dispatch, using the same compressed-colour contrast bound and depth-edge suppression as native TAA.
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D uHistory;
layout(set = 0, binding = 1) uniform usampler2D uDilated;
layout(set = 1, binding = 0, rgba16f) uniform writeonly image2D uOutput;
layout(set = 2, binding = 0) uniform Params {
    vec4 size; // output extent and reciprocals
    vec4 sharpen; // strength, whether depth-edge metadata is valid
} params;
vec3 compress(vec3 c) { return c / (1.0 + max(c.r, max(c.g, c.b))); }
vec3 expand(vec3 c) { return c / max(1.0 - max(c.r, max(c.g, c.b)), 1e-4); }
vec4 at_pixel(ivec2 p) {
    const vec2 uv = (vec2(clamp(p, ivec2(0), ivec2(params.size.xy) - 1)) + .5) * params.size.zw;
    return textureLod(uHistory, uv, 0.0);
}
void main() {
    const ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(p, ivec2(params.size.xy)))) return;
    const vec4 centre = at_pixel(p);
    if (params.sharpen.x <= 0.0) {
        imageStore(uOutput, p, centre);
        return;
    }
    const vec3 c = compress(centre.rgb);
    const vec3 n = compress(at_pixel(p + ivec2(0, -1)).rgb), s = compress(at_pixel(p + ivec2(0, 1)).rgb);
    const vec3 e = compress(at_pixel(p + ivec2(1, 0)).rgb), w = compress(at_pixel(p + ivec2(-1, 0)).rgb);
    float boost = params.sharpen.x * 1.5;
    if (params.sharpen.y > .5) {
        // Every history texel under the output pixel, including fractional-density/odd-size edges. A history
        // scale <= 2 touches at most three texels per axis. Preserve the most recent depth bend among them.
        const ivec2 extent = textureSize(uDilated, 0);
        const vec2 ratio = vec2(extent) * params.size.zw;
        const ivec2 first = ivec2(floor(vec2(p) * ratio));
        const ivec2 last = min(ivec2(ceil(vec2(p + 1) * ratio)) - 1, extent - 1);
        uint bent = 0u;
        for (int y = 0; y < 3 && first.y + y <= last.y; ++y)
            for (int x = 0; x < 3 && first.x + x <= last.x; ++x) {
                const uint packed = texelFetch(uDilated, first + ivec2(x, y), 0).r;
                bent = max(bent, (packed & 3u) | (((packed >> 16u) & 3u) << 2u));
            }
        boost *= clamp(1.0 - float(bent) / 8.0, 0.0, 1.0);
    }
    const vec3 low = min(c, min(min(n, s), min(e, w))), high = max(c, max(max(n, s), max(e, w)));
    const vec3 sharp = clamp(c + (c - (n + s + e + w) * .25) * boost, low, high);
    imageStore(uOutput, p, vec4(expand(sharp), centre.a));
}
