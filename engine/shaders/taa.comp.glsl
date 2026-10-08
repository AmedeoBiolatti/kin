#version 450 core
// kin's temporal anti-aliasing (Scene3D::taa): this frame's picture, drawn a
// fraction of a pixel off (the jitter), blended into the picture of the frames
// before. Each pixel looks for where its point was last frame (the velocity
// the pre-pass wrote), reads the history there with a Catmull-Rom filter,
// clips it to the colours near the pixel in this frame (a 3 x 3 box in YCoCg,
// from the mean and spread, inside the neighbours' own range: history that
// does not agree with what is there now is a ghost) and mixes. The history
// weight falls to nothing where there is none (off the screen, a new mesh,
// the first frame), where the surface the history shows moved differently
// from this one (the velocity the last frame had at that spot is compared
// with this frame's: something that left a spot uncovered, or arrived on it),
// and a little with speed and with how far the clip moved it.
// Light is compressed (c / (1 + max c)) for the clip and the mix, so a very
// bright pixel does not outweigh its neighbours or flicker. A second mode
// (params.z) sharpens a finished picture: a small local-contrast boost kept
// inside the neighbours' range.
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D uCurrent; // this frame, or in sharpen mode the result so far; linear, premultiplied
layout(set = 0, binding = 1) uniform sampler2D uHistory;
layout(set = 0, binding = 2) uniform sampler2D uMotion;  // B, A: velocity in picture units (the pre-pass's)
layout(set = 0, binding = 3) uniform sampler2D uPastMotion; // the same, of the last frame
layout(set = 1, binding = 0, rgba16f) uniform writeonly image2D uResult;
layout(set = 2, binding = 0) uniform Params {
    vec4 sizes;  // the picture's width and height, and 1 over them
    vec4 params; // x: history weight, y: 1 when there is no history, z: 1 to sharpen instead, w: sharpness
    vec4 jitter; // xy: this frame's offset in pixels (the picture is drawn that far right and down), zw: 0
} params;

vec3 compress(vec3 c) {
    return c / (1.0 + max(c.r, max(c.g, c.b)));
}

vec3 expand(vec3 c) {
    return c / max(1.0 - max(c.r, max(c.g, c.b)), 1e-4);
}

vec3 to_ycocg(vec3 c) {
    return vec3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}

