// kin's temporal anti-aliasing (Scene3D::taa): this frame's picture, drawn a
// fraction of a pixel off (the jitter), blended into the picture of the frames
// before. Each pixel looks for where its point was last frame (the velocity of
// the nearest surface round it: edge_velocity), reads the history there with a
// Catmull-Rom filter, clips it to the colours near the pixel in this frame (a
// 3 x 3 box in YCoCg, from the mean and spread, inside the neighbours' own
// range: history that does not agree with what is there now is a ghost) and
// mixes. The history weight falls to nothing where there is none (off the
// screen, a new mesh, the first frame), where the surface the history shows
// moved differently from this one (the velocity the last frame's resolve took
// at that spot is compared with this frame's: something that left a spot
// uncovered, or arrived on it), and a little with speed and with how far the
// clip moved it.
// Light is compressed (c / (1 + max c)) for the clip and the mix, so a very
// bright pixel does not outweigh its neighbours or flicker. A still pixel (the
// picture moving under half a pixel a frame) keeps more of its history, with a
// wider clip: detail finer than a pixel (a menu's print, a railing's bars),
// which one frame's few samples miss in some of the jitter's places, is held
// steady there instead of being pulled toward each frame's samples, which
// shimmered with the jitter's period. A second mode (params.z) sharpens a
// finished picture: a small local-contrast boost kept inside the neighbours'
// range, within surfaces (not where the depth bent in the last frames).
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0)
    uniform sampler2D uCurrent; // this frame, or in sharpen mode the result so far; linear, premultiplied
layout(set = 0, binding = 1) uniform sampler2D uHistory;
layout(set = 0, binding = 2)
    uniform sampler2D uMotion; // BA: contributing-layer motion; R > 1: coverage; G: opaque independent motion
layout(set = 0, binding = 3) uniform sampler2D uPastMotion; // the same, of the last frame
layout(set = 0, binding = 4) uniform sampler2D uDepth;      // the main pass's depth (reverse Z) where depth.x is 1
layout(set = 0, binding = 5) uniform usampler2D uPastDilated; // the velocities the last frame's resolve took
layout(set = 1, binding = 0, rgba16f) uniform writeonly image2D uResult;
layout(set = 1, binding = 1, r32ui) uniform writeonly uimage2D uDilated; // this frame's (pack_dilated), for the next
#ifdef KIN_TEMPORAL_UPSCALE
layout(set = 0, binding = 6) uniform sampler2D uPastConfidence;
layout(set = 1, binding = 2, r16f) uniform writeonly image2D uConfidence;
#endif
layout(set = 2, binding = 0) uniform Params {
    vec4 sizes;  // the picture's width and height, and 1 over them
    vec4 params; // x: history weight, y: 1 when there is no history, z: 1 to sharpen instead, w: sharpness
    vec4 jitter; // xy: jitter, z: coverage present (2: frozen jitter), w: physical coverage changed (excluding jitter)
    vec4 depth;  // x: 1 when uDepth is the main pass's depth; y: where the overlay's slice of it starts (2: none);
                 // z: 1 when the sharpening follows (the resolve keeps where the depth bent, for it)
    vec4 still;  // x: native still weight; y: still hold enabled; z: scaled-view history weight cap
    vec4 input_size; // shading extent and reciprocals; sizes is the display/history extent
}
params;

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

// The velocities kept for the next frame (uDilated): two halves, each with its two lowest bits (a relative 0.4 % of
// the velocity, far under the half pixel the next frame's mismatch test goes by) given to how recently the depth bent
// at the pixel: 15 where it bends now, one less each frame since (followed as the picture moves), for the sharpening.
uint pack_dilated(vec2 velocity, uint bent) {
    return (packHalf2x16(velocity) & ~0x00030003u) | (bent & 3u) | ((bent >> 2u) << 16u);
}
vec2 dilated_velocity(uint packed) {
    return unpackHalf2x16(packed & ~0x00030003u);
}
uint dilated_bent(uint packed) {
    return (packed & 3u) | (((packed >> 16u) & 3u) << 2u);
}

ivec2 input_extent() {
#ifdef KIN_TEMPORAL_UPSCALE
    return ivec2(params.input_size.xy);
#else
    return ivec2(params.sizes.xy);
#endif
}

vec4 fetch(ivec2 p) {
    const ivec2 size = input_extent();
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
    const vec4 filtered = sum / total;
    // Returned as it is when not finite, for main() to drop: max() may make a NaN 0 (black history, blended in).
    if (any(isnan(filtered)) || any(isinf(filtered))) {
        return filtered;
    }
    return max(filtered, vec4(0.0));
}

