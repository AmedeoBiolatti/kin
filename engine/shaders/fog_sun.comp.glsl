#version 450 core
// The fog's sunlight through the sun's shadows (Fog3D::sun_shadows): for each texel at half the view's size, the share
// of the fog between the camera and what the pre-pass drew there (to the end of the height fog for the sky) that the
// sun reaches. The fog brightens toward the sun by its in-scattering only where its air is in sunlight: in a street's
// shade the air between the camera and a wall is in the buildings' shadow, and lit like the rest of the fog it veiled
// the shade. The ray is walked in `steps` stretches, closer together near the camera (where the fog's change across a
// pixel is largest), each stretch's share of the opacity (kin_fog_optical_depth, the closed integral) weighed by
// whether a point in it (moved along the stretch per pixel and per frame: temporal anti-aliasing averages the steps)
// sees the sun, from the cascade of the sun's shadow map whose slice of the view holds it; past the last cascade, and
// outside its map, the air is lit. The main pass, the sky pass and the particles then leave the sun's in-scattering out
// of their fog, and fog_sun_apply.frag adds it times the share, once a pixel; at 1 everywhere it is the fog without this.
layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0) uniform sampler2D uDepth;               // the pre-pass's, reverse Z: 0 is nothing
layout(set = 0, binding = 1) uniform sampler2DArrayShadow uShadow;   // the sun's, a layer a cascade, compared
layout(set = 1, binding = 0, rgba16f) uniform writeonly image2D uShare; // x: the lit share, y: the opacity, z: the
                                                                        // distance along the ray (0: the sky)

#include "kin_fog.inc.glsl"

layout(std140, set = 2, binding = 0) uniform Params {
    mat4 to_world;     // the camera's space to the world's
    vec4 unproject_xy; // the inverse projection, as gtao.comp's
    vec4 unproject_zw;
    vec4 sizes;        // xy: the result's size, zw: the depth's
    vec4 eye;          // xyz: the camera (world)
    vec4 forward;      // xyz: the way it looks (world), along which the cascades' ends are measured; w: the sun's
                       // shadows' strength
    KinFog fog;
    mat4 shadow_matrix[4]; // the world to each cascade's map
    vec4 cascade_end;      // the view depth where each cascade ends
    vec4 cascade_bias;     // each cascade's depth bias
    vec4 shadow;           // x: cascades, y: the view depth the shadows reach, z: the walk's offset this frame, w: steps
} params;

#include "kin_depth.inc.glsl"

// Whether the sun reaches `p` (view depth `depth`): the cascade whose slice holds it, one filtered comparison.
float sunlit(vec3 p, float depth) {
    const int count = int(params.shadow.x);
    int k = 0;
    for (int i = 0; i < count - 1; ++i) {
        k += depth > params.cascade_end[i] ? 1 : 0;
    }
    const vec4 clip = params.shadow_matrix[k] * vec4(p, 1.0);
    const vec3 ndc = clip.xyz / clip.w;
    const vec2 uv = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0 || ndc.z < 0.0 || ndc.z > 1.0) {
        return 1.0;
    }
    // (A shadow of strength below 1 lets that share of the sun through: Lights3D::sun_shadow_strength.)
    return mix(1.0, texture(uShadow, vec4(uv, float(k), ndc.z + params.cascade_bias[k])), params.forward.w);
}

float opacity_at(KinFog f, vec3 dir, float t) {
    return kin_fog_opacity(f, kin_fog_optical_depth(f, params.eye.xyz, dir, t, false));
}

void main() {
    const ivec2 xy = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(xy, ivec2(params.sizes.xy)))) {
        return;
    }
    // The nearest surface of the texel's 2 x 2 pixels, as gi_resolve.comp's (what the main pass compares with).
    const ivec2 last = ivec2(params.sizes.zw) - 1;
    ivec2 at = min(xy * 2, last);
    float depth = texelFetch(uDepth, at, 0).r;
    for (int i = 1; i < 4; ++i) {
        const ivec2 q = min(xy * 2 + ivec2(i & 1, i >> 1), last);
        const float d = texelFetch(uDepth, q, 0).r;
        if (d > depth) {
            at = q;
            depth = d;
        }
    }
    const bool sky = depth <= 0.0;
    // The ray: toward the surface, or toward a far point for the sky (the far plane is at infinity).
    const vec3 seen = view_position(at, sky ? 1e-7 : depth);
    const vec3 to = (params.to_world * vec4(seen, 0.0)).xyz;
    const float dist = length(to);
    const vec3 dir = to / max(dist, 1e-9);
    // The sky's fog is the height fog's alone (kin_fog_optical_depth's `infinite`), as the sky pass has it.
    KinFog f = params.fog;
    if (sky) {
        f.density.x = 0.0;
    }
    const float total = sky ? kin_fog_opacity(f, kin_fog_optical_depth(f, params.eye.xyz, dir, 0.0, true))
                            : opacity_at(f, dir, dist);
    if (total <= 1e-5) {
        imageStore(uShare, xy, vec4(1.0, total, sky ? 0.0 : dist, 1.0));
        return;
    }
    // The walk: from the fog's start to the surface, or to where the shadows end (past it the air is lit).
    const float along = max(dot(dir, params.forward.xyz), 1e-4);
    const float reach = params.shadow.y / along;
    const float t0 = f.limits.x;
    const float t1 = sky ? reach : min(dist, reach);
    // (Nothing before the fog's start: its opacity there is 0.)
    float lit = 0.0, before = 0.0;
    if (t1 > t0) {
        const int steps = int(params.shadow.w);
        const float offset = fract(noise(vec2(xy)) + params.shadow.z);
        float s = t0;
        for (int i = 0; i < steps; ++i) {
            const float u = float(i + 1) / float(steps);
            const float next = t0 + (t1 - t0) * u * u;
            const float after = opacity_at(f, dir, next);
            const float t = mix(s, next, offset);
            // (A stretch with next to no fog is not looked up.)
            if (after - before > 1e-4) {
                lit += (after - before) * sunlit(params.eye.xyz + dir * t, t * along);
            }
            s = next;
            before = after;
        }
    }
    // What lies past the walk is lit.
    lit += max(total - before, 0.0);
    imageStore(uShare, xy, vec4(clamp(lit / total, 0.0, 1.0), total, sky ? 0.0 : dist, 1.0));
}
