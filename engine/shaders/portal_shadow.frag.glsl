#version 450 core
#extension GL_GOOGLE_include_directive : require
#include "kin_portal_sun.inc.glsl"

// A first-hop solar path stops at another portal. The two mesh passes clip out
// their own opening, so it cannot shadow the beam that is being transported.
layout(set = 3, binding = 0) uniform PortalShadow {
    mat4 to_world;
    vec4 clip;
    vec4 info; // portal count, map resolution
    KinPortalSun portal[8];
} draw;

void main() {
    const vec2 uv = gl_FragCoord.xy / draw.info.y;
    const vec2 xy = vec2(2.0 * uv.x - 1.0, 1.0 - 2.0 * uv.y);
    const vec4 a = draw.to_world * vec4(xy, 1.0, 1.0);
    const vec4 b = draw.to_world * vec4(xy, 0.0, 1.0);
    const vec3 origin = a.xyz / a.w;
    const vec3 way = b.xyz / b.w - origin;
    float nearest = -1.0;
    for (int i = 0; i < int(draw.info.x); ++i) {
        const KinPortalSun p = draw.portal[i];
        const float toward = dot(p.plane.xyz, way);
        if (abs(toward) < 1e-6) continue;
        const float t = -dot(p.plane, vec4(origin, 1.0)) / toward;
        if (t < 0.0 || t > 1.0) continue;
        const vec4 at = vec4(origin + t * way, 1.0);
        if (dot(draw.clip, at) < 0.0 || abs(dot(p.x, at)) >= p.direction.w ||
            abs(dot(p.y, at)) >= p.aperture.x) continue;
        nearest = max(nearest, 1.0 - t);
    }
    if (nearest < 0.0) discard;
    gl_FragDepth = nearest;
}