void sharpen(ivec2 p) {
    const vec4 centre = fetch(p);
    const vec3 c = compress(centre.rgb);
    const vec3 n = compress(fetch(p + ivec2(0, -1)).rgb), s = compress(fetch(p + ivec2(0, 1)).rgb);
    const vec3 e = compress(fetch(p + ivec2(1, 0)).rgb), w = compress(fetch(p + ivec2(-1, 0)).rgb);
    const vec3 low = min(c, min(min(n, s), min(e, w))), high = max(c, max(max(n, s), max(e, w)));
    const vec3 blur = (n + s + e + w) * 0.25;
    // Not at a silhouette, whose blended pixels (the resolve's anti-aliasing) the boost would push to either side,
    // back into steps: texture detail within a surface is sharpened, the outline of one surface against another left
    // as the resolve gave it. Where the depth bent (depth_bends) in any of the last eight frames, not only this one:
    // the jitter moves a thing thinner than a pixel (a railing's bar, a cable) in and out of one frame's depth, while
    // the resolved picture holds it in every frame, so going by this frame's depth alone sharpened its pixels in some
    // of the jitter's places and not in the others: a flicker with the jitter's period. Back to the full boost over
    // eight more frames after an edge has gone.
    float boost = params.params.w * 1.5;
    if (params.depth.x > 0.5) {
        const uint bent = dilated_bent(texelFetch(uPastDilated, p, 0).r); // this frame's, in this mode
        boost *= clamp(1.0 - float(bent) / 8.0, 0.0, 1.0);
    }
    const vec3 sharp = clamp(c + (c - blur) * boost, low, high);
    imageStore(uResult, p, vec4(expand(sharp), centre.a));
}

// Whether the depth bends across the pixel's cross. The depth (reverse Z, its far plane at infinity) runs straight
// across any plane on the screen, however slanted (a floor to the horizon), so where it bends there is an edge: a
// silhouette, the sky beside a roof, a crease.
bool depth_bends(ivec2 p) {
    const ivec2 last = input_extent() - 1;
    const float d = texelFetch(uDepth, p, 0).r;
    const float dn = texelFetch(uDepth, clamp(p + ivec2(0, -1), ivec2(0), last), 0).r;
    const float ds = texelFetch(uDepth, clamp(p + ivec2(0, 1), ivec2(0), last), 0).r;
    const float de = texelFetch(uDepth, clamp(p + ivec2(1, 0), ivec2(0), last), 0).r;
    const float dw = texelFetch(uDepth, clamp(p + ivec2(-1, 0), ivec2(0), last), 0).r;
    const float bend = max(abs(de + dw - 2.0 * d), abs(dn + ds - 2.0 * d));
    return bend > 0.01 * max(d, max(max(dn, ds), max(de, dw)));
}

// R: mesh=2, new mesh=3, matched particle=4+irradiance, new
// particle=6+irradiance, relit particle=8+irradiance. G retains the
// opaque independent-motion marker through every transparent layer.
bool fresh(float coverage) {
    return (coverage > 2.5 && coverage < 3.5) || (coverage > 5.5 && coverage < 7.5);
}
float changed_light(ivec2 pixel, vec4 motion) {
    if (motion.r < 7.5)
        return 0.0;
    if (params.jitter.z > 1.5)
        return 1.0;
    const vec2 past = (vec2(pixel) + .5) * params.input_size.zw - motion.ba;
    const vec4 before = textureLod(uPastMotion, past, 0.0);
    return before.r > 3.5 ? abs(fract(before.r) - fract(motion.r)) : 0.0;
}

// The velocity a pixel's history is found by. Each frame's jitter puts a pixel at an edge on one surface one frame
// and on the other the next, so its own velocity flickers between the two: where the nearer surface moves (a
// character over a wall), the edge's history is found in the wrong place or taken for a surface that moved
// differently and dropped, and a moving edge keeps little of its anti-aliasing. With the main pass's depth, the
// velocity of the nearest surface round the pixel (one clearly nearer: within a fiftieth of its depth is the same
// surface) is the edge's on both sides of it. The world round the overlay keeps its own: the gun is fixed to the
// screen while the world behind it turns with the camera, and given the gun's velocity the world's edge would smear.
vec2 edge_velocity(ivec2 p) {
    const vec2 own = texelFetch(uMotion, p, 0).ba;
    if (params.depth.x < 0.5) {
        return own;
    }
    const ivec2 last = input_extent() - 1;
    const float own_depth = texelFetch(uDepth, p, 0).r;
    const bool world = own_depth < params.depth.y;
    float nearest = own_depth * 1.02 + 1e-7; // reverse Z: nearer is larger
    vec2 chosen = own;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            const ivec2 q = clamp(p + ivec2(x, y), ivec2(0), last);
            const float d = texelFetch(uDepth, q, 0).r;
            if (d > nearest && !(world && d >= params.depth.y)) {
                nearest = d;
                chosen = texelFetch(uMotion, q, 0).ba;
            }
        }
    }
    return chosen;
}