vec3 from_ycocg(vec3 c) {
    return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

vec4 fetch(ivec2 p) {
    const ivec2 size = ivec2(params.sizes.xy);
    return texelFetch(uCurrent, clamp(p, ivec2(0), size - 1), 0);
}

// Five bilinear taps that make a 4 x 4 Catmull-Rom (sharper than bilinear,
// which blurs a moving picture a little more each frame).
vec4 history_at(vec2 uv) {
    const vec2 pos = uv * params.sizes.xy;
    const vec2 centre = floor(pos - 0.5) + 0.5;
    const vec2 f = pos - centre;
    const vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    const vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    const vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    const vec2 w3 = f * f * (-0.5 + 0.5 * f);
    const vec2 w12 = w1 + w2;
    const vec2 t12 = (centre + w2 / w12) * params.sizes.zw;
    const vec2 t0 = (centre - 1.0) * params.sizes.zw;
    const vec2 t3 = (centre + 2.0) * params.sizes.zw;
    const vec4 sum = textureLod(uHistory, vec2(t12.x, t0.y), 0.0) * (w12.x * w0.y) +
                     textureLod(uHistory, vec2(t0.x, t12.y), 0.0) * (w0.x * w12.y) +
                     textureLod(uHistory, t12, 0.0) * (w12.x * w12.y) +
                     textureLod(uHistory, vec2(t3.x, t12.y), 0.0) * (w3.x * w12.y) +
                     textureLod(uHistory, vec2(t12.x, t3.y), 0.0) * (w12.x * w3.y);
    // The four corners are dropped; what is left is divided back up to one.
    const float total = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return max(sum / total, vec4(0.0));
}

void sharpen(ivec2 p) {
    const vec4 centre = fetch(p);
    const vec3 c = compress(centre.rgb);
    const vec3 n = compress(fetch(p + ivec2(0, -1)).rgb), s = compress(fetch(p + ivec2(0, 1)).rgb);
    const vec3 e = compress(fetch(p + ivec2(1, 0)).rgb), w = compress(fetch(p + ivec2(-1, 0)).rgb);
    const vec3 low = min(c, min(min(n, s), min(e, w))), high = max(c, max(max(n, s), max(e, w)));
    const vec3 blur = (n + s + e + w) * 0.25;
    const vec3 sharp = clamp(c + (c - blur) * (params.params.w * 1.5), low, high);
    imageStore(uResult, p, vec4(expand(sharp), centre.a));
}

void main() {
    const ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (p.x >= int(params.sizes.x) || p.y >= int(params.sizes.y)) {
        return;
    }
    if (params.params.z > 0.5) {
        sharpen(p);
        return;
    }
    const vec4 current = fetch(p);
    const vec2 velocity = texelFetch(uMotion, p, 0).ba;
    const vec2 uv = (vec2(p) + 0.5) * params.sizes.zw;
    const vec2 past = uv - velocity;
    if (params.params.y > 0.5 || any(lessThan(past, vec2(0.0))) || any(greaterThan(past, vec2(1.0))) || any(isnan(velocity))) {
        imageStore(uResult, p, current);
        return;
    }

    // The neighbourhood: mean and spread of the 3 x 3 around the pixel, and its range.
    vec3 sum = vec3(0.0), sum_squares = vec3(0.0), low = vec3(1e9), high = vec3(-1e9);
    float alpha_low = 1e9, alpha_high = -1e9;
    // This frame's picture at the pixel's own centre: each of the 3 x 3 is a sample taken a
    // little off the pixel grid (the jitter), so the pixel's value is read from them by how
    // near the centre each fell (a Gaussian, as Karis' filter for TAA), not taken as its
    // own sample, which sits anywhere in the pixel from frame to frame and would
    // pull the picture a fraction of a pixel to wherever the last few jitters were.
    // With no jitter there is nothing to correct: the pixel's own sample.
    const bool jittered = dot(params.jitter.xy, params.jitter.xy) > 0.0;
    vec3 filtered = vec3(0.0);
    float filtered_alpha = 0.0, filtered_weight = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            const vec4 c = fetch(p + ivec2(x, y));
            const vec3 compressed = compress(c.rgb);
            const vec2 away = vec2(x, y) - params.jitter.xy;
            const float near = jittered ? exp(-6.0 * dot(away, away)) : (x == 0 && y == 0 ? 1.0 : 0.0);
            filtered += compressed * near;
            filtered_alpha += c.a * near;
            filtered_weight += near;
            const vec3 q = to_ycocg(compressed);
            sum += q;
            sum_squares += q * q;
            low = min(low, q);
            high = max(high, q);
            alpha_low = min(alpha_low, c.a);
            alpha_high = max(alpha_high, c.a);
        }
    }
    const vec3 mean = sum / 9.0;
    const vec3 spread = sqrt(max(sum_squares / 9.0 - mean * mean, vec3(0.0)));
    const vec3 box_low = max(mean - 1.25 * spread, low), box_high = min(mean + 1.25 * spread, high);

    vec4 history = history_at(past);
    if (any(isnan(history)) || any(isinf(history))) {
        imageStore(uResult, p, current);
        return;
    }
    // Clipped toward the middle of the box along the line to the history, not
    // each channel on its own, which would shift its colour.
    vec3 h = to_ycocg(compress(history.rgb));
    const vec3 middle = 0.5 * (box_low + box_high), extent = 0.5 * (box_high - box_low) + 1e-4;
    const vec3 offset = h - middle;
    const vec3 ratio = abs(offset / extent);
    const float outside = max(ratio.x, max(ratio.y, ratio.z));
    if (outside > 1.0) {
        h = middle + offset / outside;
    }
    history.a = clamp(history.a, alpha_low, alpha_high);

    // The surface the history shows: the velocity the last frame had at the pixel it is
    // read from, against this pixel's. A different one means this pixel was covered
    // by something else then (or covers what was something else): the history is
    // of another surface and is dropped, over the half pixel past a tolerance that
    // grows a little with speed.
    const ivec2 size = ivec2(params.sizes.xy);
    const ivec2 nearest = clamp(ivec2(floor(past * params.sizes.xy)), ivec2(0), size - 1);
    const float mismatch_px = length((texelFetch(uPastMotion, nearest, 0).ba - velocity) * params.sizes.xy);
    const float speed = length(velocity * params.sizes.xy);
    float weight = params.params.x;
    weight *= 1.0 - clamp((mismatch_px - (0.35 + 0.05 * speed)) / 0.4, 0.0, 1.0);
    // A moving picture is read less exactly (every sub-pixel shift blurs it a
    // little), and a history that had to be pulled far into the box was
    // probably not this surface.
    weight *= 1.0 - 0.25 * clamp(speed / 3.0, 0.0, 1.0);
    // Reading between two pixels blurs (each frame's, kept for ten): the further from a
    // pixel centre the history is read, the less of it is trusted.
    const vec2 between = abs(fract(past * params.sizes.xy) - 0.5);
    weight *= 1.0 - 0.6 * clamp(max(between.x, between.y) * 3.0, 0.0, 1.0);
    weight *= 1.0 - 0.5 * clamp(outside - 1.0, 0.0, 1.0);
    const vec3 colour = mix(filtered / filtered_weight, from_ycocg(h), weight);
    imageStore(uResult, p, vec4(expand(colour), mix(filtered_alpha / filtered_weight, history.a, weight)));
}
