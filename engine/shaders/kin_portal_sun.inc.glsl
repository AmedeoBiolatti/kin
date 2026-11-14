#ifndef KIN_PORTAL_SUN
#define KIN_PORTAL_SUN
struct KinPortalSun {
    mat4 shadow_matrix;
    vec4 plane;
    vec4 x;
    vec4 y;
    vec4 direction; // xyz: toward the transported sun, w: half width
    vec4 aperture; // half height, depth bias, normal offset (world), layer (-1: unshadowed; -2: no solar path)
};

bool kin_portal_crossing(KinPortalSun p, vec3 world, vec3 way, bool front_only) {
    const float side = dot(p.plane, vec4(world, 1.0));
    const float toward = dot(p.plane.xyz, way);
    if (abs(toward) < 1e-6 || side * toward >= 0.0 || (front_only && side <= 1e-4)) {
        return false;
    }
    const vec4 at = vec4(world - way * (side / toward), 1.0);
    return abs(dot(p.x, at)) < p.direction.w && abs(dot(p.y, at)) < p.aperture.x;
}

float kin_portal_sunlight(KinPortalSun p, vec3 world, vec3 offset_normal, sampler2DArrayShadow depth,
                          float texel, float strength, bool shadows) {
    if (p.aperture.w < -1.5 || !kin_portal_crossing(p, world, p.direction.xyz, true)) {
        return 0.0;
    }
    if (!shadows || p.aperture.w < 0.0) {
        return 1.0;
    }
    const vec4 clip = p.shadow_matrix * vec4(world + offset_normal * p.aperture.z, 1.0);
    const vec3 ndc = clip.xyz / clip.w;
    const vec2 uv = vec2(0.5 + 0.5 * ndc.x, 0.5 - 0.5 * ndc.y);
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))) || ndc.z < 0.0 || ndc.z > 1.0) {
        return 0.0; // the configured shadow area did not cover this receiver
    }
    float seen = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            const vec2 tap = clamp(uv + vec2(x, y) * texel, vec2(0.5 * texel), vec2(1.0 - 0.5 * texel));
            seen += texture(depth, vec4(tap, p.aperture.w, ndc.z + p.aperture.y));
        }
    }
    return mix(1.0, seen / 9.0, strength);
}
#endif
