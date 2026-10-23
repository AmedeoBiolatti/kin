// What the pre-pass fragment shaders share (prepass*.frag).

// The world normal on the side the viewer sees, from the vertex normal, or (a flat-shaded mesh with clusters has
// none: zero) from the derivatives of the world position, which every pre-pass fragment shader declares as vWorld.
vec3 kin_prepass_normal() {
    const vec3 dp1 = dFdx(vWorld), dp2 = dFdy(vWorld);
    if (dot(vNormal, vNormal) < 1e-20) {
        const vec3 n = cross(dp1, dp2);
        const float l = length(n);
        return l > 0.0 ? n * (-1.0 / l) : vec3(0.0, 1.0, 0.0);
    }
    return normalize(vNormal) * (gl_FrontFacing ? 1.0 : -1.0);
}

// A unit normal in two floats (octahedral); gtao.comp and contact.comp decode it.
vec2 octahedral(vec3 n) {
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    const vec2 fold = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return n.z >= 0.0 ? n.xy : fold;
}

#include "kin_velocity.inc.glsl"
