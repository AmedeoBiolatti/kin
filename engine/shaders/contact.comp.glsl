#version 450 core
// kin's contact shadows (Scene3D::contact_shadows): from each pixel, a short
// march toward the sun through the pre-pass's depth. Where the depth buffer
// holds something nearer the camera than the ray, but not by more than the
// thickness (a thing further in front is taken to be a thin occluder the ray
// passes behind), the sun is blocked. The start of the march is jittered a
// different amount every pixel, so 16 to 24 steps do the work of many; the
// blur that follows (ao_blur.comp, depth-aware) hides the noise. Fades out
// over the end of the march, and where the hit is near the screen's edge
// (past which nothing is known). Writes the share of the sun that gets
// through (1: all).
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D uDepth;   // reverse Z, 0 is nothing
layout(set = 0, binding = 1) uniform sampler2D uNormals; // world normals, octahedral
layout(set = 1, binding = 0, r8) uniform writeonly image2D uResult;
layout(set = 2, binding = 0) uniform Params {
    mat4 view;           // world to the camera's (its rotation turns the normals)
    mat4 projection;
    // The inverse projection, without its zeroes (as in gtao.comp).
    vec4 unproject_xy;   // a, b, c, d
    vec4 unproject_zw;   // e, f, g, h
    vec4 sizes;          // xy: the result's size (the depth's: the same), zw: the same
    vec4 sun;            // xyz: toward the sun in view space, w: 1 if orthographic
    vec4 shape;          // x: distance (units), y: thickness (units), z: intensity, w: steps
} params;

// Interleaved gradient noise (Jimenez): smooth under a blur, cheap.
float noise(vec2 p) {
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

vec3 decode_normal(vec2 e) {
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) {
        n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    }
    return normalize(n);
}

vec3 view_position(ivec2 p, float depth) {
    const vec2 ndc = vec2(p.x + 0.5, params.sizes.w - p.y - 0.5) * (2.0 / params.sizes.zw) - 1.0;
    const float w = params.unproject_zw.z * depth + params.unproject_zw.w;
    return vec3(params.unproject_xy.x * ndc.x + params.unproject_xy.z, params.unproject_xy.y * ndc.y + params.unproject_xy.w,
                params.unproject_zw.x * depth + params.unproject_zw.y) / w;
}

float view_z(float depth) {
    return (params.unproject_zw.x * depth + params.unproject_zw.y) / (params.unproject_zw.z * depth + params.unproject_zw.w);
}

void main() {
    const ivec2 xy = ivec2(gl_GlobalInvocationID.xy);
    const ivec2 size = ivec2(params.sizes.xy);
    if (xy.x >= size.x || xy.y >= size.y) {
        return;
    }
    const float depth = texelFetch(uDepth, xy, 0).r;
    if (depth <= 0.0) {
        imageStore(uResult, xy, vec4(1.0));
        return;
    }
    const bool ortho = params.sun.w > 0.5;
    const vec3 n = normalize(mat3(params.view) * decode_normal(texelFetch(uNormals, xy, 0).rg));
    const vec3 l = params.sun.xyz;
    // A surface turned from the sun has no sun to lose (the shading's own n.l
    // already zeroes it); one nearly edge on would only march along itself.
    if (dot(n, l) <= 0.05) {
        imageStore(uResult, xy, vec4(1.0));
        return;
    }
    vec3 p = view_position(xy, depth);
    // How much a pixel spans at the surface: the march's tolerance is in
    // pixels, so a far floor and a near one are treated alike.
    const float pixel = 2.0 * (ortho ? 1.0 : max(-p.z, 1e-3)) / (params.projection[1][1] * params.sizes.w);
    const vec3 v = ortho ? vec3(0.0, 0.0, 1.0) : normalize(-p);
    // Off the surface a little, more where it is seen at a slant (a pixel then
    // covers more depth): its own depth must not read as an occluder.
    p += n * pixel * (1.0 + 2.0 * (1.0 - clamp(dot(n, v), 0.0, 1.0)));

    const float distance_to_sun = params.shape.x;
    const float thickness = params.shape.y;
    const int steps = int(params.shape.w);
    const float jitter = noise(vec2(xy));
    float blocked = 0.0;
    for (int i = 0; i < steps; ++i) {
        const float s = (float(i) + jitter) / float(steps);
        const float t = s * distance_to_sun;
        const vec3 r = p + l * t;
        const vec4 clip = params.projection * vec4(r, 1.0);
        if (clip.w <= 1e-4) {
            break; // behind the camera
        }
        const vec2 ndc = clip.xy / clip.w;
        const vec2 uv = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
        if (uv.x < 0.0 || uv.y < 0.0 || uv.x >= 1.0 || uv.y >= 1.0) {
            break; // off the screen: nothing known beyond
        }
        const ivec2 q = min(ivec2(uv * params.sizes.zw), ivec2(params.sizes.zw) - 1);
        const float d = texelFetch(uDepth, q, 0).r;
        if (d <= 0.0) {
            continue; // the sky
        }
        // View space looks down -z: nearer the camera is a greater z.
        const float dz = view_z(d) - r.z;
        const float tolerance = pixel * (0.5 + 1.5 * (1.0 - clamp(dot(n, v), 0.0, 1.0)));
        if (dz > tolerance) {
            // Soft at the thickness, so a hit does not flicker in and out of it.
            float hit = 1.0 - smoothstep(0.75 * thickness, thickness, dz);
            hit *= 1.0 - smoothstep(0.6, 1.0, s);                                      // the march's end
            const vec2 edge = min(uv, 1.0 - uv);
            hit *= smoothstep(0.0, 0.05, min(edge.x, edge.y));                          // the screen's edge
            blocked = max(blocked, hit);
        }
    }
    imageStore(uResult, xy, vec4(1.0 - clamp(params.shape.z, 0.0, 1.0) * blocked));
}
