#version 450 core
// kin's screen-space reflections (Scene3D::ssr): from each texel of a half-size grid over the view, the mirror
// reflection of the pre-pass's surface there, marched through the pre-pass's depth. The march's steps grow with the
// distance (finer near the surface, where most hits are); where the ray
// passes behind what the depth holds, by less than the thickness, it has hit, and a few halvings find where (the start
// differs pixel by pixel with temporal anti-aliasing, which averages it, and not without, where it would show). Writes
// how far the hit is, not its colour: the main pass follows its own reflection (its normal map's) that far and reads the
// previous frame's picture there, blurred by its roughness (kin_ssr_reflection, ssr_pyramid.comp). The share to use
// fades toward the screen's edges, toward the march's end and for rays that come back toward the camera (what they meet
// is mostly off the screen). Three words a texel: the distance, the share (a half) and the view depth of the surface
// traced, so that the main pass takes only the texels of its own surface.
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D uDepth;   // the pre-pass's, reverse Z: 0 is nothing
layout(set = 0, binding = 1) uniform sampler2D uNormals; // world normals, octahedral
layout(std430, set = 1, binding = 0) writeonly buffer Result {
    uint result[];
};
layout(set = 2, binding = 0) uniform Params {
    mat4 view;        // world to the camera's (its rotation turns the normals)
    mat4 projection;  // the camera's, as the pre-pass drew with it
    // The inverse projection, without its zeroes (as in gtao.comp).
    vec4 unproject_xy; // a, b, c, d
    vec4 unproject_zw; // e, f, g, h
    vec4 sizes;        // xy: the grid's (half the view's), zw: the depth's
    vec4 shape;        // x: thickness, y: the farthest a ray goes, z: steps, w: intensity
    vec4 colour;       // z: 1 to start each pixel's march at a different point (temporal anti-aliasing averages it)
} params;

#include "kin_depth.inc.glsl"

float view_z(float depth) {
    return (params.unproject_zw.x * depth + params.unproject_zw.y) / (params.unproject_zw.z * depth + params.unproject_zw.w);
}

// Where a point of the camera's space shows on the depth (uv), and its clip w.
vec3 to_screen(vec3 q) {
    const vec4 clip = params.projection * vec4(q, 1.0);
    const vec2 ndc = clip.xy / clip.w;
    return vec3(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5, clip.w);
}

void write(uint at, float distance, float share, float depth) {
    result[at * 3u] = floatBitsToUint(distance);
    result[at * 3u + 1u] = packHalf2x16(vec2(share, 0.0));
    result[at * 3u + 2u] = floatBitsToUint(depth);
}

void main() {
    const ivec2 xy = ivec2(gl_GlobalInvocationID.xy);
    const ivec2 size = ivec2(params.sizes.xy);
    if (xy.x >= size.x || xy.y >= size.y) {
        return;
    }
    const uint at = uint(xy.y * size.x + xy.x);
    const ivec2 full = ivec2(params.sizes.zw);
    const ivec2 pixel = min(xy * 2, full - 1);
    const float depth = texelFetch(uDepth, pixel, 0).r;
    if (depth <= 0.0) {
        write(at, 0.0, 0.0, 0.0);
        return;
    }
    vec3 p = view_position(pixel, depth);
    const vec3 n = normalize(mat3(params.view) * decode_normal(texelFetch(uNormals, pixel, 0).rg));
    const vec3 v = normalize(-p);
    const vec3 r = reflect(-v, n);
    // A ray coming back toward the camera meets what is behind it, off the screen: fade those out.
    const float facing = 1.0 - smoothstep(0.25, 0.75, r.z);
    if (facing <= 0.0) {
        write(at, 0.0, 0.0, -p.z);
        return;
    }
    // Off the surface by about a pixel, so its own depth does not read as a hit.
    const float pixel_size = 2.0 * max(-p.z, 1e-3) / (params.projection[1][1] * params.sizes.w);
    const vec3 start = p + n * pixel_size * 1.5;
    const float thickness = params.shape.x;
    const float far = params.shape.y;
    const int steps = int(params.shape.z);
    const float jitter = params.colour.z > 0.5 ? noise(vec2(xy)) : 0.5;
    float before = 0.0, hit = -1.0;
    for (int i = 1; i <= steps; ++i) {
        const float s = (float(i) - 1.0 + jitter) / float(steps);
        const float t = s * s * far;
        const vec3 q = start + r * t;
        const vec3 at_screen = to_screen(q);
        if (at_screen.z <= 1e-4 || any(lessThan(at_screen.xy, vec2(0.0))) || any(greaterThanEqual(at_screen.xy, vec2(1.0))))
            break; // behind the camera, or off the screen: nothing known
        const float scene = texelFetch(uDepth, min(ivec2(at_screen.xy * params.sizes.zw), full - 1), 0).r;
        if (scene > 0.0) {
            const float behind = view_z(scene) - q.z; // how far the ray is behind the surface there
            if (behind > 0.0 && behind < thickness + t * 0.02) {
                hit = t;
                break;
            }
        }
        before = t;
    }
    if (hit < 0.0) {
        write(at, 0.0, 0.0, -p.z);
        return;
    }
    // Where between the last step in front and the first behind.
    float lo = before, hi = hit;
    for (int k = 0; k < 5; ++k) {
        const float mid = 0.5 * (lo + hi);
        const vec3 at_screen = to_screen(start + r * mid);
        const float scene = texelFetch(uDepth, min(ivec2(at_screen.xy * params.sizes.zw), full - 1), 0).r;
        if (scene > 0.0 && view_z(scene) - (start + r * mid).z > 0.0)
            hi = mid;
        else
            lo = mid;
    }
    const vec3 found = to_screen(start + r * hi);
    const ivec2 hit_pixel = min(ivec2(found.xy * params.sizes.zw), full - 1);
    const float hit_depth = texelFetch(uDepth, hit_pixel, 0).r;
    if (hit_depth <= 0.0) {
        write(at, 0.0, 0.0, -p.z);
        return;
    }
    const vec2 edge_now = min(found.xy, 1.0 - found.xy);
    const float edges = smoothstep(0.0, 0.08, min(edge_now.x, edge_now.y));
    const float reach = 1.0 - smoothstep(0.7, 1.0, hi / far);
    // (from the surface itself, not the march's start off it)
    write(at, length(start + r * hi - p), clamp(facing * edges * reach * params.shape.w, 0.0, 1.0), -p.z);
}