void main() {
    const ivec2 output_pixel = ivec2(gl_GlobalInvocationID.xy);
    if (output_pixel.x >= int(params.sizes.x) || output_pixel.y >= int(params.sizes.y)) {
        return;
    }
    if (params.params.z > 0.5) {
        sharpen(output_pixel);
        return;
    }
#ifdef KIN_TEMPORAL_UPSCALE
    // Output pixel centres projected onto the jittered shading grid. Motion/depth and current colour stay
    // at shading resolution; history and the previous dilated velocity stay at output resolution.
    const vec2 sample_position = (vec2(output_pixel) + .5) * params.input_size.xy * params.sizes.zw - .5;
    const ivec2 p = clamp(ivec2(floor(sample_position + .5)), ivec2(0), input_extent() - 1);
#else
    const ivec2 p = output_pixel;
#endif
    const vec4 current = fetch(p);
    const vec2 velocity = edge_velocity(p);
    const vec2 uv = (vec2(output_pixel) + 0.5) * params.sizes.zw;
    const vec2 past = uv - velocity;
    const ivec2 size = ivec2(params.sizes.xy);
    // The last frame's pixel this one's history is read from.
    const ivec2 nearest = clamp(ivec2(floor(past * params.sizes.xy)), ivec2(0), size - 1);
    const bool no_history = params.params.y > 0.5 || any(lessThan(past, vec2(0.0))) ||
                            any(greaterThan(past, vec2(1.0))) || any(isnan(velocity));
    uint bent = 0u;
    if (params.depth.z > 0.5) {
        const uint before = no_history ? 0u : dilated_bent(texelFetch(uPastDilated, nearest, 0).r);
        bent = depth_bends(p) ? 15u : max(before, 1u) - 1u;
    }
    imageStore(uDilated, output_pixel, uvec4(pack_dilated(velocity, bent), 0u, 0u, 0u));
#ifndef KIN_TEMPORAL_UPSCALE
    if (no_history) {
        imageStore(uResult, output_pixel, current);
        return;
    }
#endif

    // The neighbourhood: mean and spread of the 3 x 3 around the pixel, and its range.
    vec3 sum = vec3(0.0), sum_squares = vec3(0.0), low = vec3(1e9), high = vec3(-1e9);
    float alpha_low = 1e9, alpha_high = -1e9;
    // This frame's picture at the pixel's own centre: each of the 3 x 3 is a sample taken a
    // little off the pixel grid (the jitter), so the pixel's value is read from them by how
    // near the centre each fell (a Gaussian, as Karis' filter for TAA), not taken as its
    // own sample, which sits anywhere in the pixel from frame to frame and would
    // pull the picture a fraction of a pixel to wherever the last few jitters were.
    // With no jitter there is nothing to correct: the pixel's own sample.
#ifdef KIN_TEMPORAL_UPSCALE
    const bool jittered = true;
#else
    const bool jittered = dot(params.jitter.xy, params.jitter.xy) > 0.0;
#endif
    vec3 filtered = vec3(0.0);
    float filtered_alpha = 0.0, filtered_weight = 0.0;
    bool transient = false;
    float lighting_change = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            const vec4 c = fetch(p + ivec2(x, y));
            if (params.jitter.z > 0.5) {
                const ivec2 pixel = clamp(p + ivec2(x, y), ivec2(0), input_extent() - 1);
                const vec4 motion = texelFetch(uMotion, pixel, 0);
                transient = transient || fresh(motion.r) || (motion.r > 1.5 && motion.g > 2.0);
                lighting_change = max(lighting_change, changed_light(pixel, motion));
            }
            const vec3 compressed = compress(c.rgb);
#ifdef KIN_TEMPORAL_UPSCALE
            // Measure the reconstruction footprint in output pixels. The native input-pixel footprint
            // grows with the upscale ratio and blurs away detail before history can accumulate it.
            const vec2 away = (vec2(p + ivec2(x, y)) - sample_position - params.jitter.xy) *
                              params.sizes.xy * params.input_size.zw;
#else
            const vec2 away = vec2(x, y) - params.jitter.xy;
#endif
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
#ifdef KIN_TEMPORAL_UPSCALE
    // A reset is a spatially reconstructed frame, with no uninitialised history or velocity reads.
    if (no_history) {
        imageStore(uConfidence, output_pixel, vec4(1.0));
        imageStore(uResult, output_pixel, vec4(expand(filtered / filtered_weight), filtered_alpha / filtered_weight));
        return;
    }
#endif
    const float coverage = texelFetch(uMotion, p, 0).r;
    if (params.jitter.z > .5 && coverage > 7.5) {
        // Reconstruction and Catmull-Rom can carry a shadow change from just
        // beyond the 3x3 colour box. Cover both filters' complete footprint.
        for (int y = -2; y <= 2 && !transient; ++y)
            for (int x = -2; x <= 2; ++x) {
                const ivec2 pixel = clamp(p + ivec2(x, y), ivec2(0), input_extent() - 1);
                lighting_change = max(lighting_change, changed_light(pixel, texelFetch(uMotion, pixel, 0)));
            }
    }
    if (params.jitter.z > 0.5) {
        // Catmull-Rom history reads a 4x4 footprint, not just its nearest pixel.
        // Reject the old footprint as well: this is what removes the trail when
        // an emitter moves, fades out, wraps or disappears entirely.
        const ivec2 base = ivec2(floor(past * params.input_size.xy - 0.5));
        for (int y = -1; y <= 2 && !transient; ++y) {
            for (int x = -1; x <= 2; ++x) {
                const ivec2 pixel = clamp(base + ivec2(x, y), ivec2(0), input_extent() - 1);
                const vec4 old_motion = texelFetch(uPastMotion, pixel, 0);
                const float old_coverage = old_motion.r;
                transient = transient || fresh(old_coverage) ||
                            (old_coverage > 1.5 && old_motion.g > 2.0) ||
                            (params.jitter.w > .5 && texelFetch(uMotion, p, 0).r < 1.5 && old_coverage > 1.5);
            }
        }
        if (transient) {
#ifdef KIN_TEMPORAL_UPSCALE
            imageStore(uConfidence, output_pixel, vec4(0.0));
#endif
            // Keep spatial reconstruction of the jittered sample, without a
            // fictitious single velocity for several transparent layers.
            imageStore(uResult, output_pixel, vec4(expand(filtered / filtered_weight), filtered_alpha / filtered_weight));
            return;
        }
    }
    const vec3 mean = sum / 9.0;
    const vec3 spread = sqrt(max(sum_squares / 9.0 - mean * mean, vec3(0.0)));
    vec3 box_low = max(mean - 1.25 * spread, low), box_high = min(mean + 1.25 * spread, high);
    // A still pixel's box goes by the spread alone, further (2.5 of it), past what this frame's nine samples reached:
    // one frame's samples can all miss a detail finer than a pixel (the jitter puts them elsewhere), and clipped to
    // them its history was pulled to the background and back once a period. A flat neighbourhood (no spread) clips as
    // tightly as before, so a change of light on it is followed at once.
    const float speed = length(velocity * params.sizes.xy);
    // (Nothing of this where a still pixel keeps no more than a moving one: TemporalAA3D::still_blend at blend.)
    const float still = params.still.y > 0.5 ? 1.0 - clamp(speed / 0.5, 0.0, 1.0) : 0.0;
#ifdef KIN_TEMPORAL_UPSCALE
    // Jitter only sometimes supplies an input sample close to this output pixel. Preserve history's
    // detail when this frame poorly samples it; a close sample can update it more decisively.
    // Integral of exp(-6*r*r) is pi/6; multiply by shading-sample density in output pixels.
    // Normalise the update rate so close samples can replace more than an ordinary frame's share.
    const float average_weight = (3.14159265358979 / 6.0) *
                                 params.input_size.x * params.input_size.y * params.sizes.z * params.sizes.w;
    const float sample_mass = filtered_weight / average_weight;
    const float sample_trust = clamp(sample_mass, 0.0, 1.0);
    const float clip_hold = params.still.y > 0.5 ? 1.0 - sample_trust : 0.0;
#else
    const float clip_hold = still;
#endif
    box_low = mix(box_low, min(box_low, mean - 2.5 * spread), clip_hold);
    box_high = mix(box_high, max(box_high, mean + 2.5 * spread), clip_hold);
#ifdef KIN_TEMPORAL_UPSCALE
    // A sparse input neighbourhood can put the output's own valid sample in the minority. Clipping its
    // history away from that sample erases thin features even when reprojection agrees.
    const vec3 reconstruction = to_ycocg(filtered / filtered_weight);
    box_low = min(box_low, reconstruction);
    box_high = max(box_high, reconstruction);
#endif

    vec4 history = history_at(past);
    if (any(isnan(history)) || any(isinf(history))) {
#ifdef KIN_TEMPORAL_UPSCALE
        imageStore(uConfidence, output_pixel, vec4(0.0));
#endif
        imageStore(uResult, output_pixel, current);
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
    // (The last frame's velocity there as its resolve took it: a pixel at that edge took the edge's then, as this one
    // does now.)
    const vec2 then = dilated_velocity(texelFetch(uPastDilated, nearest, 0).r);
    const float mismatch_px = length((then - velocity) * params.sizes.xy);
    // A still pixel's history is read from its own place, exactly (no blur to accumulate): it keeps more of it (the
    // mean of the frames the history holds while they are few, then more than a moving pixel). The tests below still
    // drop it where it is of another surface or the light changed.
#ifdef KIN_TEMPORAL_UPSCALE
    // Warm up the reprojected pixel, not the whole view. Newly exposed pixels and rejected history have
    // few valid frames even when the rest of the view has been refining for a long time.
    const float previous_age = textureLod(uPastConfidence, past, 0.0).r;
    const float max_age = 1.0 / max(1.0 - params.still.z, .02) - 1.0;
    const float local_age = !isnan(previous_age) && !isinf(previous_age) ? clamp(previous_age, 0.0, max_age) : 0.0;
    const float local_weight = min(params.still.z, local_age / (local_age + 1.0));
    float weight = params.still.y > 0.5
                       ? clamp(1.0 - (1.0 - min(params.params.x, local_weight)) * sample_mass, 0.0, local_weight)
                       : params.params.x;
    float confidence_retention = min(1.0, .0005 / max(lighting_change, .0005));
#else
    float weight = mix(params.params.x, params.still.x, still);
#endif
    // Illumination changes lower confidence continuously. Small fire flicker
    // still accumulates coverage samples; larger moving-light/shadow changes
    // respond promptly over the full reconstruction footprint.
    weight *= min(1.0, .0005 / max(lighting_change, .0005));
    // With frozen sample positions there are no new edge samples to retain.
    // Match the current illumination exactly for deterministic inspection.
    if (params.jitter.z > 1.5 && lighting_change > 0.0) {
        weight = 0.0;
#ifdef KIN_TEMPORAL_UPSCALE
        confidence_retention = 0.0;
#endif
    }
    // A composite has several depths. Use the foremost contributing layer's
    // reprojection with bounded confidence, rather than disabling temporal AA
    // for every transparent whenever anything moves elsewhere in the scene.
    if (texelFetch(uMotion, p, 0).r > 1.5 && speed > .1)
        weight = min(weight, 0.6);
    weight *= 1.0 - clamp((mismatch_px - (0.35 + 0.05 * speed)) / 0.4, 0.0, 1.0);
    // A moving picture is read less exactly (every sub-pixel shift blurs it a
    // little), and a history that had to be pulled far into the box was
    // probably not this surface.
#ifndef KIN_TEMPORAL_UPSCALE
    weight *= 1.0 - 0.25 * clamp(speed / 3.0, 0.0, 1.0);
    // Reading between two pixels blurs (each frame's, kept for ten): the further from a
    // pixel centre the history is read, the less of it is trusted. Only a little: the
    // Catmull-Rom read keeps most of the detail, and a deeper cut (0.6 of it) left an
    // edge moving a fraction of a pixel a frame with few frames to smooth it (0.3 keeps
    // such an edge within a quarter of a still one, and a slow pan as sharp as before).
    const vec2 between = abs(fract(past * params.sizes.xy) - 0.5);
    weight *= 1.0 - 0.3 * clamp(max(between.x, between.y) * 3.0, 0.0, 1.0);
    // (Not for a still pixel, whose wider box already sets how far its history may be.)
    weight *= 1.0 - 0.5 * clamp(outside - 1.0, 0.0, 1.0) * (1.0 - still);
#endif
#ifdef KIN_TEMPORAL_UPSCALE
    // Reduce the next frame's age as well as this frame's colour weight on motion or lighting rejection.
    confidence_retention *= 1.0 - clamp((mismatch_px - (0.35 + 0.05 * speed)) / 0.4, 0.0, 1.0);
    imageStore(uConfidence, output_pixel, vec4(min(max_age, 1.0 + local_age * confidence_retention)));
#endif
    const vec3 colour = mix(filtered / filtered_weight, from_ycocg(h), weight);
    imageStore(uResult, output_pixel, vec4(expand(colour), mix(filtered_alpha / filtered_weight, history.a, weight)));
}
